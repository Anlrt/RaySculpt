"""Inspect original BREP/mesh geometry off the UI thread."""
import numpy as np
from cad_scene import SOLIDS,rotation,consumed_tools
from cad_geometry import load_mesh,transformed
from cad_topology import shape,shapes,edge_points
from plate_cad import TopAbs,TopoDS,BRep,components
from OCP import BRepAlgoAPI,BRepCheck,BRepGProp,GProp,BRepBndLib,Bnd


def inspect(scene,assets):
    issues=[]
    def issue(item,message,lines=(),other=None,severity="error"):
        basis=rotation(item)
        world=[(np.asarray(line)@basis.T+item["pos"]).tolist() for line in lines]
        issues.append(dict(message=message,objects=[item["id"]]+([other["id"]] if other else []),
                           severity=severity,lines=world))

    bodies=[]
    consumed=consumed_tools(scene)
    for obj in scene["objects"]:
        if obj["id"] in consumed:
            continue
        k=obj["kind"]
        if k not in SOLIDS|{"screen","area"}:
            continue
        if k=="mesh":
            xyz,tri=load_mesh(assets/obj["asset"])
            pts,remap=np.unique(xyz,axis=0,return_inverse=True)
            f=remap[tri]
            a,b,c=pts[f].transpose(1,0,2)
            bad=np.flatnonzero(np.linalg.norm(np.cross(b-a,c-a),axis=1)<=1e-12)
            if len(bad):
                issue(obj,f'{obj["name"]} · вырожденных граней {len(bad)}',
                      (pts[f[i,[0,1,2,0]]] for i in bad[:120]))
            directed=np.vstack((f[:,[0,1]],f[:,[1,2]],f[:,[2,0]]))
            edge,idx,count=np.unique(np.sort(directed,axis=1),axis=0,return_inverse=True,return_counts=True)
            winding=np.bincount(idx,weights=np.sign(directed[:,1]-directed[:,0]))
            for mask,label in ((count==1,"открытые кромки"),(count>2,"немногообразные рёбра"),
                               ((count==2)&(winding!=0),"несогласованная ориентация граней")):
                ids=np.flatnonzero(mask)
                if len(ids):
                    issue(obj,f'{obj["name"]} · {label}, {len(ids)}',pts[edge[ids[:120]]])
            # Exact intersections of original mesh facets with finite screen rectangles.
            for screen in (i for i in scene["objects"] if i["kind"]=="screen"):
                local=(xyz@rotation(obj).T+obj["pos"]-np.array(screen["pos"]))@rotation(screen)
                hits=[]
                for start in range(0,len(tri),8192):
                    tris=tri[start:start+8192]
                    vs=local[tris]
                    half=np.array([screen["dims"]["w"]/2,screen["dims"]["d"]/2,0.])
                    ok=np.all((vs.min(1)<=half+1e-8)&(vs.max(1)>=-half-1e-8),axis=1)
                    for t,v in zip(tris[ok],vs[ok]):
                        if triangle_rect(v,half[:2]):
                            hits.append(xyz[t[[0,1,2,0]]])
                            if len(hits)>=20:
                                break
                    if len(hits)>=20:
                        break
                if hits:
                    issue(obj,f'{obj["name"]} пересекает экран {screen["name"]}',hits,screen)
            continue
        solid=shape(scene,obj,assets)
        if k in SOLIDS:
            analyzer=BRepCheck.BRepCheck_Analyzer(solid)
            if not analyzer.IsValid():
                bad=[f for f in shapes(solid,"face") if not analyzer.IsValid(f)]
                issue(obj,f'{obj["name"]} · тело некорректно, проблемных граней {len(bad)}',
                      (edge_points(e) for f in bad[:10] for e in shapes(f,"edge")[:8]
                       if not BRep.BRep_Tool.Degenerated_s(TopoDS.TopoDS.Edge(e))))
            if not list(components(solid,TopAbs.TopAbs_SOLID)):
                from OCP import ShapeAnalysis
                bounds=ShapeAnalysis.ShapeAnalysis_FreeBounds(solid,1e-7,False,True)
                free=[e for wires in (bounds.GetClosedWires(),bounds.GetOpenWires()) for e in shapes(wires,"edge")]
                issue(obj,f'{obj["name"]} · оболочка без объёма, свободных кромок {len(free)}',
                      (edge_points(e) for e in free[:120]),severity="warning")
        located=transformed(solid,rotation(obj),obj["pos"])
        bbox=Bnd.Bnd_Box()
        BRepBndLib.BRepBndLib.Add_s(located,bbox)
        bodies.append((obj,located,bbox))
    for i,(a,A,ba) in enumerate(bodies):
        for b,B,bb in bodies[:i]:
            if ba.IsOut(bb) or (a["kind"]=="screen" and b["kind"]=="screen"):
                continue
            common=BRepAlgoAPI.BRepAlgoAPI_Common(A,B)
            if not common.IsDone():
                issue(a,"Ядро CAD не смогло проверить пересечение",other=b,severity="warning")
                continue
            overlap=common.Shape()
            props=GProp.GProp_GProps()
            if a["kind"] in SOLIDS and b["kind"] in SOLIDS:
                BRepGProp.BRepGProp.VolumeProperties_s(overlap,props)
            else:
                BRepGProp.BRepGProp.SurfaceProperties_s(overlap,props)
            if abs(props.Mass())>1e-9:
                issue(a,f'{a["name"]} и {b["name"]} пересекаются',other=b)
    return issues


def triangle_rect(v,half):
    # Separating axes for a triangle and a zero-thickness box in the screen frame.
    edges=np.roll(v,-1,axis=0)-v
    axes=[np.cross(edges[0],edges[1]),*(np.cross(e,a) for e in edges for a in np.eye(3))]
    for axis in axes:
        if np.linalg.norm(axis)<1e-12:
            continue
        projected=v@axis
        radius=half@abs(axis[:2])
        if min(projected)>radius+1e-9 or max(projected)<-radius-1e-9:
            return False
    return True
