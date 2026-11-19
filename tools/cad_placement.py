"""Bounded tabletop placement search, scored against the actual plate when available."""
from copy import deepcopy
from pathlib import Path
import json
import math
import subprocess
import numpy as np
from PIL import Image
from cad_scene import ROOT,optical_setup,rotation,relative,diagnostics,trace_settings,index


def apply_layout(scene,screen_id,mode,gap,lamp_distance,size):
    from cad_scene import move,LABELS,descendants
    plate,light,screens=optical_setup(scene)
    screen=next(s for s in screens if s["id"]==screen_id)
    if any(i["locked"] or "parent" in i for i in (light,screen)):
        raise ValueError("Расстановка требует подвижного источника и выбранного экрана. Проверьте фиксацию и сопряжения.")
    if any(plate["id"] in descendants(scene,item["id"]) for item in (light,screen)):
        raise ValueError("Пластина следует за перемещаемым объектом. Снимите эту связь перед расстановкой.")
    r=rotation(plate)
    origin=np.array(plate["pos"])
    pos,_=relative(plate,screen)
    pos[2]=plate["dims"]["h"]+gap
    if light["name"]==LABELS[light["kind"]]:
        light["name"]=LABELS[mode]
    light.update(kind=mode,dims={"w":size,"d":size} if mode=="area" else {})
    move(scene,light,(origin-r[:,2]*(lamp_distance if mode=="area" else 75.)).tolist(),plate["rot"][:])
    move(scene,screen,(origin+r@pos).tolist(),screen["rot"][:])


def search(req):
    scene,mode=req["scene"],req["mode"]
    plate,light,screens=optical_setup(scene)
    screen=next(s for s in screens if s["id"]==req["screen"])
    if req["min_gap"]<=0 or req["max_gap"]<req["min_gap"]:
        raise ValueError("Максимальное расстояние должно быть не меньше минимального")
    assets=Path(req["assets"])
    directory=Path(req["path"]).with_suffix("")
    directory.mkdir()
    target=None
    if screen.get("image"):
        from design_photo import monochrome
        target=monochrome(assets/screen["image"],64)
    source,_=relative(plate,light)
    c,b=relative(plate,screen)
    h=plate["dims"]["h"]
    current_gap=c[2]-h
    gaps=np.unique(np.clip([*np.linspace(req["min_gap"],req["max_gap"],7),current_gap],req["min_gap"],req["max_gap"]))
    lamps=np.unique(np.clip([req["max_lamp"]*.45,req["max_lamp"]*.7,req["max_lamp"],-source[2]],10,req["max_lamp"])) if mode=="area" else [0.]
    width=plate["dims"]["w"]
    n=index(scene,plate)
    best=[]
    for g in gaps:
        for d in lamps:
            test=deepcopy(scene)
            apply_layout(test,screen["id"],mode,float(g),float(d),req["size"])
            if any(bad for bad,_,_ in diagnostics(test)):
                continue
            # Freeform slope estimated at the four aperture corners, before synthesis.
            deviations=[]
            for x,y in ((-1,-1),(1,-1),(1,1),(-1,1)):
                xy=np.array([x,y])*width/2
                goal=np.array([c[0],c[1],h+g])+b@np.array([x*screen["dims"]["w"]/2,y*screen["dims"]["d"]/2,0])
                incoming=np.r_[xy,d] if mode=="area" else np.array([0.,0.,1.])
                incoming=incoming/np.linalg.norm(incoming)
                inside_xy=incoming[:2]/n
                inside=np.r_[inside_xy,math.sqrt(1-inside_xy@inside_xy)]
                outgoing=goal-np.r_[xy,h]
                if np.linalg.norm(outgoing)<1e-8:
                    deviations.append(float("inf"))
                    continue
                normal=n*inside-outgoing/np.linalg.norm(outgoing)
                deviations.append(np.linalg.norm(normal[:2])/max(normal[2],1e-9))
            blur=g*(2*math.tan(math.radians(1919/3600/2)) if mode=="sun" else req["size"]/d)
            # Prefer low penumbra, small slopes, compact setup and moderate corner falloff.
            pitch=max(screen["dims"].values())/64
            slope=max(deviations)
            score=blur/pitch+.9*slope+.12*g/req["max_gap"]
            if mode=="area":
                score+=.2*width/d+.06*d/req["max_lamp"]
            best.append(dict(gap=float(g),lamp=float(d),blur=float(blur),score=float(score),
                             slope=float(slope),mode=mode,screen=screen["id"],size=req["size"]))
    if not best:
        raise ValueError("В заданном диапазоне все варианты конфликтуют с геометрией сцены")
    if not plate.get("profile"):
        return sorted(best,key=lambda row:row["score"])[:3]
    for j,row in enumerate(best):
        test=deepcopy(scene)
        apply_layout(test,row["screen"],mode,row["gap"],row["lamp"],row["size"])
        config=directory/f"{j}.txt"
        config.write_text(trace_settings(test,64,120000),encoding="ascii")
        out=directory/str(j)
        subprocess.run([ROOT/"build/Release/caustic_cli.exe","trace-scene",config,assets/plate["profile"],out],check=True)
        report=json.loads((out/"report.json").read_text())
        entry=report["screens"][screens.index(screen)]
        if entry["flux"]<=0:
            row["score"]=float("inf")
            continue
        row["capture"]=report["hit"]/report["rays"]
        if target is not None:
            import cv2
            with Image.open(out/entry["image"]) as im:
                encoded=np.asarray(im.convert("L"),dtype=float)/255
            actual=np.where(encoded<=.04045,encoded/12.92,((encoded+.055)/1.055)**2.4)
            goal=cv2.resize(target,(actual.shape[1],actual.shape[0]),interpolation=cv2.INTER_AREA)
            gain=np.sum(actual*goal)/max(1e-15,np.sum(actual**2))
            row["error"]=float(np.linalg.norm(actual*gain-goal)/max(1e-12,np.linalg.norm(goal)))
        row["score"]=row.get("error",0)+.12*(1-row["capture"])+.025*row["score"]
    feasible=[r for r in best if math.isfinite(r["score"])]
    if not feasible:
        raise ValueError("Ни в одном проверенном положении лучи не достигли выбранного экрана")
    return sorted(feasible,key=lambda row:row["score"])[:3]
