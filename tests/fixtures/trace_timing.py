"""D01 trace phase timings and thread scaling on the A01 portrait fixture."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess

import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def run(reference, output):
    output.mkdir(parents=True)
    engine=ROOT/"build/Release/caustic_reference.exe"
    profile=reference/"portrait/surface.txt"
    settings=(reference/"portrait/scene.txt").read_text(encoding="ascii").split()
    records=[]
    for rays in (16_000, 2_000_000, 8_000_000):
        settings[3]=str(rays)
        config=output/f"{rays}.txt"
        config.write_text(" ".join(settings),encoding="ascii")
        reference_field=None
        reference_counts=None
        for threads in (1,4,8,12,24):
            runs=[]
            for repeat in range(4):
                folder=output/f"{rays}-{threads}-{repeat}"
                subprocess.run([engine,"trace-scene",config,profile,folder,"0","1","",str(threads)],check=True)
                report=json.loads((folder/"report.json").read_text())
                counts={key:report[key] for key in ("rays","hit","tir","miss","side","rejected")}
                if sum(counts[k] for k in ("hit","tir","miss","side","rejected"))!=rays:
                    raise ValueError("trace outcome counts do not balance")
                if sum(screen["hits"] for screen in report["screens"])!=counts["hit"]:
                    raise ValueError("per-screen hit count disagrees with the global count")
                if reference_counts is None:
                    reference_counts=counts
                    reference_field=np.fromfile(folder/"1.f64",dtype="<f8")
                if counts!=reference_counts:
                    raise ValueError("changing the thread count changed the ray outcomes")
                image=np.fromfile(folder/"1.f64",dtype="<f8")
                timing=json.loads((folder/"timing.json").read_text())
                if (folder/"pool.json").exists():
                    timing.update(json.loads((folder/"pool.json").read_text()))
                timing["max_abs_field_delta"]=float(np.max(np.abs(image-reference_field)))
                runs.append(timing)
            entry={key:statistics.median(row[key] for row in runs[1:]) for key in runs[0]}
            entry["rays"]=rays
            records.append(entry)
            print(entry,flush=True)
    (output/"trace-timing.json").write_text(json.dumps(records,indent=2)+"\n",encoding="utf-8")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference",type=Path)
    parser.add_argument("output",type=Path)
    args=parser.parse_args()
    run(args.reference.resolve(),args.output.resolve())
