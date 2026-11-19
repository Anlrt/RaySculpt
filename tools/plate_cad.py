"""Exact solid construction for the two cardinal-spline edge conventions."""
from pathlib import Path
import sys

import numpy as np

local_libs = Path(__file__).resolve().parents[1] / "build" / "cad-libs"
if local_libs.is_dir():
    sys.path.insert(0,str(local_libs))
try:
    from OCP import (BRep, BRepBuilderAPI as Build, BRepCheck, BRepLib, BRepTools,
                     Geom, IFSelect, Interface, STEPControl, TopAbs, TopExp, TopoDS,
                     collections, gp)
except ImportError as exc:
    raise ImportError("CAD export needs cadquery-ocp-novtk 8.0.1.1.0. See README.") from exc


def clamp_axis(values, linear_ends):
    """Insert the two end knots to full multiplicity without fitting the data."""
    near, far = values[0], values[-1]
    if not linear_ends:
        near = (5*values[0]+values[1])/6
        far = (5*values[-1]+values[-2])/6
    return np.concatenate(([near, (2*values[0]+values[1])/3], values[1:-1],
                           [(2*values[-1]+values[-2])/3, far]))


def optical_face(profile):
    z = clamp_axis(clamp_axis(profile.relief, profile.linear_ends).T, profile.linear_ends)
    n = profile.grid
    extent = profile.width/2
    axis = np.linspace(-extent, extent, n)
    pole_axis = clamp_axis(axis, True)
    points = collections.Array2_gp_Pnt(1,n+2,1,n+2)
    knots = collections.Array1_double(1,n)
    mult = collections.Array1_int(1,n)
    for i,value in enumerate(axis,1):
        knots.SetValue(i,value)
        mult.SetValue(i,4 if i in (1,n) else 1)
    for i,x in enumerate(pole_axis,1):
        for j,(y,h) in enumerate(zip(pole_axis,z[i-1]),1):
            points.SetValue(i,j,gp.gp_Pnt(x,y,profile.thickness+h))
    return Geom.Geom_BSplineSurface(points,knots,knots,mult,mult,3,3)


def components(shape, kind):
    explorer = TopExp.TopExp_Explorer(shape,kind)
    while explorer.More():
        yield explorer.Current()
        explorer.Next()


def solid_for(profile):
    surface = optical_face(profile)
    low,high = -profile.width/2,profile.width/2
    corners = [(low,low),(high,low),(high,high),(low,high)]
    lower = [gp.gp_Pnt(x,y,0) for x,y in corners]
    upper = [surface.Value(x,y) for x,y in corners]
    bottom = [Build.BRepBuilderAPI_MakeEdge(lower[i],lower[(i+1)%4]).Edge() for i in range(4)]
    vertical = [Build.BRepBuilderAPI_MakeEdge(a,b).Edge() for a,b in zip(lower,upper)]
    top = [Build.BRepBuilderAPI_MakeEdge(curve).Edge() for curve in
           (surface.VIso(low),surface.UIso(high),surface.VIso(high),surface.UIso(low))]
    def reverse(edge):
        return TopoDS.TopoDS.Edge(edge.Reversed())
    top[2],top[3] = reverse(top[2]),reverse(top[3])
    def planar_face(edges):
        wire = Build.BRepBuilderAPI_MakeWire()
        for edge in edges:
            wire.Add(edge)
        return Build.BRepBuilderAPI_MakeFace(wire.Wire(),True).Face()
    sew = Build.BRepBuilderAPI_Sewing(1e-7)
    sew.Add(Build.BRepBuilderAPI_MakeFace(surface,1e-7).Face())
    sew.Add(planar_face(bottom).Reversed())
    for i in range(4):
        sew.Add(planar_face((bottom[i],vertical[(i+1)%4],reverse(top[i]),reverse(vertical[i]))))
    sew.Perform()
    shells = list(components(sew.SewedShape(),TopAbs.TopAbs_SHELL))
    if len(shells)!=1 or sew.NbFreeEdges() or sew.NbMultipleEdges():
        raise ValueError("CAD faces did not form one watertight shell")
    solid = Build.BRepBuilderAPI_MakeSolid(TopoDS.TopoDS.Shell(shells[0])).Solid()
    if not BRepLib.BRepLib.OrientClosedSolid_s(solid) or not BRepCheck.BRepCheck_Analyzer(solid).IsValid():
        raise ValueError("Invalid optical solid")
    return solid


def write_solid(solid, path):
    if path.suffix==".brep":
        if not BRepTools.BRepTools.Write_s(solid,str(path)):
            raise OSError("Open CASCADE could not write BREP")
        return
    done = IFSelect.IFSelect_RetDone
    writer = STEPControl.STEPControl_Writer()
    Interface.Interface_Static.SetCVal_s("write.step.unit","MM")
    if writer.Transfer(solid,STEPControl.STEPControl_ManifoldSolidBrep)!=done or writer.Write(str(path))!=done:
        raise OSError("Open CASCADE could not write STEP")


def read_optical_face(path):
    if path.suffix==".brep":
        shape = TopoDS.TopoDS_Shape()
        if not BRepTools.BRepTools.Read_s(shape,str(path),BRep.BRep_Builder()):
            raise ValueError("Invalid BREP file")
    else:
        reader = STEPControl.STEPControl_Reader()
        if reader.ReadFile(str(path))!=IFSelect.IFSelect_RetDone or reader.TransferRoots()!=1:
            raise ValueError("STEP import failed")
        shape = reader.OneShape()
    solids = list(components(shape,TopAbs.TopAbs_SOLID))
    faces = list(components(shape,TopAbs.TopAbs_FACE))
    if len(solids)!=1 or len(faces)!=6 or not BRepCheck.BRepCheck_Analyzer(shape).IsValid():
        raise ValueError("Expected one valid solid with six faces")
    surfaces = [BRep.BRep_Tool.Surface_s(TopoDS.TopoDS.Face(f)) for f in faces]
    optical = [s for s in surfaces if isinstance(s,Geom.Geom_BSplineSurface)]
    if len(optical)!=1 or sum(isinstance(s,Geom.Geom_Plane) for s in surfaces)!=5:
        raise ValueError("Unexpected face geometry")
    return optical[0]
