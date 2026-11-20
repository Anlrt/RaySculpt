"""D02 STEP/BREP parity against the original OCCT Python verification."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np
from OCP import BRep, BRepBuilderAPI, BRepTools, gp

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools"))
import cad_scene as model
from cad_geometry import export_body,visual_geometry
from cad_topology import resolved
from export_plate import Profile
from plate_cad import TopoDS
from verify_solid import verify


def verify_native(executable, source, work):
    work.mkdir(parents=True)
    profile=Profile.read(source)
    ret={}
    for ext in (".step",".brep"):
        result=work/("local"+ext)
        subprocess.run([executable,source,"linear" if profile.linear_ends else "clamped",result],check=True)
        ret[ext]=verify(profile,result)[0]
    os.environ["RAYSCULPT_CAD_EXPORT"]=str(executable)
    item=model.node("plate")
    item.update(profile=model.store_asset(source,work),pos=[125.,0.,25.],rot=[90.,0.,90.])
    moved=work/"Пластина с поворотом.brep"
    export_body(item,work,moved)
    moved_shape=TopoDS.TopoDS_Shape()
    if not BRepTools.BRepTools.Read_s(moved_shape,str(moved),BRep.BRep_Builder()):
        raise ValueError("Cannot re-open transformed BREP")
    trsf=gp.gp_Trsf()
    trsf.SetValues(*np.column_stack((model.rotation(item),item["pos"])).flatten())
    local=BRepBuilderAPI.BRepBuilderAPI_Transform(moved_shape,trsf.Inverted(),True).Shape()
    recovered=work/"recovered.brep"
    if not BRepTools.BRepTools.Write_s(local,str(recovered)):
        raise ValueError("Cannot write localized verification shape")
    ret["moved"]=verify(profile,recovered)[0]

    original=moved.read_bytes()
    malformed=work/"bad.txt"
    malformed.write_text("not a profile",encoding="ascii")
    item["profile"]=malformed.name
    try:
        export_body(item,work,moved)
    except (ValueError, subprocess.CalledProcessError):
        if moved.read_bytes()!=original:
            raise AssertionError("failed export replaced the original")
    else:
        raise AssertionError("failed export was published")
    ret["bodies"]=verify_bodies(work)
    (work/"cad-export.json").write_text(json.dumps(ret,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(ret))


def verify_bodies(work):
    scene=model.new_scene()
    box,sphere,cylinder,prism=(model.node(kind) for kind in ("box","sphere","cylinder","prism"))
    tool=model.node("prism")
    tool.update(circle=[0.,0.,5.],outline=[])
    scene["objects"].extend([box,sphere,cylinder,prism,tool])
    sphere["cut"]=0.
    prism["arcs"]={"0":-4.}
    box["operations"]=[dict(id="hole",kind="subtract",tool=tool["id"]),
                       dict(id="section",kind="section",origin=[0.,0.,12.],normal=[1.,0.,1.])]
    comparisons={}
    for item in (box,sphere,cylinder,prism):
        body=dict(resolved(scene,item),pos=[125.,0.,25.],rot=[90.,0.,90.])
        expected=visual_geometry(body,work)[2]
        output=work/(item["kind"]+".step")
        export_body(body,work,output)
        reopened=model.node("cad")
        reopened["asset"]=model.store_asset(output,work)
        actual=visual_geometry(reopened,work)[2]
        if not np.isclose(actual,expected,rtol=.006,atol=0):
            raise ValueError(f"Native solid volume differs for {item['kind']}")
        comparisons[item["kind"]]=dict(reference_volume_mm3=expected,native_volume_mm3=actual)
    return comparisons


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable",type=Path)
    parser.add_argument("profile",type=Path)
    parser.add_argument("output",type=Path)
    a=parser.parse_args()
    verify_native(a.executable.resolve(),a.profile.resolve(),a.output.resolve())
