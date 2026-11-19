"""Read back a STEP/BREP optical solid and compare it with the saved design."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

import numpy as np

from export_plate import Profile
from plate_cad import clamp_axis, gp, read_optical_face


def verify(profile, file):
    s = read_optical_face(file)
    n = profile.grid
    if (s.UDegree(),s.VDegree(),s.NbUPoles(),s.NbVPoles(),s.NbUKnots(),s.NbVKnots())!=(3,3,n+2,n+2,n,n):
        raise ValueError("Unexpected spline structure")
    if s.IsURational() or s.IsVRational() or s.IsUPeriodic() or s.IsVPeriodic():
        raise ValueError("Unexpected weights or periodicity")
    low,high = -profile.width/2,profile.width/2
    axis = np.linspace(low,high,n)
    for i,t in enumerate(axis,1):
        multiplicity = 4 if i in (1,n) else 1
        if (max(abs(s.UKnot(i)-t),abs(s.VKnot(i)-t))>1e-8 or s.UMultiplicity(i)!=multiplicity
                or s.VMultiplicity(i)!=multiplicity):
            raise ValueError("Unexpected knots or units")
    grid = np.array([[s.Pole(i,j).Coord() for j in range(1,n+3)] for i in range(1,n+3)])
    xy = clamp_axis(axis,True)
    z = clamp_axis(clamp_axis(profile.relief,profile.linear_ends).T,profile.linear_ends)+profile.thickness
    max_pole = max(np.max(abs(grid[:,:,0]-xy[:,None])),np.max(abs(grid[:,:,1]-xy)),
                   np.max(abs(grid[:,:,2]-z)))
    # Evaluate the CAD kernel's own derivatives, including the two axes and
    # four aperture edges. The reference here is the native cardinal basis.
    rand = np.random.default_rng(81621)
    x,y = rand.uniform(low,high,(2,8192))
    x = np.concatenate((x,axis,axis,np.full(n,low),np.full(n,high)))
    y = np.concatenate((y,np.full(n,low),np.full(n,high),axis,axis))
    def weights(q):
        pos = np.clip((q/profile.width+.5)*(n-1),0,n-1)
        i = np.minimum(pos.astype(int),n-2)
        t = pos-i
        a = 1-t
        w = [a**3/6,(4+3*t*t*(t-2))/6,(4+3*a*a*(a-2))/6,t**3/6]
        d = [-a*a/2,1.5*t*t-2*t,2*a-1.5*a*a,t*t/2]
        if profile.linear_ends:
            for row in (w,d):
                left,right = np.where(i==0,row[0],0),np.where(i==n-2,row[3],0)
                row[1] += left-right
                row[2] += right-left
        return [np.clip(i+k,0,n-1) for k in (-1,0,1,2)],w,d
    ix,wx,dx = weights(x)
    iy,wy,dy = weights(y)
    def combine(a,b):
        return sum(a[i]*b[j]*profile.relief[iy[j],ix[i]] for j in range(4) for i in range(4))
    height = profile.thickness+combine(wx,wy)
    scale = (n-1)/profile.width
    expected = np.column_stack((-scale*combine(dx,wy),-scale*combine(wx,dy),np.ones(len(x))))
    points = np.empty((len(x),3))
    normals = np.empty_like(points)
    p,u,v = gp.gp_Pnt(),gp.gp_Vec(),gp.gp_Vec()
    for k,(qx,qy) in enumerate(zip(x,y)):
        s.D1(qx,qy,p,u,v)
        points[k] = p.Coord()
        normals[k] = u.Crossed(v).Coord()
    max_height = np.max(abs(points[:,2]-height))
    max_angle = np.max(np.degrees(np.arctan2(np.linalg.norm(np.cross(normals,expected),axis=1),
                                             np.sum(normals*expected,axis=1))))
    if max_pole>1e-7 or max_height>1e-7 or max_angle>1e-5:
        raise ValueError("The imported CAD surface differs from the optical design")
    def unclamp(poles):
        # From P1=(2*C0+C1)/3, P[n]=(2*Cend+Cprev)/3.
        return np.concatenate(([(3*poles[1]-poles[2])/2],poles[2:-2],
                               [(3*poles[-2]-poles[-3])/2]))
    recovered = unclamp(unclamp(grid[:,:,2]).T)-profile.thickness
    stats = dict(solids=1,faces=6,optical_faces=1,poles=int((n+2)**2),samples=len(x),
                 max_pole_component_error_mm=float(max_pole),max_height_error_mm=float(max_height),
                 max_normal_error_deg=float(max_angle))
    return stats,recovered


if __name__=="__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("design",type=Path)
    parser.add_argument("cad",type=Path)
    parser.add_argument("--target",type=Path,help="linear PGM for an optional photographic verification trace")
    parser.add_argument("--out",type=Path,help="new directory for the verification trace")
    parser.add_argument("--rays",type=int,default=36_000_000)
    parser.add_argument("--cli",type=Path,default=Path(__file__).resolve().parents[1]/"build/Release/caustic_cli.exe")
    args = parser.parse_args()
    if bool(args.target)!=bool(args.out):
        parser.error("--target and --out must be supplied together")
    profile = Profile.read(args.design)
    stats,recovered = verify(profile,args.cad)
    if args.target:
        if not profile.linear_ends:
            parser.error("Projection replay needs the source and screen stored in a photographic design")
        source = args.design/"surface.txt" if args.design.is_dir() else args.design
        if source.suffix.lower()==".json":
            source=source.parent/"surface.txt"
        with tempfile.TemporaryDirectory() as tmp:
            params = Path(tmp)/"surface.txt"
            with source.open() as original, params.open("w",encoding="ascii") as f:
                f.write(original.readline())
                np.savetxt(f,recovered,fmt="%.17g")
            subprocess.run([str(args.cli),"trace-photo",str(params),str(args.target),str(args.out),
                            str(args.rays),"0"],check=True)
        (args.out/"cad_verification.json").write_text(json.dumps(stats,indent=2)+"\n")
    print(json.dumps(stats))
