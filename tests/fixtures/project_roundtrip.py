"""Verify the native Qt project/material workflow with both CCAD versions."""
import argparse
from copy import deepcopy
from hashlib import sha256
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools"))
import cad_scene as model


def run(editor,worker,output):
    output.mkdir(parents=True)
    assets=output/"assets"
    assets.mkdir()
    original=ROOT/"examples/mona_lisa.ccad"
    current=model.load_scene(original,assets)
    current["version"]=2
    new=output/"версия-2.ccad"
    model.save_scene(current,assets,new)
    legacy=deepcopy(current)
    legacy["version"]=1
    old=output/"версия-1.ccad"
    model.save_scene(legacy,assets,old)
    restored=output/"reopened"
    restored.mkdir()
    for source,scene in ((old,legacy),(new,current)):
        before=sha256(source.read_bytes()).digest()
        copy=output/f"копия-{scene['version']}.ccad"
        subprocess.run([editor,worker,"--check",source,copy],check=True,timeout=40)
        expected=deepcopy(scene)
        body=next(item for item in expected["objects"] if "material" in item)
        body["material"]="bk7"
        body["line"]=expected["materials"]["bk7"]["default"]
        received=model.load_scene(copy,restored)
        if received!=expected or sha256(source.read_bytes()).digest()!=before:
            raise AssertionError(f"Native CCAD v{scene['version']} round-trip modified unexpected fields")
        print(f"CCAD v{scene['version']}: materials edited, assets verified, original preserved")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("editor",type=Path)
    parser.add_argument("worker",type=Path)
    parser.add_argument("output",type=Path)
    a=parser.parse_args()
    run(a.editor.resolve(),a.worker.resolve(),a.output.resolve())
