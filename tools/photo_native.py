"""Opt-in bindings to the native photometric solvers."""
import ctypes as ct
from pathlib import Path

import numpy as np

POINTER = ct.POINTER(ct.c_double)
SIZE = ct.c_size_t
DOUBLE = ct.c_double
FILTER = ct.CFUNCTYPE(ct.c_int, POINTER, POINTER, POINTER, SIZE, SIZE, DOUBLE)


def bind(library, symbol, arguments):
    function = getattr(library, symbol)
    function.argtypes = arguments
    function.restype = ct.c_int
    return function


def buffer(data):
    return np.ascontiguousarray(data,dtype=np.float64)


def pointer(data):
    return data.ctypes.data_as(POINTER)


def checked(status, rejection):
    if status:
        raise ValueError({
            1: "Invalid native optical input",
            2: rejection,
            3: "OpenCV could not apply the spectral filter",
            4: "Native optical solver failed",
        }.get(status,"Unknown native optical solver error"))


class NativePhotoNumerics:
    def __init__(self, library, adapter):
        self.library=ct.CDLL(str(Path(library).resolve()))
        self.adapter=ct.CDLL(str(Path(adapter).resolve()))
        self.filter=FILTER(("raysculpt_spectral_filter_v1",self.adapter))
        self.solve_map=bind(self.library,"raysculpt_inverse_transport_v1",
                            [POINTER,SIZE,DOUBLE,DOUBLE,DOUBLE,SIZE,POINTER,POINTER,FILTER])
        self.solve_profile=bind(self.library,"raysculpt_optical_profile_v1",
                                [POINTER,POINTER,SIZE,DOUBLE,DOUBLE,DOUBLE,DOUBLE,DOUBLE,POINTER,FILTER])
        self.correct=bind(self.library,"raysculpt_calibrate_v1",
                          [POINTER,POINTER,POINTER,POINTER,SIZE,SIZE,DOUBLE,DOUBLE,POINTER,POINTER,FILTER])

    def inverse(self, field, width, height, aperture, steps):
        source=buffer(field)
        if source.ndim!=2 or source.shape[0]!=source.shape[1]:
            raise ValueError("Expected a square optical target lattice")
        n=source.shape[0]
        x,y=(np.empty((n+1,n+1)) for _ in range(2))
        status=self.solve_map(pointer(source),n,width,height,aperture,steps,
                              pointer(x),pointer(y),self.filter)
        checked(status,"Transport orientation could not be restored within the iteration budget.")
        return x,y

    def profile(self, tx, ty, aperture, thickness, index, source, screen):
        x,y=buffer(tx),buffer(ty)
        if x.ndim!=2 or x.shape[0]!=x.shape[1] or x.shape!=y.shape:
            raise ValueError("Expected matching square transport arrays")
        result=np.empty_like(x)
        status=self.solve_profile(pointer(x),pointer(y),x.shape[0],aperture,thickness,index,
                                  source,screen,pointer(result),self.filter)
        checked(status,"Exit surface cannot support the requested directions.")
        return result

    def calibrate(self, actual, target, goal, drive, error, floor):
        a,t,g,d=map(buffer,(actual,target,goal,drive))
        if a.ndim!=2 or not a.shape==t.shape==g.shape==d.shape:
            raise ValueError("Expected matching calibration images")
        output=np.empty_like(a)
        score=DOUBLE()
        status=self.correct(pointer(a),pointer(t),pointer(g),pointer(d),*a.shape,error,floor,
                            ct.byref(score),pointer(output),self.filter)
        checked(status,"Native calibration could not be completed")
        return score.value,output
