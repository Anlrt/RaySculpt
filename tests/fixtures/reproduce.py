"""Reproduce the A01 optical reference on Windows x64."""
import argparse
from copy import deepcopy
from hashlib import sha256
import json
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import cad_scene as model
from cad_fit import fit


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def record_files(folder):
    return {
        p.relative_to(folder).as_posix(): {"bytes": p.stat().st_size, "sha256": sha256(p.read_bytes()).hexdigest()}
        for p in sorted(folder.rglob("*")) if p.is_file()
    }


def make_cases(folder):
    flat = model.new_scene()
    yield "flat", flat, None, [0], 256, 2_000_000

    arrow = deepcopy(flat)
    target = folder / "arrow.png"
    picture = Image.new("L", (96, 96), 85)
    ImageDraw.Draw(picture).polygon([(18, 24), (53, 24), (53, 14), (80, 40),
                                     (53, 66), (53, 55), (18, 55)], fill=215)
    picture.save(target)
    arrow["objects"][2]["image"] = model.store_asset(target, folder / "assets")
    yield "arrow", arrow, "photo", [0], 96, 1_000_000

    portrait = model.load_scene(ROOT / "examples/mona_lisa.ccad", folder / "assets")
    yield "portrait", portrait, None, [0], 256, 2_000_000

    double = deepcopy(flat)
    screen = double["objects"][2]
    other = model.node("screen", 260.)
    other["rot"] = screen["rot"][:]
    center = np.array(screen["pos"])
    x, y = np.meshgrid(np.linspace(-1, 1, 40), np.linspace(-1, 1, 64))
    for j, item in enumerate([screen, other]):
        item["pos"] = (center + model.rotation(screen)[:, 0] * (-12.5 if j == 0 else 12.5)).tolist()
        item["dims"] = {"w": 25., "d": 40.}
        image = folder / f"screen-{j}.png"
        Image.fromarray(np.uint8(60 + 140 * np.exp(-4 * (x*x + (y-.45*(-1)**j)**2)))).save(image)
        item["image"] = model.store_asset(image, folder / "assets")
    double["objects"].append(other)
    yield "two_screens", double, "fit", [0], 96, 240_000

    turn = deepcopy(flat)
    mesh = ROOT / "examples/sample_asymmetric.obj"
    turn["turntable"] = dict(asset=model.store_asset(mesh, folder / "assets"),
                             name=mesh.name, screen=turn["objects"][2]["id"], angles=[-8., 8.])
    yield "two_angles", turn, "fit", [-8, 0, 8], 96, 240_000

    area = deepcopy(flat)
    area["objects"][1].update(kind="area", dims={"w": 8., "d": 7.})
    yield "area_source", area, None, [0], 48, 16_000


def run(out):
    cli = ROOT / "build/Release/caustic_reference.exe"
    production = ROOT / "build/Release/caustic_cli.exe"
    assets = out / "assets"
    assets.mkdir(parents=True)
    manifest = {"trace_threads": 12, "trace_seed": 1, "linear_encoding": "little-endian float64 row-major",
                "cases": {}}
    for name, scene, design, angles, resolution, rays in make_cases(out):
        print(name, flush=True)
        case = out / name
        case.mkdir()
        plate, _, screens = model.optical_setup(scene)
        if design == "photo":
            _, screen, source, distance = model.design_geometry(scene)
            subprocess.run([sys.executable, ROOT/"tools/design_photo.py", assets/screen["image"],
                            "--out", case/"design", "--grid", "96", "--map-size", "96",
                            "--resolution", "96", "--cells", "96", "--rays", "1000000",
                            "--corrections", "0", "--plate", str(plate["dims"]["w"]),
                            "--thickness", str(plate["dims"]["h"]), "--width", str(screen["dims"]["w"]),
                            "--index", str(model.index(scene, plate)), "--source", str(source),
                            "--screen", str(distance)], check=True)
            profile = case / "design/surface.txt"
        elif design == "fit":
            fit(scene, assets, case / "design")
            profile = case / "design/result/surface.txt"
        else:
            profile = assets / plate["profile"] if "profile" in plate else None

        if profile is not None:
            shutil.copyfile(profile, case / "surface.txt")
        cfg = case / "scene.txt"
        cfg.write_text(model.trace_settings(scene, resolution, rays), encoding="ascii")
        for angle in angles:
            if name == "two_angles":
                subprocess.run([production, "render-target", "--obj", ROOT/"examples/sample_asymmetric.obj",
                                "--angle", str(angle), "--elevation", "0", "--width", "96",
                                "--height", "96", "--out", case/f"target-{angle}.pgm"], check=True)
        if name == "two_angles":
            targets = [(case/f"target-{angle}.pgm").read_bytes() for angle in angles]
            if len(set(targets)) != len(angles):
                raise ValueError("Reference target views must differ across ordered angles")

        checks = []
        for angle in angles:
            fields = []
            for attempt in (1, 2):
                trace = case / f"trace-{angle}-{attempt}"
                subprocess.run([cli, "trace-scene", cfg, case/"surface.txt" if profile else "-",
                                trace, str(angle)], check=True)
                fields.append(record_files(trace))
            if fields[0] != fields[1]:
                raise ValueError(f"Reference reruns disagree for {name} at {angle} degrees")
            report = json.loads((case / f"trace-{angle}-1/report.json").read_text())
            checks.append({"angle_deg": angle, "report": report, "files": fields[0]})
        manifest["cases"][name] = {
            "settings": cfg.read_text(encoding="ascii").strip(),
            "resolution": resolution, "rays": rays,
            "material": scene["materials"][plate["material"]], "line": plate["line"],
            "profile": record_files(case).get("surface.txt"),
            "design_report": json.loads((case / ("design/report.json" if design == "photo" else
                "design/result/validation.json")).read_text()) if design else None,
            "checks": checks,
        }
    write_json(out / "manifest.json", manifest)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new directory for the reference artifacts")
    run(parser.parse_args().output.resolve())
