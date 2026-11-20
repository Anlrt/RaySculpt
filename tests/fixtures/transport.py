"""C02 comparisons of Python and Rust bilinear inverse transport."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time
from zipfile import ZipFile

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from design_photo import forward_transport, forward_transport_native, inverse_transport, monochrome


def measure(function):
    times = []
    for _ in range(3):
        start = time.perf_counter()
        arrays = function()
        times.append(time.perf_counter() - start)
    return statistics.median(times), arrays


def run(worker, out):
    out.mkdir(parents=True)
    with ZipFile(ROOT/"examples/mona_lisa.ccad") as archive:
        scene = json.loads(archive.read("scene.json"))
        key = next(obj["image"] for obj in scene["objects"] if "image" in obj)
        image = out/"portrait.img"
        image.write_bytes(archive.read("assets/"+key))
    portrait = monochrome(image,256)
    report = []
    for grid in (96, 320, 512):
        gradient=np.broadcast_to(np.linspace(.15,.65,grid),(grid,grid))
        photo=cv2.resize(portrait[::-1],(grid,grid),interpolation=cv2.INTER_AREA)
        photo=np.hypot(photo,.08*photo.mean())
        axis=np.linspace(-20.,20.,grid+1)
        mx,my=np.meshgrid(axis,axis)
        for name, mapping in (("flat",(mx,my)),("gradient",inverse_transport(gradient,50.,50.,40.,100)),
                              ("portrait",inverse_transport(photo,50.,50.,40.,100))):
            case=out/f"{grid}-{name}"
            case.mkdir()
            params=(*mapping,grid,40.,50.,50.)
            py,expected=measure(lambda:forward_transport(*params))
            single=forward_transport_native(worker,*params,threads=1)
            rust,actual=measure(lambda:forward_transport_native(worker,*params))
            max_error=max(float(np.max(np.abs(a-b))) for a,b in zip(expected[2:],actual[2:]))
            if max_error>50e-6 or any(not np.array_equal(a,b) for a,b in zip(single[2:],actual[2:])):
                raise ValueError(f"Rust transport differs for {name}, grid {grid}")
            np.savez(case/"reference.npz",mx=mapping[0],my=mapping[1],tx=expected[2],ty=expected[3])
            row=dict(scene=name,grid=grid,python_s=py,rust_s=rust,speedup=py/rust,max_error_mm=max_error)
            print(row,flush=True)
            report.append(row)
        np.savez(out/f"{grid}-folded.npz",mx=-mx,my=my)
        for solver in (forward_transport,lambda *args:forward_transport_native(worker,*args)):
            try:
                solver(-mx,my,grid,40.,50.,50.)
            except ValueError as failure:
                if "Transport folded" not in str(failure):
                    raise
            else:
                raise AssertionError("folded map was accepted")
    output_reports=[]
    for name,arguments in (("python",[]),("rust",["--transport-worker",str(worker)])):
        folder=out/name
        subprocess.run([sys.executable,ROOT/"tools/design_photo.py",image,"--out",folder,
                        "--grid","320","--map-size","320","--resolution","256","--cells","320",
                        "--rays","8000000","--max-relief","7.2","--corrections","1",*arguments],check=True)
        output_reports.append(json.loads((folder/"report.json").read_text())["picture_relative_rmse"])
    if output_reports[1]-output_reports[0]>.001:
        raise ValueError("native transport increased the final relative RMS beyond 0.001")
    (out/"transport.json").write_text(json.dumps(dict(cases=report,photo_rms=output_reports),indent=2)+"\n")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("worker",type=Path)
    parser.add_argument("output",type=Path)
    args=parser.parse_args()
    run(args.worker.resolve(),args.output.resolve())
