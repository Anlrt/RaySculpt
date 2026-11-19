"""Indexed plate meshes and interchange writers. Coordinates are millimetres."""
from dataclasses import dataclass
import math
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

import numpy as np

from transport_mesh import FACET, emit, surface


@dataclass
class PlateMesh:
    cells: int
    vertices: np.ndarray
    diagonal_ac: np.ndarray

    @classmethod
    def from_grid(cls, top, diagonal):
        n = len(top)-1
        rim = np.concatenate((top[0,:-1], top[:-1,-1], top[-1,:0:-1], top[:0:-1,0]))
        rim[:,2] = 0
        vertices = np.concatenate((top.reshape(-1,3),rim,np.zeros((1,3),dtype=top.dtype)))
        return cls(n,vertices,diagonal)

    @classmethod
    def sample(cls, p, n):
        axis = np.linspace(-p.width/2,p.width/2,n+1)
        top = np.empty((n+1,n+1,3))
        top[:,:,0],top[:,:,1] = axis,axis[:,None]
        for j in range(0,n+1,32):
            rows = top[j:j+32]
            rows[:,:,2] = p.thickness+surface(p.relief,p.width,rows[:,:,0],rows[:,:,1],p.linear_ends)
        return cls.from_grid(top,np.ones((n,n),dtype=bool))

    @classmethod
    def from_saved_stl(cls,path):
        # Both plate mesh layouts store each pair of top facets together.
        # Recover shared vertices without welding away sharp optical features.
        with path.open("rb") as f:
            f.seek(80)
            count = int.from_bytes(f.read(4),"little")
        n = (math.isqrt(36+2*count)-6)//2
        if not 1<=n<=2048 or count!=2*n*n+12*n or path.stat().st_size!=84+count*50:
            raise ValueError("Unsupported plate STL topology. Use --cells to remesh the profile.")
        raw = np.memmap(path,dtype=FACET,mode="r",offset=84,shape=(count,))
        faces = raw["vertices"][:2*n*n].reshape(n,n,2,3,3)
        top = np.empty((n+1,n+1,3),dtype=np.float32)
        top[:-1,:-1] = faces[:,:,0,0]
        top[:-1,-1] = faces[:,-1,0,1]
        top[-1,:-1] = faces[-1,:,1,2]
        top[-1,-1] = faces[-1,-1,1,1]
        if not np.isfinite(top).all() or np.min(top[:,:,2])<=0:
            raise ValueError("Invalid STL exit coordinates")
        diagonal = np.all(faces[:,:,0,0]==faces[:,:,1,0],axis=-1)
        mesh = cls.from_grid(top,diagonal)
        offset = 0
        for indices in mesh.triangles():
            length = len(indices)
            if not np.array_equal(mesh.vertices[indices],raw["vertices"][offset:offset+length]):
                raise ValueError("STL does not match the plate topology. Use --cells to remesh.")
            offset += length
        return mesh

    def triangles(self):
        n = self.cells
        top = np.arange((n+1)**2,dtype=np.int32).reshape(n+1,n+1)
        for j in range(0,n,24):
            stop = min(n,j+24)
            a,b = top[j:stop,:-1],top[j:stop,1:]
            c,d = top[j+1:stop+1,1:],top[j+1:stop+1,:-1]
            split = self.diagonal_ac[j:stop]
            yield np.stack((a,b,np.where(split,c,d),np.where(split,a,b),c,d),axis=-1).reshape(-1,3)
        rim = np.concatenate((top[0,:-1],top[:-1,-1],top[-1,:0:-1],top[:0:-1,0]))
        bottom = len(top.ravel())+np.arange(len(rim))
        b,b0 = np.roll(rim,-1),np.roll(bottom,-1)
        center = np.full_like(bottom,len(self.vertices)-1)
        yield np.stack((bottom,b0,b,bottom,b,rim,center,b0,bottom),axis=-1).reshape(-1,3)

    def write(self,path):
        if path.suffix==".stl":
            with path.open("xb") as f:
                count = 2*self.cells**2+12*self.cells
                f.write(b"Caustic CAD; mm".ljust(80,b"\0")+count.to_bytes(4,"little"))
                for indices in self.triangles():
                    emit(f,self.vertices[indices])
        elif path.suffix==".obj":
            with path.open("xb") as f:
                f.write(b"# Caustic CAD; coordinates in mm\no Plate\n")
                np.savetxt(f,self.vertices,fmt="v %.16g %.16g %.16g")
                for indices in self.triangles():
                    np.savetxt(f,indices+1,fmt="f %d %d %d")
        else:
            self.write_3mf(path)

    def write_3mf(self,path):
        with ZipFile(path,"x",compression=ZIP_DEFLATED,compresslevel=2) as package:
            package.writestr("[Content_Types].xml",'''<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>''')
            package.writestr("_rels/.rels",'''<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="model" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel" Target="/3D/3dmodel.model"/></Relationships>''')
            with package.open("3D/3dmodel.model","w") as f:
                f.write(b'<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources><object id="1" type="model"><mesh><vertices>\n')
                np.savetxt(f,self.vertices,fmt='<vertex x="%.16g" y="%.16g" z="%.16g"/>')
                f.write(b'</vertices><triangles>\n')
                for indices in self.triangles():
                    np.savetxt(f,indices,fmt='<triangle v1="%d" v2="%d" v3="%d"/>')
                f.write(b'</triangles></mesh></object></resources><build><item objectid="1"/></build></model>')
