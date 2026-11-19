"""Design one continuous freeform plate for a complete on-axis photograph."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile

import cv2
import numpy as np
from PIL import Image, ImageOps
from export_plate import CAD,finalize_design,formats_list


def poisson_inverse(shape, dx, dy):
    """Inverse cell-centred Neumann Laplacian; the constant mode is free."""
    h, w = shape
    x = -4 * (np.sin(np.pi * np.arange(w) / (2*w)) / dx)**2
    y = -4 * (np.sin(np.pi * np.arange(h) / (2*h)) / dy)**2
    symbol = x[None, :] + y[:, None]
    symbol[0, 0] = 1
    inverse = 1 / symbol
    inverse[0, 0] = 0
    return lambda rhs: cv2.idct(cv2.dct(np.ascontiguousarray(rhs)) * inverse)


def inverse_transport(target, width, height, aperture, steps):
    """Map screen to exit aperture, with a convex potential and fixed side normals."""
    n = target.shape[0]
    dx, dy = width/n, height/n
    ax, ay = aperture/width, aperture/height
    density = target / target.mean() + 0.025
    density *= ax*ay / density.mean()
    solve = poisson_inverse(target.shape, dx, dy)
    potential = np.zeros_like(target)
    for iteration in range(steps):
        p = np.pad(potential, 1, mode="edge")
        xx = ax + (p[1:-1, 2:] + p[1:-1, :-2] - 2*potential) / dx**2
        yy = ay + (p[2:, 1:-1] + p[:-2, 1:-1] - 2*potential) / dy**2
        xy = (p[2:, 2:] + p[:-2, :-2] - p[2:, :-2] - p[:-2, 2:]) / (4*dx*dy)
        residual = np.sqrt((xx-yy)**2 + 4*(density+xy**2)) - xx - yy
        potential += .82 * solve(residual)
        if np.sqrt(np.mean(residual**2)) < 2e-5 * (ax+ay):
            break
    p = np.pad(potential, 1, mode="edge")
    # Derivatives on vertices, including the prescribed boundary normals.
    vx = ((p[1:, 1:] - p[1:, :-1]) + (p[:-1, 1:] - p[:-1, :-1])) / (2*dx)
    vy = ((p[1:, 1:] - p[:-1, 1:]) + (p[1:, :-1] - p[:-1, :-1])) / (2*dy)
    axis = np.linspace(-aperture/2, aperture/2, n+1)
    mx, my = vx + axis, vy + axis[:, None]
    # The bilinear map has an affine Jacobian determinant in each cell.
    # Its minimum is at a corner. Repair only neighbourhoods that fold.
    for _ in range(16):
        hx, hy = np.diff(mx,axis=1), np.diff(my,axis=1)
        kx, ky = np.diff(mx,axis=0), np.diff(my,axis=0)
        bad = np.zeros((n,n), dtype=bool)
        for horizontal in (slice(None,-1),slice(1,None)):
            for vertical in (slice(None,-1),slice(1,None)):
                bad |= hx[horizontal]*ky[:,vertical] - hy[horizontal]*kx[:,vertical] < 1e-6*(aperture/n)**2
        if not bad.any():
            return mx, my
        neighbours = np.pad(bad.astype(np.uint8),((0,1),(0,1)))
        repair = cv2.dilate(neighbours, np.ones((5,5),np.uint8)).astype(bool)
        mx[repair] = cv2.GaussianBlur(mx,(0,0),1.1)[repair]
        my[repair] = cv2.GaussianBlur(my,(0,0),1.1)[repair]
        mx[:,0], mx[:,-1] = axis[0], axis[-1]
        my[0,:], my[-1,:] = axis[0], axis[-1]
    raise ValueError("Transport orientation could not be restored within the iteration budget.")


def sample_cell(a, ix, iy, u, v):
    a00, a10 = a[iy, ix], a[iy, ix+1]
    a01, a11 = a[iy+1, ix], a[iy+1, ix+1]
    lower, upper = a10-a00, a11-a01
    return (a00 + u*lower + v*(a01-a00 + u*(upper-lower)),
            lower + v*(upper-lower), a01-a00 + u*(upper-lower))


def forward_transport(mx, my, grid, aperture, width, height):
    """Invert bilinear cells analytically in each damped Newton step."""
    n = mx.shape[0]-1
    axis = np.linspace(-aperture/2, aperture/2, grid)
    x, y = np.meshgrid(axis, axis)
    u, v = (x/aperture+.5)*n, (y/aperture+.5)*n
    for _ in range(80):
        ix, iy = np.minimum(u.astype(int), n-1), np.minimum(v.astype(int), n-1)
        local = (ix, iy, u-ix, v-iy)
        px, xu, xv = sample_cell(mx, *local)
        py, yu, yv = sample_cell(my, *local)
        ex, ey = px-x, py-y
        if np.max(np.maximum(abs(ex), abs(ey))) < 1e-7*aperture:
            break
        det = xu*yv - xv*yu
        if np.any(det <= 0):
            raise ValueError("Transport folded. Reduce contrast or increase the transport resolution.")
        du, dv = (yv*ex-xv*ey)/det, (xu*ey-yu*ex)/det
        # A full Newton step can jump across thin, dark regions into a
        # different cell. Backtrack each point against its true residual.
        scale = 1 / np.maximum(1., np.maximum(abs(du), abs(dv))/(n/8))
        for _ in range(14):
            a, b = np.clip(u-du*scale, 0, n), np.clip(v-dv*scale, 0, n)
            i, j = np.minimum(a.astype(int), n-1), np.minimum(b.astype(int), n-1)
            trial = (i,j,a-i,b-j)
            ex1 = sample_cell(mx,*trial)[0] - x
            ey1 = sample_cell(my,*trial)[0] - y
            reject = ex1**2+ey1**2 > ex**2+ey**2 + 1e-24
            if not reject.any():
                break
            scale[reject] *= .5
        u, v = a, b
    else:
        raise ValueError("Transport inversion did not converge within its iteration budget.")
    return x, y, (u/n-.5)*width, (v/n-.5)*height


def optical_surface(x, y, tx, ty, aperture, thickness, index, source, screen):
    """Fit one integrable height to the Snell normals at the actual exit positions."""
    spacing = aperture / (x.shape[0]-1)
    solve = poisson_inverse(x.shape, spacing, spacing)
    relief = np.zeros_like(x)
    for _ in range(7):
        z = thickness + relief
        # Invert refraction through the plane entrance for a point source.
        factor = 1 + z/(index*source)
        for _ in range(3):
            r2 = (x*x + y*y) / factor**2
            factor = 1 + z / np.sqrt((index*source)**2 + (index**2-1)*r2)
        entry_x, entry_y = x/factor, y/factor
        distance = np.sqrt(source**2 + entry_x**2 + entry_y**2)
        nx, ny = entry_x/distance, entry_y/distance
        nz = np.sqrt(index**2 - nx**2 - ny**2)
        ox, oy, oz = tx-x, ty-y, screen-z
        norm = np.sqrt(ox*ox + oy*oy + oz*oz)
        denominator = nz-oz/norm
        if np.min(denominator) <= 0:
            raise ValueError("Exit surface cannot support the requested directions.")
        gx, gy = (ox/norm-nx)/denominator, (oy/norm-ny)/denominator
        # The normal equations of the edge-gradient least-squares problem.
        fx = (gx[:, 1:] + gx[:, :-1])/(2*spacing)
        fy = (gy[1:, :] + gy[:-1, :])/(2*spacing)
        rhs = np.zeros_like(x)
        rhs[:, :-1] += fx
        rhs[:, 1:] -= fx
        rhs[:-1, :] += fy
        rhs[1:, :] -= fy
        relief = solve(rhs)
        relief -= .5*(relief.min()+relief.max())
    return relief


def spline_coefficients(height):
    """Interpolate nodal heights with cubic splines and linear end continuation."""
    def filter_axis(values):
        out = values.copy()
        upper = np.zeros(len(out))
        for j in range(1, len(out)-1):
            pivot = 4 - upper[j-1]
            upper[j] = 1/pivot
            out[j] = (6*values[j]-out[j-1])/pivot
        for j in range(len(out)-2, 0, -1):
            out[j] -= upper[j]*out[j+1]
        return out
    return filter_axis(filter_axis(height).T).T


def monochrome(path, resolution):
    with Image.open(path) as original:
        photo = ImageOps.exif_transpose(original).convert("RGBA")
        w, h = photo.size
        width, height = max(1, round(resolution*w/max(w,h))), max(1, round(resolution*h/max(w,h)))
        # Decode before averaging: an alternating dark/light pattern conserves flux.
        rgb = np.asarray(photo)
        table = np.arange(256)/255
        table = np.where(table <= .04045, table/12.92, ((table+.055)/1.055)**2.4)
        gray = np.zeros((h,w), dtype=np.float32)
        for channel, weight in enumerate((.2126,.7152,.0722)):
            gray += (weight*table[rgb[...,channel]]).astype(np.float32)
        gray *= rgb[...,3]/255
    return cv2.resize(gray, (width,height), interpolation=cv2.INTER_AREA).astype(float)


def write_profile(path, args, target):
    h, w = target.shape
    height = args.width*h/w
    # World Y increases upwards. Only the computational lattice is square.
    field = cv2.resize(target[::-1], (args.map_size,args.map_size), interpolation=cv2.INTER_AREA)
    mapping = inverse_transport(field, args.width, height, args.plate, args.steps)
    positions = forward_transport(*mapping, args.grid, args.plate, args.width, height)
    surface = optical_surface(*positions, args.plate, args.thickness, args.index, args.source, args.screen)
    coefficients = spline_coefficients(surface)
    if not np.isfinite(coefficients).all() or abs(coefficients).max() > args.max_relief:
        raise ValueError("Required relief exceeds the specified plate budget.")
    with path.open("w",encoding="ascii") as f:
        print(args.grid,args.plate,args.thickness,args.index,args.source,args.screen,args.width,height,file=f)
        np.savetxt(f,coefficients,fmt="%.15g")


def photo_design(args):
    if CAD.intersection(args.formats):
        import plate_cad  # Detect missing CAD support before optimizing.
    target = monochrome(args.image, args.resolution)
    if target.max() <= 0:
        raise ValueError("The target has no light.")
    h,w = target.shape
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.out.parent,prefix=".photo-design-") as folder:
        temp = Path(folder)
        with (temp/"target.pgm").open("wb") as f:
            f.write(f"P5\n{w} {h}\n65535\n".encode("ascii"))
            f.write(np.rint(target*65535).astype(">u2").tobytes())
        # Extreme dark-to-light transitions can require almost singular ray
        # maps. Roll off only the deepest shadows in the optical drive.
        # The original target remains the reference for all reported errors.
        goal = np.hypot(target,args.shadow_floor*target.mean())
        goal *= target.mean()/goal.mean()
        drive = goal.copy()
        history = []
        scores = []
        best_score = float("inf")
        stop = "iteration budget"
        def display(linear):
            linear = np.maximum(0,linear)
            return np.where(linear<=.0031308,12.92*linear,1.055*linear**(1/2.4)-.055)
        display_target = display(target)
        # Fit the incident-energy distribution again after measuring the real
        # refractive surface. All candidates see the same calibration rays.
        for iteration in range(args.corrections+1):
            profile = temp/f"profile_{iteration}.txt"
            try:
                write_profile(profile,args,drive)
            except ValueError as exc:
                if history:
                    stop = str(exc)
                    break
                raise
            calibration = temp/f"pass_{iteration}"
            subprocess.run([str(args.cli),"trace-photo",str(profile),str(temp/"target.pgm"),
                            str(calibration),str(min(args.rays,24_000_000)),"0","103433"],check=True)
            report = json.loads((calibration/"report.json").read_text())
            gx, gy = report["guard_px"]
            measured = np.fromfile(calibration/"irradiance.bin",dtype="=f4").reshape(h+2*gy,w+2*gx)
            actual = measured[gy:gy+h,gx:gx+w]
            # Squared linear irradiance gives very little weight to bright
            # streaks in shadows. Assess perceived tone and small-scale error.
            delta = display(actual) - display_target
            texture = delta - cv2.GaussianBlur(delta,(0,0),1.5)
            error = report["picture_relative_rmse"]
            score = np.sqrt(np.mean(delta**2)) + np.sqrt(np.mean(texture**2)) + .15*error
            history.append(error)
            scores.append(float(score))
            if score >= best_score:
                stop = "calibration score stopped improving"
                break
            best_score, best = score, profile
            # Low-pass just the correction to avoid imprinting sampling noise.
            # Do not soften the target or the reported projection.
            drive = np.maximum(.5*args.shadow_floor*target.mean(),
                               drive + .80*cv2.GaussianBlur(goal-actual,(0,0),.65))
        result = temp/"result"
        needs_mesh = bool(set(args.formats)-CAD)
        optical_mesh = needs_mesh and args.mesh_layout=="transport"
        subprocess.run([str(args.cli),"trace-photo",str(best),str(temp/"target.pgm"),
                        str(result),str(args.rays),str(args.cells if needs_mesh and not optical_mesh else 0)],check=True)
        (result/"irradiance.bin").unlink(missing_ok=True)
        report_path = result/"report.json"
        report = json.loads(report_path.read_text())
        if optical_mesh:
            from transport_mesh import export as export_transport_mesh
            field = cv2.resize(goal[::-1],(args.map_size,args.map_size),interpolation=cv2.INTER_AREA)
            mapping = inverse_transport(field,args.width,args.width*h/w,args.plate,args.steps)
            report["triangles"] = export_transport_mesh(result/"02_plate.stl",best,mapping,args.cells)
            report["mesh_cells"] = args.cells
            report["mesh_layout"] = "screen_transport"
        report.update(calibration_relative_rmse=history, calibration_scores=scores,
                      selected_pass=scores.index(best_score)+1, calibration_stop=stop,
                      map_size=args.map_size, transport_max_steps=args.steps, shadow_floor=args.shadow_floor)
        report_path.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
        finalize_design(result,args.formats,"02_plate")
        result.rename(args.out)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("image",type=Path)
    p.add_argument("--out",required=True,type=Path)
    p.add_argument("--formats",type=formats_list,default=("stl",),help="comma-separated: step,brep,stl,3mf,obj")
    p.add_argument("--cli",type=Path,default=Path(__file__).resolve().parents[1]/"build/Release/caustic_cli.exe")
    p.add_argument("--grid",type=int,default=512)
    p.add_argument("--map-size",type=int,default=640)
    p.add_argument("--resolution",type=int,default=448)
    p.add_argument("--steps",type=int,default=100)
    p.add_argument("--corrections",type=int,default=2)
    p.add_argument("--shadow-floor",type=float,default=.08,help="shadow toe in units of mean target irradiance")
    p.add_argument("--rays",type=int,default=36_000_000)
    p.add_argument("--cells",type=int,default=1024)
    p.add_argument("--mesh-layout",choices=("uniform","transport"),default="uniform")
    p.add_argument("--plate",type=float,default=40)
    p.add_argument("--width",type=float,default=50)
    p.add_argument("--thickness",type=float,default=8)
    p.add_argument("--max-relief",type=float,default=5)
    p.add_argument("--index",type=float,default=1.49)
    p.add_argument("--source",type=float,default=600)
    p.add_argument("--screen",type=float,default=260)
    args = p.parse_args()
    if args.out.exists() or not args.cli.is_file():
        p.error("Use a new output directory and a built caustic_cli.")
    if not (32 <= args.grid <= 1024 and 32 <= args.map_size <= 1024 and not args.grid%2
            and not args.map_size%2 and 16 <= args.resolution <= 1024 and 1 <= args.steps <= 500
            and 0 <= args.shadow_floor <= .5 and 0 <= args.corrections <= 5
            and 1000 <= args.rays <= 200_000_000
            and (not(set(args.formats)-CAD) or args.grid-1 <= args.cells <= (2048 if args.mesh_layout=="transport" else 2047))):
        p.error("Invalid grid, map size, output resolution, iteration, mesh or ray budget.")
    if not all(math.isfinite(v) and v>0 for v in (args.width,args.plate,args.thickness,args.index,
            args.source,args.screen,args.max_relief)) or not (args.index>1 and args.thickness>args.max_relief
            and args.screen>args.thickness+args.max_relief):
        p.error("Invalid optical geometry.")
    photo_design(args)
