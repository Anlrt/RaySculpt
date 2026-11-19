"""Integrate transmitted flux directly on the exported STL exit facets."""
import argparse
import json

import numpy as np
from PIL import Image
from pathlib import Path

from transport_mesh import FACET
from verify_photo_stl import transmission


def trace(folder, strata, emitter):
    info = json.loads((folder/"report.json").read_text())
    with (folder/"surface.txt").open() as stream:
        _,width,_,n,source,screen,pic_w,pic_h = map(float,stream.readline().split())
    path = folder/"02_plate.stl"
    cells = info["mesh_cells"]
    count = 2*cells*cells
    with path.open("rb") as stream:
        stream.seek(80)
        total = int.from_bytes(stream.read(4),"little")
    if total != count+12*cells or path.stat().st_size != 84+50*total:
        raise ValueError("STL size or topology mismatch")
    triangles = np.memmap(path,mode="r",dtype=FACET,offset=84,shape=(total,))["vertices"][:count]
    w,h = info["picture_px"]
    bx,by = info["guard_px"]
    sw,sh = w+2*bx,h+2*by
    image = np.zeros(sw*sh)
    offsets = (np.arange(strata*strata)%strata, np.arange(strata*strata)//strata)
    rng = np.random.default_rng(142693)
    area = outside = 0.
    missed = tir = 0
    # At a sampled exit point invert refraction at the flat entrance. The
    # Jacobian below changes exit-plane area to illuminated entrance area.
    for first in range(0,count,4096):
        v = triangles[first:first+4096].astype(float)
        p = v[:,0]
        e1,e2 = v[:,1]-p,v[:,2]-p
        normals = np.cross(e1,e2)
        areas = normals[:,2:3]/2
        if areas.min()<=0:
            raise ValueError("Inverted STL triangle")
        area += areas.sum()
        gx,gy = -normals[:,0:1]/(2*areas),-normals[:,1:2]/(2*areas)
        if ((abs(gx)+abs(gy))*(width+emitter)/(2*n*source)).max()>=.9:
            raise ValueError("Facet is outside the single-intersection verification bound")
        shape = (len(v),strata**2)
        a,b = [(offset+rng.random(shape))/strata for offset in offsets]
        flip = a+b>1
        a[flip], b[flip] = 1-a[flip],1-b[flip]
        x,y,z = [p[:,i:i+1]+a*e1[:,i:i+1]+b*e2[:,i:i+1] for i in range(3)]
        sx = emitter*(rng.random(shape)-.5) if emitter else 0.
        sy = emitter*(rng.random(shape)-.5) if emitter else 0.
        rx,ry = x-sx,y-sy
        factor = 1+z/(n*source)
        for _ in range(12):
            r2 = (rx**2+ry**2)/factor**2
            q = np.sqrt((n*source)**2+(n*n-1)*r2)
            new = 1+z/q
            if np.max(abs(factor-new))<2e-13:
                break
            factor = new
        else:
            raise ValueError("Entrance inversion did not converge")
        ex,ey = rx/factor,ry/factor
        illum = (abs(ex+sx)<=width/2)&(abs(ey+sy)<=width/2)
        distance = np.sqrt(source*source+ex*ex+ey*ey)
        ix,iy = ex/(n*distance),ey/(n*distance)
        iz = np.sqrt(1-ix*ix-iy*iy)
        normal_len = np.sqrt(1+gx*gx+gy*gy)
        ci = (iz-gx*ix-gy*iy)/normal_len
        radicand = 1-n*n*(1-ci*ci)
        tir += np.count_nonzero(illum & (radicand<0))
        cost = np.sqrt(np.maximum(0,radicand))
        bend = (n*ci-cost)/normal_len
        oz = n*iz-bend
        t = (screen-z)/np.where(oz>0,oz,1)
        col = np.floor((x+(n*ix+bend*gx)*t)*w/pic_w+sw/2).astype(int)
        row = np.floor(-(y+(n*iy+bend*gy)*t)*h/pic_h+sh/2).astype(int)
        reaches = (oz>0)&(col>=0)&(col<sw)&(row>=0)&(row<sh)
        missed += np.count_nonzero(illum & (radicand>=0) & ~reaches)
        jac = (1-(ex*gx+ey*gy)/q)/((1+z/q)*(1+z*(n*source)**2/q**3))
        cos0 = source/distance
        weights = (areas/strata**2)*jac*cos0/distance**2*transmission(cos0,iz,1,n)*transmission(ci,cost,n,1)
        if emitter:
            weights *= cos0
        valid = illum & reaches & (radicand>=0)
        index = (row*sw+col)[valid]
        outside += weights[~illum].sum()
        # Accumulate sparsely; each block generally hits only a few rows.
        if index.size:
            low = index.min()
            block = np.bincount(index-low,weights=weights[valid])
            image[low:low+len(block)] += block
    if abs(area-width**2)>1e-5*width**2:
        raise ValueError("Facets do not cover the specified aperture")
    image = image.reshape(sh,sw)
    target_gray = np.asarray(Image.open(folder/"01_monochrome.png"))/255
    target = np.where(target_gray<=.04045,target_gray/12.92,((target_gray+.055)/1.055)**2.4)
    captured = image.sum()
    image *= target.sum()/captured
    inside = np.s_[by:by+h,bx:bx+w]
    normalized_error = np.linalg.norm((image-target)[inside])/np.linalg.norm(target[inside])
    clipped = np.clip(image,0,1)
    gamma = np.where(clipped<=.0031308,12.92*clipped,1.055*clipped**(1/2.4)-.055)
    suffix = f"_source_{emitter:g}mm" if emitter else ""
    Image.fromarray(np.rint(gamma*255).astype("uint8")).save(folder/f"04_facet_projection{suffix}.png")
    result = dict(rays=count*strata**2,exit_area_mm2=area,source_size_mm=emitter,
                  captured_flux=float(captured),unilluminated_exit_weight=float(outside),
                  miss=int(missed),tir=int(tir),picture_relative_rmse=float(normalized_error),
                  captured_flux_guard_pct=float(100*(1-image[inside].sum()/image.sum())))
    (folder/f"facet_trace{suffix}.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result),flush=True)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder",type=Path)
    parser.add_argument("--strata",type=int,default=4)
    parser.add_argument("--source-size",type=float,default=0)
    args=parser.parse_args()
    if not 1<=args.strata<=16 or not 0<=args.source_size<=100:
        parser.error("Invalid sampling budget or emitter width")
    trace(args.folder,args.strata,args.source_size)
