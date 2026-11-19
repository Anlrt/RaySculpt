"""Retrace a design_photo export using the triangles in the binary STL."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def read_top(path, n):
    facet = np.dtype([("normal","<f4",(3,)),("vertices","<f4",(3,3)),("attribute","<u2")])
    with path.open("rb") as stream:
        stream.seek(80)
        count = int.from_bytes(stream.read(4),"little")
    if count != 2*n*n + 12*n or path.stat().st_size != 84+50*count:
        raise ValueError("Unexpected STL topology or file size")
    records = np.memmap(path,mode="r",dtype=facet,offset=84,shape=(count,))
    vertices = records["vertices"][:2*n*n].reshape(n,n,2,3,3)
    nodes = np.empty((n+1,n+1,3),dtype=np.float64)
    nodes[:-1,:-1] = vertices[:,:,0,0]
    nodes[-1,:-1] = vertices[-1,:,1,2]
    nodes[:-1,-1] = vertices[:,-1,0,1]
    nodes[-1,-1] = vertices[-1,-1,0,2]
    for face,corner,expected in [(0,0,nodes[:-1,:-1]),(0,1,nodes[:-1,1:]),(0,2,nodes[1:,1:]),
                                 (1,0,nodes[:-1,:-1]),(1,1,nodes[1:,1:]),(1,2,nodes[1:,:-1])]:
        if not np.array_equal(vertices[:,:,face,corner],expected):
            raise ValueError("Inconsistent shared STL vertices or facet orientation")
    xs, ys, z = nodes[0,:,0], nodes[:,0,1], nodes[:,:,2]
    if (not np.all(nodes[:,:,0] == xs) or not np.all(nodes[:,:,1] == ys[:,None])
            or np.any(np.diff(xs)<=0) or np.any(np.diff(ys)<=0) or not np.all(z>0)):
        raise ValueError("Unsupported exit mesh")
    gx = np.diff(z,axis=1)/np.diff(xs)
    gy = np.diff(z,axis=0)/np.diff(ys)[:,None]
    return xs,ys,z,gx,gy


def transmission(cosi,cost,n1,n2):
    rs = (n1*cosi-n2*cost)/(n1*cosi+n2*cost)
    rp = (n1*cost-n2*cosi)/(n1*cost+n2*cosi)
    return 1 - (rs**2+rp**2)/2


def check(folder, side, source_size):
    report = json.loads((folder/"report.json").read_text())
    with (folder/"surface.txt").open() as stream:
        _,plate,thickness,index,source,screen,width,height = map(float,stream.readline().split())
    xs,ys,z,gx,gy = read_top(folder/"02_plate.stl",report["mesh_cells"])
    n = len(xs)-1
    if max(abs(xs[0]+plate/2), abs(xs[-1]-plate/2), abs(ys[0]+plate/2),abs(ys[-1]-plate/2)) > 1e-5:
        raise ValueError("Profile and STL apertures differ")
    bound = (abs(gx).max()+abs(gy).max())*(plate+source_size)/(2*index*source)
    if bound >= .9:
        raise ValueError("Exit intersection is outside the verifier's contraction bound")
    w,h = report["picture_px"]
    bx,by = report["guard_px"]
    sw,sh = w+2*bx,h+2*by
    raster = np.zeros(sw*sh)
    rng = np.random.default_rng(975319)
    side_loss = tir = missed = 0
    for first in range(0,side,24):
        rows = np.arange(first,min(first+24,side))[:,None]
        shape = (len(rows),side)
        x = ((np.arange(side)+rng.random(shape))/side-.5)*plate
        y = ((rows+rng.random(shape))/side-.5)*plate
        dx0,dy0 = x,y
        if source_size:
            dx0 = x-source_size*(rng.random(shape)-.5)
            dy0 = y-source_size*(rng.random(shape)-.5)
        distance = np.sqrt(dx0*dx0+dy0*dy0+source**2)
        ix,iy = dx0/(distance*index), dy0/(distance*index)
        iz = np.sqrt(1-ix**2-iy**2)
        tx,ty = ix/iz,iy/iz
        level = thickness
        for _ in range(36):
            px,py = x+level*tx,y+level*ty
            # A side-escaping ray can pass the aperture before reaching the
            # trial height. Extend by its boundary value for the root solve,
            # then classify the actual (unclipped) hit against the aperture.
            ux,uy = np.clip(px,xs[0],xs[-1]),np.clip(py,ys[0],ys[-1])
            # Near a float32 knot use its actual stored coordinate. Rounding
            # of the exported mesh otherwise changes the chosen triangle.
            i = np.clip(((ux/plate+.5)*n).astype(int),0,n-1)
            j = np.clip(((uy/plate+.5)*n).astype(int),0,n-1)
            i = np.clip(i+(ux>xs[i+1]).astype(int)-(ux<xs[i]).astype(int),0,n-1)
            j = np.clip(j+(uy>ys[j+1]).astype(int)-(uy<ys[j]).astype(int),0,n-1)
            dx,dy = ux-xs[i], uy-ys[j]
            lower = dx*(ys[j+1]-ys[j]) >= dy*(xs[i+1]-xs[i])
            sx = np.where(lower,gx[j,i],gx[j+1,i])
            sy = np.where(lower,gy[j,i+1],gy[j,i])
            new = z[j,i]+sx*dx+sy*dy
            if np.max(abs(new-level)) < 1e-8:
                break
            level = new
        else:
            raise ValueError("STL intersection failed to converge")
        valid = (abs(px)<=plate/2)&(abs(py)<=plate/2)
        side_loss += np.count_nonzero(~valid)
        normal_length = np.sqrt(1+sx*sx+sy*sy)
        cosi = (iz-sx*ix-sy*iy)/normal_length
        k = 1-index**2*(1-cosi**2)
        tir += np.count_nonzero(valid & (k<0))
        valid &= k>=0
        cost = np.sqrt(np.maximum(k,0))
        factor = (index*cosi-cost)/normal_length
        oz = index*iz-factor
        distance_z = (screen-level)/np.where(oz>0,oz,1)
        hit_x = px+(index*ix+factor*sx)*distance_z
        hit_y = py+(index*iy+factor*sy)*distance_z
        col = np.floor(hit_x*w/width + sw/2).astype(int)
        row = np.floor(-hit_y*h/height + sh/2).astype(int)
        lands = (oz>0)&(col>=0)&(col<sw)&(row>=0)&(row<sh)
        missed += np.count_nonzero(valid & ~lands)
        valid &= lands
        cos_front = source/distance
        flux = (cos_front/distance**2 * transmission(cos_front,iz,1,index)
                * transmission(cosi,cost,index,1))
        if source_size:
            flux *= cos_front  # Lambertian emission from the square source.
        raster += np.bincount((row[valid]*sw+col[valid]),weights=flux[valid],minlength=sw*sh)
    raster = raster.reshape(sh,sw)
    # The stored target PNG is an 8-bit display image, so its quantization affects
    # this independent verifier's reported error very slightly.
    gray = np.asarray(Image.open(folder/"01_monochrome.png"))/255
    target = np.where(gray<=.04045, gray/12.92, ((gray+.055)/1.055)**2.4)
    raster *= target.sum()/raster.sum()
    inside = np.s_[by:by+h,bx:bx+w]
    error = np.linalg.norm((raster-target)[inside])/np.linalg.norm(target[inside])
    linear = np.clip(raster,0,1)
    srgb = np.where(linear<=.0031308,12.92*linear,1.055*linear**(1/2.4)-.055)
    suffix = f"_source_{source_size:g}mm" if source_size else ""
    Image.fromarray(np.rint(srgb*255).astype("uint8")).save(folder/f"04_stl_projection{suffix}.png")
    result = dict(rays=side**2,source_size_mm=source_size,side_escape=int(side_loss),tir=int(tir),miss=int(missed),
                  picture_relative_rmse=float(error),
                  captured_flux_guard_pct=float(100*(1-raster[inside].sum()/raster.sum())))
    (folder/f"stl_verification{suffix}.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(result),flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder",type=Path)
    parser.add_argument("--side",type=int,default=4000,help="strata per axis")
    parser.add_argument("--source-size",type=float,default=0,help="square source width in mm; zero is a point")
    args = parser.parse_args()
    if not (100 <= args.side <= 12000 and 0 <= args.source_size <= 100):
        parser.error("Invalid ray budget or source width.")
    check(args.folder,args.side,args.source_size)
