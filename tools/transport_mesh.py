"""Triangulate a photographic plate with vertices distributed in screen space."""
import numpy as np


FACET = np.dtype([("normal","<f4",(3,)),("vertices","<f4",(3,3)),("attribute","<u2")])


def basis(axis, n, width, linear_ends=True):
    coord = np.clip((axis/width+.5)*(n-1),0,n-1)
    i = np.minimum(coord.astype(int),n-2)
    t = coord-i
    s = 1-t
    w = [s**3/6,(4+3*t*t*(t-2))/6,(4+3*s*s*(s-2))/6,t**3/6]
    if linear_ends:
        w[1] += np.where(i==0,w[0],0)
        w[2] -= np.where(i==0,w[0],0)
        w[2] += np.where(i==n-2,w[3],0)
        w[1] -= np.where(i==n-2,w[3],0)
    return [np.clip(i+k,0,n-1) for k in (-1,0,1,2)], w


def surface(z, width, x, y, linear_ends=True):
    ix,wx = basis(x,len(z),width,linear_ends)
    iy,wy = basis(y,len(z),width,linear_ends)
    return sum(wy[j]*sum(wx[i]*z[iy[j],ix[i]] for i in range(4)) for j in range(4))


def interpolate_map(field, u, v):
    old = len(field)-1
    x,y = u*old,v*old
    i,j = np.minimum(x.astype(int),old-1),np.minimum(y.astype(int),old-1)
    dx,dy = x-i,y-j
    return ((1-dy)*((1-dx)*field[j,i]+dx*field[j,i+1])
            + dy*((1-dx)*field[j+1,i]+dx*field[j+1,i+1]))


def emit(stream, triangles):
    data = np.zeros(triangles.shape[:-2],dtype=FACET)
    data["vertices"] = triangles
    # Derive the normals from the actual serialized float32 vertices.
    v = data["vertices"].astype(np.float64)
    normal = np.cross(v[...,1,:]-v[...,0,:],v[...,2,:]-v[...,0,:])
    length = np.linalg.norm(normal,axis=-1)
    if np.any(length<=1e-13):
        raise ValueError("Degenerate export triangle")
    data["normal"] = normal/length[...,None]
    data.tofile(stream)


def export(path, profile, mapping, n):
    with profile.open() as stream:
        _,width,thickness,*_ = map(float,stream.readline().split())
        coefficients = np.loadtxt(stream)
    # Mix in a little uniform aperture spacing to bound concentration in the
    # darkest parts, where the optical transport can approach singularity.
    nodes = np.empty((n+1,n+1,3),dtype=np.float32)
    rng = np.random.default_rng(381901)
    cols = np.arange(n+1)
    for j in range(0,n+1,32):
        rows = slice(j,j+32)
        row = np.arange(j,min(j+32,n+1))[:,None]
        shape = (len(row),len(cols))
        # Bounded jitter decorrelates facet streaks. Fix the four boundary
        # coordinates normal to the rim so the aperture remains rectangular.
        u = (cols + rng.uniform(-.23,.23,shape)*((cols!=0)&(cols!=n)))/n
        v = (row + rng.uniform(-.23,.23,shape)*((row!=0)&(row!=n)))/n
        x = .97*interpolate_map(mapping[0],u,v)+.03*(u-.5)*width
        y = .97*interpolate_map(mapping[1],u,v)+.03*(v-.5)*width
        nodes[rows,:,0],nodes[rows,:,1] = x,y
        nodes[rows,:,2] = thickness+surface(coefficients,width,x,y)
    count = 2*n*n+12*n
    with path.open("xb") as stream:
        stream.write(b"Caustic CAD; transport mesh; mm".ljust(80,b"\0"))
        stream.write(count.to_bytes(4,"little"))
        for j in range(0,n,24):
            end = min(n,j+24)
            a,b = nodes[j:end,:-1],nodes[j:end,1:]
            c,d = nodes[j+1:end+1,1:],nodes[j+1:end+1,:-1]
            # Choose the shorter aperture diagonal for less elongated faces.
            split = np.sum((a[...,:2]-c[...,:2])**2,axis=-1) <= np.sum((b[...,:2]-d[...,:2])**2,axis=-1)
            ac = np.stack((np.stack((a,b,c),axis=2),np.stack((a,c,d),axis=2)),axis=2)
            bd = np.stack((np.stack((a,b,d),axis=2),np.stack((b,c,d),axis=2)),axis=2)
            triangles = np.where(split[...,None,None,None],ac,bd)
            v = triangles.astype(np.float64)
            signed = ((v[...,1,0]-v[...,0,0])*(v[...,2,1]-v[...,0,1])
                      -(v[...,1,1]-v[...,0,1])*(v[...,2,0]-v[...,0,0]))
            if np.any(signed<=0):
                raise ValueError("Folded transport mesh")
            emit(stream,triangles)
        rim = np.concatenate((nodes[0,:-1], nodes[:-1,-1], nodes[-1,:0:-1], nodes[:0:-1,0]))
        next_rim = np.roll(rim,-1,axis=0)
        bottom = rim.copy()
        bottom[:,2] = 0
        next_bottom = np.roll(bottom,-1,axis=0)
        center = np.zeros_like(bottom)
        emit(stream,np.stack((np.stack((bottom,next_bottom,next_rim),axis=1),
                              np.stack((bottom,next_rim,rim),axis=1),
                              np.stack((center,next_bottom,bottom),axis=1)),axis=1))
    return count
