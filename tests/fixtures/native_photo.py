"""C03 photo numerics parity, elapsed time and peak parent working set."""
import argparse
import ctypes as ct
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import sys
import time
from zipfile import ZipFile

import numpy as np

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools"))
from export_plate import Profile
from plate_cad import gp,optical_face


class Counters(ct.Structure):
    _fields_=[("size",wintypes.DWORD),("faults",wintypes.DWORD)]+[
        (field,ct.c_size_t) for field in ("peak","working","peak_paged","paged","peak_nonpaged",
                                        "nonpaged","pagefile","peak_pagefile")]


def measured(command):
    kernel=ct.WinDLL("kernel32",use_last_error=True)
    open_process=kernel.OpenProcess
    open_process.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    open_process.restype=wintypes.HANDLE
    kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    memory=ct.WinDLL("psapi",use_last_error=True).GetProcessMemoryInfo
    memory.argtypes=[wintypes.HANDLE,ct.POINTER(Counters),wintypes.DWORD]
    memory.restype=wintypes.BOOL
    start=time.perf_counter()
    wrapper=("import os,runpy,sys; print(os.getpid(),flush=True); "
             "sys.argv=sys.argv[1:]; sys.path.insert(0,os.path.dirname(sys.argv[0])); "
             "runpy.run_path(sys.argv[0],run_name='__main__')")
    child=subprocess.Popen([command[0],"-c",wrapper,*command[1:]],stdout=subprocess.PIPE)
    # A Windows virtualenv can run a launcher process. Measure the PID that
    # actually imports and runs the numerical solver.
    pid=child.stdout.readline()
    if not pid:
        raise subprocess.CalledProcessError(child.wait(),command)
    handle=open_process(0x0400|0x0010,False,int(pid))
    peak=0
    if not handle:
        child.wait()
        raise ct.WinError(ct.get_last_error())
    try:
        counts=Counters(size=ct.sizeof(Counters))
        while child.poll() is None:
            if memory(handle,ct.byref(counts),counts.size):
                peak=max(peak,counts.peak)
            time.sleep(.02)
    finally:
        kernel.CloseHandle(handle)
    child.stdout.close()
    if child.returncode:
        raise subprocess.CalledProcessError(child.returncode,command)
    return time.perf_counter()-start,peak


def run(worker,adapter,output):
    output.mkdir(parents=True)
    with ZipFile(ROOT/"examples/mona_lisa.ccad") as archive:
        scene=json.loads(archive.read("scene.json"))
        key=next(item["image"] for item in scene["objects"] if "image" in item)
        image=output/"portrait.img"
        image.write_bytes(archive.read("assets/"+key))
    metrics={}
    profiles={}
    for name,options in (("python",[]),("native",["--transport-worker",str(worker),
                                                 "--poisson-adapter",str(adapter)])):
        folder=output/name
        command=[sys.executable,ROOT/"tools/design_photo.py",image,"--out",folder,
                 "--grid","320","--map-size","320","--resolution","256","--cells","320",
                 "--rays","8000000","--max-relief","7.2","--corrections","1",*options]
        elapsed,peak=measured(command)
        report=json.loads((folder/"report.json").read_text(encoding="utf-8"))
        metrics[name]=dict(seconds=elapsed,peak_design_process_bytes=peak,
                           rms=report["picture_relative_rmse"],pass_number=report["selected_pass"],
                           scores=report["calibration_scores"])
        profiles[name]=Profile.read(folder)
    reference,actual=profiles["python"],profiles["native"]
    surf_a,surf_b=optical_face(reference),optical_face(actual)
    checks=[]
    extent=reference.width/2
    for x,y in ((0,0),(0,-extent),(0,extent),(-extent,0),(extent,0),
                (-extent,-extent),(-extent,extent),(extent,-extent),(extent,extent)):
        pairs=[]
        for surface in (surf_a,surf_b):
            point,u,v=gp.gp_Pnt(),gp.gp_Vec(),gp.gp_Vec()
            surface.D1(x,y,point,u,v)
            pairs.append((point.Z(),np.array(u.Crossed(v).Normalized().Coord())))
        checks.append(dict(x=x,y=y,height_delta_mm=abs(pairs[0][0]-pairs[1][0]),
                           normal_delta=float(np.linalg.norm(pairs[0][1]-pairs[1][1]))))
    metrics["points"]=checks
    metrics["max_coefficient_delta_mm"]=float(np.max(np.abs(reference.relief-actual.relief)))
    (output/"native-photo.json").write_text(json.dumps(metrics,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(metrics))
    if metrics["native"]["rms"]-metrics["python"]["rms"]>.001:
        raise ValueError("Native photometric stages increased independent RMS by more than 0.001")
    if metrics["native"]["pass_number"]!=metrics["python"]["pass_number"]:
        raise ValueError("Native photometric stages selected a different calibration pass")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("worker",type=Path)
    parser.add_argument("adapter",type=Path)
    parser.add_argument("output",type=Path)
    a=parser.parse_args()
    run(a.worker.resolve(),a.adapter.resolve(),a.output.resolve())
