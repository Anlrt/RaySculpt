"""Export a calculated plate from surface.txt, a design report, or its folder."""
import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import numpy as np

from plate_files import PlateMesh


FORMATS = ("step","brep","stl","3mf","obj")
CAD = {"step","brep"}


def formats_list(value):
    names = tuple(dict.fromkeys(x.strip().lower() for x in value.split(",")))
    if not names or any(x not in FORMATS for x in names):
        raise ValueError("Choose formats from " + ", ".join(FORMATS))
    return names


@dataclass
class Profile:
    width: float
    thickness: float
    relief: np.ndarray
    linear_ends: bool
    mesh_path: Path | None = None

    @property
    def grid(self):
        return len(self.relief)

    @classmethod
    def read(cls,path):
        if path.is_dir():
            path = path/("surface.txt" if (path/"surface.txt").is_file() else "report.json")
        if path.suffix.lower()==".json":
            data = json.loads(path.read_text(encoding="utf-8-sig"))
            if "spline_coefficients_mm" not in data and (path.parent/"surface.txt").is_file():
                return cls.read(path.parent/"surface.txt")
            if data.get("surface")!="uniform_cubic_b_spline":
                raise ValueError("The report does not contain a supported optical spline")
            n,width,thickness = data["grid"],data["plate_mm"],data["thickness_mm"]
            values = np.asarray(data["spline_coefficients_mm"],dtype=float)
            linear = False
            mesh = path.parent/"plate.stl"
        else:
            with path.open(encoding="ascii") as f:
                header = f.readline().split()
                if len(header)!=8:
                    raise ValueError("Expected an eight-field photographic surface header")
                n,width,thickness = int(header[0]),float(header[1]),float(header[2])
                values = np.loadtxt(f)
            linear = True
            mesh = path.parent/"02_plate.stl" if path.name=="surface.txt" else None
        if not isinstance(n,int) or not 3<=n<=1024 or values.size!=n*n:
            raise ValueError("Invalid spline grid")
        if not (math.isfinite(width) and width>0 and math.isfinite(thickness) and thickness>0
                and np.isfinite(values).all() and thickness+values.min()>0):
            raise ValueError("Invalid plate geometry")
        return cls(width,thickness,values.reshape(n,n),linear,mesh if mesh and mesh.is_file() else None)


def write_models(profile, folder, names, stem, cells=None):
    if cells is not None and not 2<=cells<=2048:
        raise ValueError("Mesh cells must be between 2 and 2048")
    if CAD.intersection(names):
        from plate_cad import solid_for,write_solid
        solid = solid_for(profile)
        for name in names:
            if name in CAD:
                write_solid(solid,folder/f"{stem}.{name}")
    mesh = None
    for name in names:
        if name in CAD:
            continue
        path = folder/f"{stem}.{name}"
        reuse = cells is None and profile.mesh_path is not None
        if reuse and name=="stl":
            if path.resolve()!=profile.mesh_path.resolve():
                shutil.copyfile(profile.mesh_path,path)
            continue
        if mesh is None:
            mesh = PlateMesh.from_saved_stl(profile.mesh_path) if reuse else PlateMesh.sample(
                profile,cells if cells is not None else max(256,profile.grid-1))
        mesh.write(path)


def export(source,out,formats=None,cells=None):
    if out.exists():
        raise FileExistsError(f"Output already exists: {out}")
    ext = out.suffix.lower().lstrip(".")
    if ext=="stp":
        ext="step"
    one_file = ext in FORMATS
    if ext and not one_file and formats is None:
        raise ValueError("Unsupported model extension: " + out.suffix)
    names = formats or ((ext,) if one_file else ("step",))
    if one_file and names!=(ext,):
        raise ValueError("Use a directory for multiple formats, or match the filename extension")
    profile = Profile.read(source)
    out.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".plate-export-",dir=out.parent) as tmp:
        folder = Path(tmp)/"models"
        folder.mkdir()
        write_models(profile,folder,names,"plate",cells)
        result = folder/("plate."+ext) if one_file else folder
        if out.exists():
            raise FileExistsError(out)
        result.rename(out)
    return names


def finalize_design(folder, names, stem):
    write_models(Profile.read(folder),folder,names,stem)
    if "stl" not in names:
        (folder/f"{stem}.stl").unlink(missing_ok=True)
    report = folder/"report.json"
    data = json.loads(report.read_text())
    data["model_files"] = [f"{stem}.{ext}" for ext in names]
    report.write_text(json.dumps(data,indent=2)+"\n",encoding="utf-8")


def native_design(cli,arguments):
    parser = argparse.ArgumentParser(add_help=False,allow_abbrev=False)
    parser.add_argument("--formats",type=formats_list,required=True)
    parser.add_argument("--out",type=Path,required=True)
    args,rest = parser.parse_known_args(arguments)
    if args.out.exists():
        raise FileExistsError(f"Output already exists: {args.out}")
    if CAD.intersection(args.formats):
        import plate_cad  # Check the installation before running the optical design.
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.out.parent,prefix=".plate-design-") as tmp:
        folder = Path(tmp)/"result"
        needs_mesh = bool(set(args.formats)-CAD)
        executable = str(Path(cli).resolve()) if Path(cli).is_file() else cli
        subprocess.run([executable,"design-plate",*rest,"--out",str(folder),
                        "--write-stl",str(int(needs_mesh))],check=True)
        finalize_design(folder,args.formats,"plate")
        folder.rename(args.out)


if __name__=="__main__":
    if sys.argv[1:2]==["_design"]:
        try:
            native_design(sys.argv[2],sys.argv[3:])
        except (OSError,ValueError,KeyError,ImportError,RuntimeError,subprocess.CalledProcessError) as exc:
            sys.exit(str(exc))
        sys.exit(0)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source",type=Path)
    parser.add_argument("--out",type=Path,required=True,help="a new file or a new directory")
    parser.add_argument("--formats",type=formats_list,help="comma-separated: " + ",".join(FORMATS))
    parser.add_argument("--cells",type=int,help="remesh using this many cells per side")
    args = parser.parse_args()
    try:
        saved = export(args.source,args.out,args.formats,args.cells)
        print("Saved",", ".join(saved),"to",args.out)
    except (OSError, ValueError, KeyError, ImportError, RuntimeError) as error:
        sys.exit(str(error))
