"""C++ spatial index for the original triangle mesh."""
import ctypes as ct
from pathlib import Path

import numpy as np

DOUBLE = ct.POINTER(ct.c_double)
INDEX = ct.POINTER(ct.c_uint32)


class MeshIndex:
    def __init__(self, library, xyz, tri):
        dll=ct.CDLL(str(Path(library).resolve()))
        create=dll.raysculpt_mesh_index_create_v1
        create.argtypes=[DOUBLE,ct.c_size_t,INDEX,ct.c_uint32]
        create.restype=ct.c_void_p
        self.release=dll.raysculpt_mesh_index_release_v1
        self.release.argtypes=[ct.c_void_p]
        self.release.restype=None
        self.query=dll.raysculpt_mesh_index_pick_v1
        self.query.argtypes=[ct.c_void_p,DOUBLE,DOUBLE,INDEX,DOUBLE]
        self.query.restype=ct.c_int
        self.handle=None
        if not len(tri) or tri.min()<0 or tri.max()>=len(xyz):
            raise ValueError("Invalid triangle mesh")
        vertices=np.ascontiguousarray(xyz,dtype=np.float64)
        triangles=np.ascontiguousarray(tri,dtype=np.uint32)
        self.handle=create(vertices.ctypes.data_as(DOUBLE),len(vertices),
                           triangles.ctypes.data_as(INDEX),len(triangles))
        if not self.handle:
            raise ValueError("Could not build native mesh index")

    def __del__(self):
        if getattr(self,"handle",None):
            self.release(self.handle)

    def nearest(self, origin, direction):
        ray=np.ascontiguousarray(origin,dtype=np.float64)
        vector=np.ascontiguousarray(direction,dtype=np.float64)
        triangle=ct.c_uint32()
        distance=ct.c_double()
        found=self.query(self.handle,ray.ctypes.data_as(DOUBLE),vector.ctypes.data_as(DOUBLE),
                         ct.byref(triangle),ct.byref(distance))
        if found!=1:
            raise ValueError("Укажите участок исходной сетки")
        return triangle.value,distance.value
