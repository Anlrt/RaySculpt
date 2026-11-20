"""Smoke test the live editor with the optional native widget on Windows."""
import argparse
import os
from pathlib import Path
import sys
import time

import numpy as np

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools"))
from PySide6.QtCore import QPointF
from PySide6.QtWidgets import QApplication
import cad_desktop as ui


def run(library,output):
    os.environ["RAYSCULPT_GPU_WIDGET"]=str(library.resolve())
    app=QApplication([])
    ui.setup_style(app)
    win=ui.Window()
    win.resize(1450,950)
    win.show()
    plate,source,screen=win.scene["objects"]
    def arrange():
        plate.update(pos=[0.,0.,0.],rot=[0.,0.,0.])
        source.update(pos=[0.,0.,-240.],rot=[0.,0.,0.])
        screen.update(pos=[0.,0.,-6.],rot=[0.,0.,0.])
    win.mutate(arrange)
    try:
        view=win.cam
        view.yaw,view.pitch,view.scale=0.,90.,7.
        view.center=np.zeros(3)
        view.update()
        deadline=time.monotonic()+5
        while (not view.native or not view.native.ready()) and time.monotonic()<deadline:
            app.processEvents()
            time.sleep(.02)
        if not view.native or not view.native.ready():
            raise RuntimeError("live editor did not initialize native OpenGL")
        cx,cy=view.project([0.,0.,0.])[:2]
        center=QPointF(cx,cy)
        if view.at(center)!=plate["id"]:
            raise AssertionError("rear screen covered the selected plate")
        win.select(screen["id"])
        if win.tree.currentItem() is not win.tree_rows[screen["id"]]:
            raise AssertionError("selection did not update the model tree")
        win.mutate(lambda:screen.update(pos=[0.,0.,16.]))
        if view.at(center)!=screen["id"]:
            raise AssertionError("front screen did not follow its pose")
        win.undo()
        if view.at(center)!=plate["id"]:
            raise AssertionError("undo did not restore the depth order")
        view.color_relief=True
        view.update()
        if view.native.widget.isVisible():
            raise AssertionError("height map did not activate its software fallback")
        view.color_relief=False
        view.update()
        app.processEvents()
        if not view.native.ready():
            raise AssertionError("native view did not resume after the software fallback")
        if not view.native.widget.grabFramebuffer().save(str(output.resolve())):
            raise OSError("could not save a native editor frame")
        print("native editor drawing, depth selection, tree, transform, undo and fallback passed")
    finally:
        win.saved=win.snapshot()
        win.close()


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll",type=Path)
    parser.add_argument("capture",type=Path)
    args=parser.parse_args()
    run(args.dll,args.capture)
