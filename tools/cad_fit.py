"""Joint low-resolution fit for non-axial scenes, multiple screens and turntable views."""
import json
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image

from cad_scene import ROOT, optical_setup, trace_settings
from cad_targets import screen_pixels, screen_target


def target_mesh(assets,key,folder):
    source=assets/key
    if source.suffix==".obj":
        return source
    from cad_geometry import load_mesh,primitive,triangulate
    if source.suffix==".stl":
        xyz,tri=load_mesh(source)
    else:
        xyz,tri=triangulate(primitive(dict(kind="cad",asset=key,dims={}),assets))
    path=folder/"target.obj"
    with path.open("wb") as f:
        np.savetxt(f,xyz,fmt="v %.15g %.15g %.15g")
        np.savetxt(f,tri+1,fmt="f %d %d %d")
    return path


def write_target(assets,screen,resolution,path,mesh=None,angle=0.):
    cols,rows=screen_pixels(screen["dims"]["w"],screen["dims"]["d"],resolution)
    if mesh is not None:
        subprocess.run([ROOT/"build/Release/caustic_cli.exe","render-target","--obj",mesh,"--angle",str(angle),
                        "--elevation","0","--width",str(cols),"--height",str(rows),"--out",path],check=True)
        with Image.open(path) as image:
            return np.asarray(image).astype(float)/255
    frame=screen_target(assets/screen["image"],(cols,rows),screen.get("image_fit","fit"))
    with path.open("wb") as f:
        f.write(f"P5\n{cols} {rows}\n65535\n".encode("ascii"))
        f.write(np.rint(frame*65535).astype(">u2").tobytes())
    return frame


def fit(scene,assets,folder):
    cli=ROOT/"build/Release/caustic_cli.exe"
    folder.mkdir()
    _,_,screens=optical_setup(scene)
    turn=scene.get("turntable")
    angles=sorted(turn["angles"]) if turn else [0.]
    count=32
    budget=count**2*len(screens)*len(angles)
    if budget>1000000:
        raise ValueError("Суммарное разрешение целей превышает бюджет оптимизации")
    (folder/"scene.txt").write_text(trace_settings(scene,count,16000),encoding="ascii")
    (folder/"angles.txt").write_text("7 "+str(len(angles))+" "+" ".join(map(str,angles)),encoding="ascii")
    source_obj=target_mesh(assets,turn["asset"],folder) if turn else None

    for j,angle in enumerate(angles):
        for k,screen in enumerate(screens):
            write_target(assets,screen,count,folder/f"{j}_{k}.pgm",source_obj,angle)
    out=folder/"result"
    subprocess.run([cli,"fit-scene",folder,out],check=True)
    # Inspect the same physical surface with fresh rays, including between target angles.
    check_angles=sorted(set(angles+[(a+b)/2 for a,b in zip(angles,angles[1:])]))
    cfg=folder/"verify.txt"
    cfg.write_text(trace_settings(scene,96,240000),encoding="ascii")
    entries=[]
    numerator=denominator=0.
    for j,angle in enumerate(check_angles):
        part=out/f"angle{j}"
        subprocess.run([cli,"trace-scene",cfg,out/"surface.txt",part,str(angle)],check=True)
        report=json.loads((part/"report.json").read_text())
        for k,screen in enumerate(screens):
            image=part/report["screens"][k]["image"]
            target_path=part/f"target{k}.pgm"
            goal=write_target(assets,screen,96,target_path,source_obj,angle)
            with Image.open(image) as im:
                encoded=np.asarray(im).astype(float)/255
            observed=np.where(encoded<=.04045,encoded/12.92,((encoded+.055)/1.055)**2.4)*report["peak"]
            obs2=float(np.sum(observed**2))
            dot=float(np.sum(observed*goal))
            goal2=float(np.sum(goal**2))
            if angle in angles:
                numerator+=dot
                denominator+=obs2
            entries.append(dict(image=image.relative_to(out).as_posix(),name=screen["name"],
                                screen=screen["id"],target=target_path.relative_to(out).as_posix(),
                                angle=angle,intermediate=angle not in angles,flux=report["screens"][k]["flux"],
                                peak=report["peak"],dot=dot,obs2=obs2,goal2=goal2))
    gain=numerator/denominator if denominator else 0
    for row in entries:
        row["display_gain"]=gain*row.pop("peak")
        dot,obs2,goal2=[row.pop(k) for k in ("dot","obs2","goal2")]
        row["error"]=float(np.sqrt(max(0.,gain*gain*obs2-2*gain*dot+goal2)/max(goal2,1e-16)))
    report=json.loads((out/"fit.json").read_text())
    report["screens"]=entries
    (out/"validation.json").write_text(json.dumps(report),encoding="utf-8")
