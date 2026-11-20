"""Source-geometry selection, dimensions and associative placement."""
from functools import lru_cache
import json
import math
import os
import numpy as np

from cad_geometry import primitive, transformed, load_mesh
from cad_scene import rotation, SOLIDS
from plate_cad import BRep, Build, TopAbs, TopExp, TopoDS, collections, gp
from OCP import BRepAdaptor, BRepExtrema, BRepGProp, GeomAbs, GProp, IntCurvesFace

TYPES={"face":TopAbs.TopAbs_FACE,"edge":TopAbs.TopAbs_EDGE,"vertex":TopAbs.TopAbs_VERTEX}
NAMES={"center":"Центр","face":"Грань","edge":"Ребро","vertex":"Вершина"}
OPS={"section":"Срез плоскостью","union":"Объединение","subtract":"Вычитание","intersect":"Пересечение"}


def unit(v):
    v=np.asarray(v,dtype=float)
    n=np.linalg.norm(v)
    if not np.isfinite(n) or n<1e-10:
        raise ValueError("Задайте ненулевое направление")
    return v/n


def frame(z,x=(1,0,0)):
    z=unit(z)
    if abs(z@unit(x))>.999999:
        x=np.eye(3)[np.argmin(abs(z))]
    x=unit(x-z*np.dot(x,z))
    return np.column_stack((x,np.cross(z,x),z))


def euler(b):
    y=math.asin(np.clip(-b[2,0],-1,1))
    if abs(math.cos(y))>1e-8:
        x,z=math.atan2(b[2,1],b[2,2]),math.atan2(b[1,0],b[0,0])
    else:
        x,z=0.,math.atan2(-b[0,1],b[1,1])
    return np.degrees([x,y,z]).tolist()


def resolved(scene,obj,trail=()):
    if obj["id"] in trail:
        raise ValueError("Цикл зависимостей операций")
    data={k:obj[k] for k in ("kind","dims","asset","profile","cut","outline","circle","arcs") if k in obj}
    ops=[]
    for feature in obj.get("operations",[]):
        op=feature.copy()
        if not op.get("suppressed") and op["kind"]!="section":
            tool=lookup(scene,op["tool"])
            basis=rotation(obj).T
            op.update(operand=resolved(scene,tool,(*trail,obj["id"])),
                      basis=(basis@rotation(tool)).round(13).tolist(),
                      offset=(basis@(np.array(tool["pos"])-obj["pos"])).round(12).tolist())
        ops.append(op)
    if ops:
        data["operations"]=ops
    return data


def lookup(scene,key):
    for item in scene["objects"]:
        if item["id"]==key:
            return item
    raise ValueError("Опорный объект удалён")


def polygon(points):
    wire=Build.BRepBuilderAPI_MakePolygon()
    for p in points:
        wire.Add(gp.gp_Pnt(*p))
    wire.Close()
    return Build.BRepBuilderAPI_MakeFace(wire.Wire(),True).Face()


@lru_cache(maxsize=12)
def cached_shape(folder,description):
    item=json.loads(description)
    if item["kind"] in SOLIDS:
        return primitive(item,folder)
    if item["kind"] in ("screen","area"):
        w,h=item["dims"]["w"]/2,item["dims"]["d"]/2
        return polygon([(-w,-h,0),(w,-h,0),(w,h,0),(-w,h,0)])
    return Build.BRepBuilderAPI_MakeVertex(gp.gp_Pnt(0,0,0)).Vertex()


def shape(scene,obj,folder):
    return cached_shape(folder,json.dumps(resolved(scene,obj),sort_keys=True))


def shapes(body,kind):
    table=collections.IndexedMap_TopoDS_Shape_TopTools_ShapeMapHasher()
    TopExp.TopExp.MapShapes_s(body,TYPES[kind],table)
    return [table.FindKey(j) for j in range(1,table.Extent()+1)]


def edge_points(edge):
    curve=BRepAdaptor.BRepAdaptor_Curve(TopoDS.TopoDS.Edge(edge))
    n=2 if curve.GetType()==GeomAbs.GeomAbs_Line else 49
    return np.asarray([curve.Value(t).Coord() for t in np.linspace(curve.FirstParameter(),curve.LastParameter(),n)])


def fingerprint(options,kind,index):
    sub=options[index]
    result=[len(options),int(sub.Orientation())]
    if kind=="face":
        surface=BRepAdaptor.BRepAdaptor_Surface(TopoDS.TopoDS.Face(sub))
        result.append(int(surface.GetType()))
        if surface.GetType()==GeomAbs.GeomAbs_Plane:
            result.extend(np.round(surface.Plane().Axis().Direction().Coord(),8).tolist())
    elif kind=="edge" and not BRep.BRep_Tool.Degenerated_s(TopoDS.TopoDS.Edge(sub)):
        result.append(int(BRepAdaptor.BRepAdaptor_Curve(TopoDS.TopoDS.Edge(sub)).GetType()))
    return result


@lru_cache(maxsize=3)
def mesh_source(path):
    return load_mesh(path)


@lru_cache(maxsize=3)
def mesh_acceleration(path,library):
    from mesh_native import MeshIndex
    return MeshIndex(library,*mesh_source(path))


def mesh_intersection(xyz,tri,ray,direction):
    # Möller–Trumbore against the original geometry, independent of display LOD.
    a,b,c=xyz[tri].transpose(1,0,2)
    e1,e2=b-a,c-a
    h=np.cross(direction,e2)
    det=np.einsum("ij,ij->i",e1,h)
    valid=abs(det)>1e-12
    inv=np.divide(1,det,out=np.zeros_like(det),where=valid)
    off=ray-a
    u=np.einsum("ij,ij->i",off,h)*inv
    q=np.cross(off,e1)
    v=(q@direction)*inv
    dist=np.einsum("ij,ij->i",e2,q)*inv
    hits=np.flatnonzero(valid&(u>=0)&(v>=0)&(u+v<=1))
    if not len(hits):
        raise ValueError("Укажите участок исходной сетки")
    j=int(hits[np.argmin(dist[hits])])
    return j,dist[j]



def ref_for(scene,obj,folder,kind,ray,direction,tolerance):
    """Ray and direction in object coordinates. Distances are in millimetres."""
    ref=dict(object=obj["id"],kind=kind,index=0)
    if kind=="center":
        return ref
    if obj["kind"]=="mesh":
        path=folder/obj["asset"]
        xyz,tri=mesh_source(path)
        library=os.environ.get("RAYSCULPT_MESH_INDEX")
        if library:
            j,distance=mesh_acceleration(path,library).nearest(ray,direction)
        else:
            j,distance=mesh_intersection(xyz,tri,ray,direction)
        hit=ray+distance*direction
        ids=tri[j]
        ref["index"]=j
        if kind=="vertex":
            ref["nodes"]=[int(ids[np.argmin(np.linalg.norm(xyz[ids]-hit,axis=1))])]
        elif kind=="edge":
            lines=[xyz[[i,k]] for i,k in zip(ids,np.roll(ids,-1))]
            side=np.argmin([segment_distance(hit,*line) for line in lines])
            ref["nodes"]=[int(ids[side]),int(ids[(side+1)%3])]
        return ref
    body=shape(scene,obj,folder)
    options=shapes(body,kind)
    if kind=="face":
        inter=IntCurvesFace.IntCurvesFace_ShapeIntersector()
        inter.Load(body,1e-7)
        inter.Perform(gp.gp_Lin(gp.gp_Pnt(*ray),gp.gp_Dir(*direction)),-1e12,1e12)
        if inter.NbPnt():
            nearest=min(range(1,inter.NbPnt()+1),key=inter.WParameter)
            hit=inter.Face(nearest)
            ref["index"]=next(i for i,s in enumerate(options) if s.IsSame(hit))
            ref["uv"]=[inter.UParameter(nearest),inter.VParameter(nearest)]
            ref["signature"]=fingerprint(options,kind,ref["index"])
            return ref
    else:
        candidates=[]
        for j,s in enumerate(options):
            if kind=="vertex":
                points=np.array([BRep.BRep_Tool.Pnt_s(TopoDS.TopoDS.Vertex(s)).Coord()])
            else:
                if BRep.BRep_Tool.Degenerated_s(TopoDS.TopoDS.Edge(s)):
                    continue
                points=edge_points(s)
            delta=points-ray
            depths=delta@direction
            projected=delta-depths[:,None]*direction
            distance=(np.linalg.norm(projected[0]) if len(points)==1 else
                      min(segment_distance(np.zeros(3),a,b) for a,b in zip(projected,projected[1:])))
            if distance<=tolerance:
                candidates.append((round(distance,5),min(depths),j))
        if candidates:
            ref["index"]=min(candidates)[2]
            ref["signature"]=fingerprint(options,kind,ref["index"])
            return ref
    raise ValueError("Щёлкните ближе к нужному элементу")


def segment_distance(p,a,b):
    diff=b-a
    t=np.clip((p-a)@diff/max(diff@diff,1e-24),0,1)
    return np.linalg.norm(p-a-t*diff)


def element(scene,ref,folder):
    obj=lookup(scene,ref["object"])
    typ=ref["kind"]
    if typ=="vertex_origin":
        return Build.BRepBuilderAPI_MakeVertex(gp.gp_Pnt(0,0,0)).Vertex()
    if typ=="center":
        center=np.zeros(3)
        if obj["kind"] in ("plate","box","cylinder","prism"):
            center[2]=obj["dims"]["h"]/2
        return Build.BRepBuilderAPI_MakeVertex(gp.gp_Pnt(*center)).Vertex()
    if obj["kind"]=="mesh":
        xyz,tri=mesh_source(folder/obj["asset"])
        if typ=="face":
            return polygon(xyz[tri[ref["index"]]])
        if typ=="edge":
            return Build.BRepBuilderAPI_MakeEdge(*(gp.gp_Pnt(*xyz[k]) for k in ref["nodes"])).Edge()
        return Build.BRepBuilderAPI_MakeVertex(gp.gp_Pnt(*xyz[ref["nodes"][0]])).Vertex()
    options=shapes(shape(scene,obj,folder),typ)
    if (not 0<=ref["index"]<len(options) or
            ("signature" in ref and fingerprint(options,typ,ref["index"])!=ref["signature"])):
        raise ValueError("Опорный элемент изменился при перестроении. Выберите его повторно.")
    return options[ref["index"]]


def local_frame(scene,ref,folder):
    elem=element(scene,ref,folder)
    k=ref["kind"]
    if k in ("center","vertex","vertex_origin"):
        p=BRep.BRep_Tool.Pnt_s(TopoDS.TopoDS.Vertex(elem)).Coord()
        return np.asarray(p),np.eye(3)
    if k=="edge":
        curve=BRepAdaptor.BRepAdaptor_Curve(TopoDS.TopoDS.Edge(elem))
        t=(curve.FirstParameter()+curve.LastParameter())/2
        p,v=gp.gp_Pnt(),gp.gp_Vec()
        curve.D1(t,p,v)
        tangent=unit(v.Coord())
        z=np.array([0.,0.,1.])
        if abs(tangent@z)>.9999:
            z=np.array([0.,1.,0.])
        return np.array(p.Coord()),frame(unit(z-tangent*(z@tangent)),tangent)
    surf=BRepAdaptor.BRepAdaptor_Surface(TopoDS.TopoDS.Face(elem))
    if surf.GetType()==GeomAbs.GeomAbs_Plane:
        props=GProp.GProp_GProps()
        BRepGProp.BRepGProp.SurfaceProperties_s(elem,props)
        p=props.CentreOfMass()
        normal=np.array(surf.Plane().Axis().Direction().Coord())
    else:
        if "uv" not in ref:
            raise ValueError("Выберите точку на криволинейной грани в рабочем виде")
        p,du,dv=gp.gp_Pnt(),gp.gp_Vec(),gp.gp_Vec()
        surf.D1(*ref["uv"],p,du,dv)
        normal=unit(np.cross(du.Coord(),dv.Coord()))
    if elem.Orientation()==TopAbs.TopAbs_REVERSED:
        normal=-normal
    return np.asarray(p.Coord()),frame(normal)


def world_frame(scene,ref,folder):
    o=lookup(scene,ref["object"])
    p,b=local_frame(scene,ref,folder)
    return rotation(o)@p+o["pos"],rotation(o)@b


def measure(scene,a,b,folder):
    elems=[]
    for ref in (a,b):
        o=lookup(scene,ref["object"])
        elems.append(transformed(element(scene,ref,folder),rotation(o),o["pos"]))
    query=BRepExtrema.BRepExtrema_DistShapeShape(*elems)
    if not query.IsDone() or query.NbSolution()<1:
        raise ValueError("Расстояние не определено")
    angle=None
    if all(r["kind"] in ("face","edge") for r in (a,b)):
        vectors=[world_frame(scene,r,folder)[1][:,2 if r["kind"]=="face" else 0] for r in (a,b)]
        angle=math.degrees(math.acos(np.clip(abs(vectors[0]@vectors[1]),0,1)))
        if a["kind"]!=b["kind"]:
            angle=90-angle
    return query.Value(),angle


def outlines(scene,ref,folder):
    s=element(scene,ref,folder)
    if ref["kind"] in ("center","vertex"):
        return [np.array([local_frame(scene,ref,folder)[0]])]
    edges=[s] if ref["kind"]=="edge" else shapes(s,"edge")
    return [edge_points(e) for e in edges if not BRep.BRep_Tool.Degenerated_s(TopoDS.TopoDS.Edge(e))]


def rebuild_mates(scene,folder):
    """Evaluate operand and parent dependencies before dependent features."""
    from cad_scene import move
    done=set()
    visiting=set()
    def visit(item):
        key=item["id"]
        if key in done:
            return
        if key in visiting:
            raise ValueError("Цикл построений или сопряжений")
        visiting.add(key)
        for op in item.get("operations",[]):
            if "tool" in op and not op.get("suppressed"):
                visit(lookup(scene,op["tool"]))
        if "parent" in item:
            visit(lookup(scene,item["parent"]))
        if "constraint" in item:
            mate=item["constraint"]
            if item.get("parent")!=mate["base"]["object"] or key!=mate["own"]["object"]:
                raise ValueError("Повреждена опора сопряжения")
            anchor,base=world_frame(scene,mate["base"],folder)
            own,local=local_frame(scene,mate["own"],folder)
            tilted=rotation({"rot":[mate["angle"],0,0]})
            r=base@tilted@local.T
            pos=anchor+base[:,2]*mate["distance"]-r@own
            if not np.allclose(item["pos"],pos,rtol=0,atol=1e-9) or not np.allclose(rotation(item),r,atol=1e-10):
                parent=item.pop("parent")
                try:
                    move(scene,item,pos.tolist(),euler(r))
                finally:
                    item["parent"]=parent
        visiting.remove(key)
        done.add(key)
    for obj in scene["objects"]:
        visit(obj)
