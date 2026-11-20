"""B01 capture of the software viewport on the A01 scenes."""
import argparse
import json
import os
from pathlib import Path
import sys
from time import perf_counter

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
os.environ["QT_QPA_PLATFORM"] = "offscreen"
from PySide6.QtCore import QPointF
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication
import cad_desktop as ui
import cad_scene as model
from cad_geometry import visual_geometry


def save_snapshot(view, path):
    objects = []
    for item in view.items:
        key = item["id"]
        if item.get("hidden") or key in view.consumed:
            continue
        if key in view.geometry:
            vertices, triangles, _ = view.geometry[key]
            vertices = vertices @ model.rotation(item).T + item["pos"]
        elif item["kind"] == "screen":
            vertices = view.vertices(item)
            triangles = np.array([[0, 1, 2], [0, 2, 3]])
        else:
            vertices = triangles = np.empty((0, 3))
        objects.append({"id": key, "kind": item["kind"], "label": item["name"], "center": item["pos"],
                        "movable": not item.get("locked") and "parent" not in item,
                        "vertices": vertices.tolist(), "triangles": triangles.tolist()})
    camera = {"yaw_deg": view.yaw, "pitch_deg": view.pitch, "center_mm": view.center.tolist(),
              "px_per_mm": view.scale}
    path.write_text(json.dumps({"objects": objects, "camera": camera, "grid": view.show_grid}),
                    encoding="utf-8")


def render(view):
    image = QImage(view.width(), view.height(), QImage.Format_RGBA8888)
    view.render(image)
    return image


def capture_motion(view, scene_name, out):
    view.fit(True)
    view.show_grid = True
    save_snapshot(view, out / f"{scene_name}.json")
    records = {}
    for grid in (True, False):
        view.show_grid = grid
        label = f"{scene_name}-{'grid' if grid else 'plain'}"
        frames = []
        for i in range(65):
            view.yaw = 36.0 + (i - 5) * .45
            start = perf_counter()
            image = render(view)
            elapsed = (perf_counter() - start) * 1000
            if i < 5:
                continue
            frames.append({"yaw_deg": view.yaw, "pitch_deg": view.pitch,
                           "center_mm": view.center.tolist(), "px_per_mm": view.scale, "ms": elapsed})
            if i in (5, 34, 64):
                if not image.save(str(out / f"{label}-{i-5:02}.png")):
                    raise OSError("Could not save the viewport frame")
        p50, p95 = np.percentile([frame["ms"] for frame in frames], [50, 95])
        records[label] = {"grid": grid, "p50_ms": float(p50), "p95_ms": float(p95), "frames": frames}
    return records


def capture_picks(view, assets, out):
    plate = model.node("plate")
    screen = model.node("screen")
    screen["dims"] = {"w": 50., "d": 50.}
    body = model.node("box")
    body["dims"].update(w=20., d=20., h=12.)
    body["pos"] = [20., 0., 0.]
    view.items = [plate, screen, body]
    view.geometry = {obj["id"]: visual_geometry(obj, assets) for obj in (plate, body)}
    view.yaw, view.pitch, view.scale = 0., 90., 7.
    view.center = np.zeros(3)
    view.selected = plate["id"]
    view.show_grid = True
    records = []
    cases = [
        ("screen-behind", -6., {body["id"]}, [(0,0,"plate"), (19,0,"plate"), (23,0,"screen"),
                                            (24,24,"screen"), (26,0,None)]),
        ("screen-in-front", 16., {body["id"]}, [(0,0,"screen"), (19,0,"screen"), (-23,0,"screen"),
                                              (-24,-24,"screen"), (-26,0,None)]),
        ("intersecting-solids", -6., set(), [(12,0,"box"), (18,0,"box"), (24,0,"box"),
                                             (27,0,"box"), (32,0,None)]),
        ("consumed-tool", -6., {body["id"]}, [(-18,-18,"plate"), (-21,0,"screen"),
                                              (27,0,None), (0,24,"screen"), (0,27,None)]),
    ]
    by_id = {obj["id"]: obj["kind"] for obj in view.items}
    for label, depth, consumed, points in cases:
        screen["pos"][2] = depth
        view.consumed = consumed
        save_snapshot(view, out / f"{label}.json")
        if not render(view).save(str(out / f"{label}.png")):
            raise OSError("Could not save the viewport selection frame")
        for x, y, expected in points:
            px, py, _ = view.project([x,y,0.])
            pixel = QPointF(float(px), float(py))
            records.append({"scene": label, "xy_px": [pixel.x(), pixel.y()],
                            "xy_mm": [x, y], "old_id": by_id.get(view.at(pixel)),
                            "visible_id": expected})
    return records


def run(out):
    out.mkdir(parents=True)
    app = QApplication([])
    ui.setup_style(app)
    window = ui.Window()
    view = window.cam
    view.setFixedSize(1600,900)
    result = {"viewport_px": [1600,900], "frames": capture_motion(view, "flat", out)}
    portrait = model.load_scene(ROOT/"examples/mona_lisa.ccad",window.assets)
    window.replace(portrait)
    view.yaw, view.pitch = 36.,26.
    result["frames"].update(capture_motion(view, "portrait", out))
    result["picks"] = capture_picks(view, window.assets, out)
    (out/"viewport.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    for label,frames in result["frames"].items():
        print(label,round(frames["p50_ms"],2),round(frames["p95_ms"],2))
    print(len(result["picks"]), "pick samples")
    window.saved = window.snapshot()
    window.close()


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path,help="new directory for viewport captures")
    run(parser.parse_args().output.resolve())
