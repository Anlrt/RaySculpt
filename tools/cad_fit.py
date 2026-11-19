"""Joint low-resolution fit for non-axial scenes, multiple screens and turntable views."""
import json
from pathlib import Path
import subprocess

import cv2
import numpy as np
from PIL import Image

from cad_scene import ROOT, optical_setup, trace_settings
from design_photo import monochrome


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
    source_obj=None
    if turn:
        source_obj=assets/turn["asset"]
        if source_obj.suffix!=".obj":
            from cad_geometry import load_mesh,primitive,triangulate
            if source_obj.suffix==".stl":
                xyz,tri=load_mesh(source_obj)
            else:
                xyz,tri=triangulate(primitive(dict(kind="cad",asset=turn["asset"],dims={}),assets))
            source_obj=folder/"target.obj"
            with source_obj.open("wb") as f:
                np.savetxt(f,xyz,fmt="v %.15g %.15g %.15g")
                np.savetxt(f,tri+1,fmt="f %d %d %d")

    def target(angle,screen,resolution,path):
        w,h=screen["dims"]["w"],screen["dims"]["d"]
        cols=max(1,round(resolution*w/max(w,h)))
        rows=max(1,round(resolution*h/max(w,h)))
        if turn:
            subprocess.run([cli,"render-target","--obj",source_obj,"--angle",str(angle),
                            "--elevation","0","--width",str(cols),"--height",str(rows),"--out",path],check=True)
            with Image.open(path) as image:
                return np.asarray(image).astype(float)/255
        gray=monochrome(assets/screen["image"],max(rows,cols))
        ratio=min(cols/gray.shape[1],rows/gray.shape[0])
        resized=cv2.resize(gray,(max(1,round(ratio*gray.shape[1])),max(1,round(ratio*gray.shape[0]))),
                           interpolation=cv2.INTER_AREA)
        frame=np.zeros((rows,cols))
        y=(rows-resized.shape[0])//2
        x=(cols-resized.shape[1])//2
        frame[y:y+resized.shape[0],x:x+resized.shape[1]]=resized
        with path.open("wb") as f:
            f.write(f"P5\n{cols} {rows}\n65535\n".encode("ascii"))
            f.write(np.rint(frame*65535).astype(">u2").tobytes())
        return frame

    for j,angle in enumerate(angles):
        for k,screen in enumerate(screens):
            target(angle,screen,count,folder/f"{j}_{k}.pgm")
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
            goal=target(angle,screen,96,part/f"target{k}.pgm")
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
                                angle=angle,intermediate=angle not in angles,flux=report["screens"][k]["flux"],
                                dot=dot,obs2=obs2,goal2=goal2))
    gain=numerator/denominator if denominator else 0
    for row in entries:
        dot,obs2,goal2=[row.pop(k) for k in ("dot","obs2","goal2")]
        row["error"]=float(np.sqrt(max(0.,gain*gain*obs2-2*gain*dot+goal2)/max(goal2,1e-16)))
    report=json.loads((out/"fit.json").read_text())
    report["screens"]=entries
    (out/"validation.json").write_text(json.dumps(report),encoding="utf-8")
