"""Bridge from the existing scene controller to the native Qt viewport."""
import ctypes as ct
import json
from pathlib import Path

import numpy as np
import shiboken6
from PySide6.QtOpenGLWidgets import QOpenGLWidget

from cad_scene import SOLIDS, rotation


class NativeViewport:
    def __init__(self, owner, library):
        self.owner=owner
        self.dll=ct.CDLL(str(Path(library).resolve()))
        create=self.bind("create",[ct.c_void_p],ct.c_void_p)
        self.send=self.bind("sync",[ct.c_void_p,ct.c_char_p,ct.c_size_t,ct.c_int],ct.c_int)
        self.query=self.bind("pick",[ct.c_void_p,ct.c_double,ct.c_double],ct.c_int)
        self.is_ready=self.bind("ready",[ct.c_void_p],ct.c_int)
        self.handle=create(shiboken6.getCppPointer(owner)[0])
        if not self.handle:
            raise RuntimeError("Unable to create the native viewport")
        self.widget=shiboken6.wrapInstance(self.handle,QOpenGLWidget)
        self.widget.setGeometry(owner.rect())
        self.versions={}
        self.serial=0
        self.keys=None
        self.failed=False

    def bind(self, name, args, result):
        method=getattr(self.dll,f"raysculpt_view_{name}_v1")
        method.argtypes=args
        method.restype=result
        return method

    def ready(self):
        return not self.failed and self.widget.isVisible() and bool(self.is_ready(self.handle))

    def sync(self):
        owner=self.owner
        if self.failed or owner.color_relief:
            self.widget.hide()
            return
        items=[item for item in owner.items if item["kind"] not in SOLIDS or item["id"] in owner.geometry]
        keys=[item["id"] for item in items]
        changed=self.keys!=keys
        versions={}
        for item in items:
            key=item["id"]
            geometry=owner.geometry.get(key)
            dims=tuple(sorted(item["dims"].items()))
            old=self.versions.get(key)
            if old and old[0] is geometry and old[1]==dims:
                versions[key]=old
            else:
                self.serial+=1
                versions[key]=(geometry,dims,self.serial)
                changed=True
        frame=dict(camera=dict(yaw_deg=owner.yaw,pitch_deg=owner.pitch,
                              center_mm=np.asarray(owner.center).tolist(),px_per_mm=owner.scale),
                   grid=owner.show_grid,selected=owner.selected,
                   highlights=[np.asarray(line).tolist() for line in owner.highlights],poses=[])
        for item in items:
            key=item["id"]
            frame["poses"].append(dict(id=key,label=item["name"],
                visible=not item.get("hidden") and key not in owner.consumed,
                error=key in owner.errors,matrix=np.column_stack((rotation(item),item["pos"])).ravel().tolist()))
        if changed:
            frame["objects"]=[]
            for item in items:
                key=item["id"]
                geometry,_,serial=versions[key]
                if geometry is not None:
                    xyz,tri,_=geometry
                elif item["kind"] in ("screen","area"):
                    width=item["dims"]["w"]/2
                    height=item["dims"]["d"]/2
                    xyz=np.array([[-width,-height,0],[width,-height,0],
                                  [width,height,0],[-width,height,0]])
                    tri=np.array([[0,1,2],[0,2,3]])
                else:
                    xyz=tri=np.empty((0,3))
                frame["objects"].append(dict(id=key,kind=item["kind"],label=item["name"],
                    version=str(serial),center=[0,0,0],vertices=xyz.tolist(),triangles=tri.tolist()))
        encoded=json.dumps(frame,separators=(",",":"),allow_nan=False).encode("utf-8")
        if not self.send(self.handle,encoded,len(encoded),int(changed)):
            self.failed=True
            self.widget.hide()
        else:
            self.keys=keys
            self.versions=versions
            if self.widget.isHidden():
                self.widget.show()

    def pick(self, position):
        index=self.query(self.handle,position.x(),position.y())
        return self.keys[index] if self.keys and 0<=index<len(self.keys) else None
