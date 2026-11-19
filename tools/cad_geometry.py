"""CAD geometry stays in Open CASCADE. Display triangulations are disposable."""
from pathlib import Path
import math
import numpy as np

from plate_cad import BRep, BRepTools, Build, IFSelect, STEPControl, TopAbs, TopoDS, components, gp, solid_for
from OCP import (Bnd, BRepAlgoAPI, BRepBndLib, BRepCheck, BRepGProp, BRepMesh, BRepPrimAPI,
                 GProp, RWStl, TopLoc)
from export_plate import Profile
from plate_files import PlateMesh
from cad_scene import rotation


def primitive(item, folder):
    kind, d = item["kind"], item["dims"]
    if kind in ("box","plate"):
        if kind=="plate" and item.get("profile"):
            shape = solid_for(Profile.read(folder/item["profile"]))
        else:
            w,h,depth = d["w"],d["h"],d.get("d",d["w"])
            shape = BRepPrimAPI.BRepPrimAPI_MakeBox(gp.gp_Pnt(-w/2,-depth/2,0),w,depth,h).Shape()
    elif kind=="sphere":
        shape = BRepPrimAPI.BRepPrimAPI_MakeSphere(d["r"]).Shape()
    elif kind=="cylinder":
        shape = BRepPrimAPI.BRepPrimAPI_MakeCylinder(d["r"],d["h"]).Shape()
    elif kind=="prism":
        poly=Build.BRepBuilderAPI_MakePolygon()
        for x,y in item["outline"]:
            poly.Add(gp.gp_Pnt(x,y,0))
        poly.Close()
        if not poly.IsDone() or len(set(map(tuple,item["outline"])))!=len(item["outline"]):
            raise ValueError("Контур должен содержать разные вершины")
        face=Build.BRepBuilderAPI_MakeFace(poly.Wire(),True)
        if not face.IsDone() or not BRepCheck.BRepCheck_Analyzer(face.Face()).IsValid():
            raise ValueError("Эскиз должен образовывать простой замкнутый контур без самопересечений")
        shape=BRepPrimAPI.BRepPrimAPI_MakePrism(face.Face(),gp.gp_Vec(0,0,d["h"])).Shape()
    elif kind=="cad":
        path = folder/item["asset"]
        if path.suffix==".brep":
            shape = TopoDS.TopoDS_Shape()
            if not BRepTools.BRepTools.Read_s(shape,str(path),BRep.BRep_Builder()):
                raise ValueError("Ошибка чтения BREP")
        else:
            reader = STEPControl.STEPControl_Reader()
            if reader.ReadFile(str(path))!=IFSelect.IFSelect_RetDone or not reader.TransferRoots():
                raise ValueError("Ошибка чтения STEP")
            shape = reader.OneShape()
    else:
        raise ValueError("Команда доступна для тела CAD")
    if "cut" in item:
        bounds = Bnd.Bnd_Box()
        BRepBndLib.BRepBndLib.Add_s(shape,bounds)
        x,y,z = bounds.CornerMin().Coord()
        X,Y,Z = bounds.CornerMax().Coord()
        cut = item["cut"]
        if not z < cut < Z:
            raise ValueError("Плоскость среза должна проходить внутри тела")
        margin = max(1.,X-x,Y-y,Z-z)*.02
        lower = gp.gp_Pnt(x-margin,y-margin,z-margin)
        upper = gp.gp_Pnt(X+margin,Y+margin,cut)
        boolean = BRepAlgoAPI.BRepAlgoAPI_Common(shape,BRepPrimAPI.BRepPrimAPI_MakeBox(lower,upper).Shape())
        if not boolean.IsDone() or not list(components(boolean.Shape(),TopAbs.TopAbs_SOLID)):
            raise ValueError("Не удалось выполнить срез тела")
        shape = boolean.Shape()
    return shape


def triangulate(shape):
    BRepMesh.BRepMesh_IncrementalMesh(shape,.12,False,.25,True)
    xyz, triangles, offset = [], [], 0
    for f in components(shape,TopAbs.TopAbs_FACE):
        loc = TopLoc.TopLoc_Location()
        mesh = BRep.BRep_Tool.Triangulation_s(TopoDS.TopoDS.Face(f),loc)
        if mesh is None:
            continue
        transform = loc.Transformation()
        points = [mesh.Node(i).Transformed(transform).Coord() for i in range(1,mesh.NbNodes()+1)]
        faces = np.array([mesh.Triangle(i).Get() for i in range(1,mesh.NbTriangles()+1)],dtype=int)-1
        if f.Orientation()==TopAbs.TopAbs_REVERSED:
            faces = faces[:,::-1]
        xyz.append(points)
        triangles.append(faces+offset)
        offset += len(points)
    if not xyz:
        raise ValueError("В модели нет отображаемых граней")
    return np.concatenate(xyz),np.concatenate(triangles)


def load_mesh(path):
    if path.suffix==".stl":
        mesh = RWStl.RWStl.ReadFile_s(str(path))
        if mesh is None:
            raise ValueError("Ошибка чтения STL")
        points = [mesh.Node(i).Coord() for i in range(1,mesh.NbNodes()+1)]
        faces = np.array([mesh.Triangle(i).Get() for i in range(1,mesh.NbTriangles()+1)])-1
    else:
        points,faces = [],[]
        with path.open(encoding="utf-8-sig") as f:
            for line in f:
                part=line.partition("#")[0].split()
                if not part:
                    continue
                if part[0]=="v":
                    points.append([float(x) for x in part[1:4]])
                if part[0]=="f":
                    face=[int(k.split("/")[0]) for k in part[1:]]
                    face=[len(points)+i if i<0 else i-1 for i in face]
                    if min(face)<0 or max(face)>=len(points):
                        raise ValueError("Некорректные индексы OBJ")
                    faces.extend((face[0],a,b) for a,b in zip(face[1:],face[2:]))
    xyz,tri=np.asarray(points,dtype=float),np.asarray(faces,dtype=int)
    if xyz.ndim!=2 or xyz.shape[1]!=3 or not np.isfinite(xyz).all() or tri.ndim!=2 or tri.shape[1]!=3 or not len(tri):
        raise ValueError("Некорректная полигональная модель")
    return xyz,tri


def profile_volume(p):
    # Integral of each clamped cubic basis is one quarter of its knot support.
    n = p.grid
    knots = np.r_[[0.]*3,np.linspace(0,1,n),[1.]*3]
    weights = (knots[4:]-knots[:-4])/4
    # Transpose of clamp_axis() for the linear-end photographic convention.
    coeff = np.zeros(n)
    coeff[0],coeff[-1] = weights[0],weights[-1]
    coeff[0:2] += weights[1]*np.array([2,1])/3
    coeff[-2:] += weights[-2]*np.array([1,2])/3
    coeff[1:-1] += weights[2:-2]
    return p.width**2*(p.thickness+float(coeff @ p.relief @ coeff))


def display_mesh(xyz,tri):
    if len(tri)<12000:
        return xyz,tri
    # Cluster for interactive display only. All exports reopen the original asset.
    size=max(np.ptp(xyz,axis=0))
    if size<=0:
        raise ValueError("Модель имеет нулевой размер")
    for count in (55,36,24,16):
        cells=np.floor((xyz-xyz.min(axis=0))*(count/size)).astype(np.int32)
        stride=count+2
        _,labels=np.unique(cells[:,0]+stride*(cells[:,1]+stride*cells[:,2]),return_inverse=True)
        weights=np.bincount(labels)
        rep=np.column_stack([np.bincount(labels,weights=dim)/weights for dim in xyz.T])
        faces=labels[tri]
        faces=faces[(faces[:,0]!=faces[:,1])&(faces[:,0]!=faces[:,2])&(faces[:,1]!=faces[:,2])]
        if 0<len(faces)<=12000:
            return rep,faces
    return (rep,faces) if len(faces) else (xyz,tri)


def mesh_volume(xyz,tri):
    # Coincident STL vertices are sometimes repeated at facet boundaries.
    pts,inv=np.unique(xyz,axis=0,return_inverse=True)
    faces=inv[tri]
    edges=np.vstack((faces[:,:2],faces[:,1:],faces[:,[2,0]]))
    signed=np.where(edges[:,0]<edges[:,1],1,-1)
    _,j,counts=np.unique(np.sort(edges,axis=1),axis=0,return_inverse=True,return_counts=True)
    if not np.all(counts==2) or np.any(np.bincount(j,weights=signed)):
        return None
    verts=pts[faces]-pts.mean(axis=0)
    return abs(float(np.einsum("ij,ij->",verts[:,0],np.cross(verts[:,1],verts[:,2]))))/6


def visual_geometry(item,folder):
    if item["kind"]=="mesh":
        xyz,tri=load_mesh(folder/item["asset"])
        return *display_mesh(xyz,tri),mesh_volume(xyz,tri)
    if item.get("profile"):
        profile=Profile.read(folder/item["profile"])
        mesh=PlateMesh.sample(profile,min(42,profile.grid))
        return mesh.vertices,np.concatenate(list(mesh.triangles())),profile_volume(profile)
    shape=primitive(item,folder)
    xyz,tri=triangulate(shape)
    volume=None
    if list(components(shape,TopAbs.TopAbs_SOLID)):
        props=GProp.GProp_GProps()
        BRepGProp.BRepGProp.VolumeProperties_s(shape,props)
        volume=abs(props.Mass())
    return *display_mesh(xyz,tri),volume


def export_body(item,folder,path,cells=None):
    from tempfile import TemporaryDirectory
    import os
    from plate_cad import write_solid
    from OCP import StlAPI
    ext=path.suffix.lower()
    with TemporaryDirectory(dir=path.parent) as tmp:
        staging=Path(tmp)/("body"+ext)
        if item["kind"]=="mesh":
            if ext not in (".obj",".stl"):
                raise ValueError("Для сетки доступны OBJ и STL")
            xyz,tri=load_mesh(folder/item["asset"])
            xyz=xyz @ rotation(item).T + item["pos"]
            if ext==".obj":
                with staging.open("wb") as f:
                    np.savetxt(f,xyz,fmt="v %.12g %.12g %.12g")
                    np.savetxt(f,tri+1,fmt="f %d %d %d")
            else:
                from transport_mesh import emit
                with staging.open("wb") as f:
                    f.write(bytes(80)+len(tri).to_bytes(4,"little"))
                    for group in np.array_split(tri,max(1,math.ceil(len(tri)/10000))):
                        emit(f,xyz[group])
        elif ext in (".3mf",".obj",".stl") and item["kind"]=="plate":
            if item.get("profile"):
                p=Profile.read(folder/item["profile"])
            else:
                d=item["dims"]
                p=Profile(d["w"],d["h"],np.zeros((3,3)),True)
            mesh=PlateMesh.sample(p,cells or (max(256,p.grid-1) if item.get("profile") else 2))
            mesh.vertices=mesh.vertices @ rotation(item).T+item["pos"]
            mesh.write(staging)
        else:
            shape=primitive(item,folder)
            matrix=np.column_stack((rotation(item),item["pos"]))
            pose=gp.gp_Trsf()
            pose.SetValues(*matrix.flatten())
            shape=Build.BRepBuilderAPI_Transform(shape,pose,True).Shape()
            if ext==".brep":
                write_solid(shape,staging)
            elif ext in (".step",".stp"):
                writer=STEPControl.STEPControl_Writer()
                if writer.Transfer(shape,STEPControl.STEPControl_AsIs)!=IFSelect.IFSelect_RetDone or writer.Write(str(staging))!=IFSelect.IFSelect_RetDone:
                    raise OSError("Ошибка записи STEP")
            elif ext==".stl":
                BRepMesh.BRepMesh_IncrementalMesh(shape,.035,False,.12,True)
                writer=StlAPI.StlAPI_Writer()
                writer.ASCIIMode=False
                if not writer.Write(shape,str(staging)):
                    raise OSError("Ошибка записи STL")
            else:
                raise ValueError("Формат недоступен для этого тела")
        os.replace(staging,path)
