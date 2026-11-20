"""Compare indexed picking with original-mesh selection for B03."""
import argparse
import json
from pathlib import Path
import sys
import time

import numpy as np

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools"))
from cad_topology import mesh_intersection
from mesh_native import MeshIndex


def mesh(columns,rows):
    x,y=np.meshgrid(np.linspace(-20.,20.,columns+1),np.linspace(-20.,20.,rows+1))
    z=5.+.3*np.cos(2*np.pi*x/40.)*np.cos(2*np.pi*y/40.)
    vertices=np.column_stack((x.ravel(),y.ravel(),z.ravel()))
    corners=np.arange(len(vertices),dtype=np.int32).reshape(rows+1,columns+1)
    a,b,c,d=(plane.ravel() for plane in (corners[:-1,:-1],corners[:-1,1:],
                                         corners[1:,1:],corners[1:,:-1]))
    return vertices,np.concatenate((np.column_stack((a,b,c)),np.column_stack((a,c,d))))


def run(library,output):
    output.mkdir(parents=True)
    samples=np.random.default_rng(1).uniform(-19.,19.,(100,2))
    vectors=np.column_stack((samples,np.full(len(samples),20.)))
    direction=np.array([0.,0.,-1.])
    result=[]
    for shape in ((250,200),(1000,500)):
        xyz,tri=mesh(*shape)
        start=time.perf_counter()
        index=MeshIndex(library,xyz,tri)
        build=time.perf_counter()-start
        measurements=[]
        for ray in vectors:
            start=time.perf_counter()
            expected=mesh_intersection(xyz,tri,ray,direction)
            python=time.perf_counter()-start
            start=time.perf_counter()
            actual=index.nearest(ray,direction)
            native=time.perf_counter()-start
            if expected[0]!=actual[0] or abs(expected[1]-actual[1])>1e-8:
                raise ValueError(f"Triangle selection changed for {ray}")
            measurements.append((python*1000,native*1000))
        p50,p95=np.percentile(measurements,[50,95],axis=0).tolist()
        measurement=dict(triangles=len(tri),clicks=len(vectors),build_s=build,
                         python_p50_ms=p50[0],python_p95_ms=p95[0],
                         native_p50_ms=p50[1],native_p95_ms=p95[1])
        print(measurement,flush=True)
        result.append(measurement)
    (output/"mesh-picking.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library",type=Path)
    parser.add_argument("output",type=Path)
    args=parser.parse_args()
    run(args.library.resolve(),args.output.resolve())
