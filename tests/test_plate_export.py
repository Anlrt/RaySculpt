"""Regression checks for smooth CAD and shared mesh export. Run with Python."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from zipfile import ZipFile

import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from export_plate import Profile,export
from plate_files import PlateMesh
from verify_solid import verify


class PlateExportTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.profile = self.root/"surface.txt"
        coeff = np.sin(np.arange(49).reshape(7,7))*.31
        with self.profile.open("w") as f:
            f.write("7 40 8 1.49 600 260 50 50\n")
            np.savetxt(f,coeff)
        self.report = self.root/"report.json"
        self.report.write_text(json.dumps(dict(surface="uniform_cubic_b_spline",grid=7,
            plate_mm=40,thickness_mm=8,spline_coefficients_mm=coeff.ravel().tolist())))

    def test_exact_edge_conventions(self):
        for source in (self.profile,self.report):
            out = self.root/source.suffix[1:]
            export(source,out,("step","brep"))
            profile = Profile.read(source)
            for file in out.iterdir():
                stats,restored = verify(profile,file)
                self.assertLess(stats["max_normal_error_deg"],1e-6)
                np.testing.assert_allclose(restored,profile.relief,atol=1e-8)

    def test_shared_mesh_connectivity(self):
        out = self.root/"mesh"
        export(self.profile,out,("stl","obj","3mf"),cells=13)
        mesh = PlateMesh.from_saved_stl(out/"plate.stl")
        ids = np.concatenate(list(mesh.triangles()))
        with ZipFile(out/"plate.3mf") as z:
            model = ET.fromstring(z.read("3D/3dmodel.model"))
            self.assertEqual(model.attrib["unit"],"millimeter")
            vertices = [[float(e.get(k)) for k in "xyz"] for e in model.findall(".//{*}vertex")]
            faces = [[int(e.get(k)) for k in ("v1","v2","v3")] for e in model.findall(".//{*}triangle")]
        np.testing.assert_allclose(vertices,mesh.vertices,atol=1e-6)
        np.testing.assert_array_equal(faces,ids)
        obj = (out/"plate.obj").read_text().splitlines()
        np.testing.assert_array_equal([[int(k)-1 for k in line.split()[1:]]
                                      for line in obj if line.startswith("f ")],ids)
        directed = np.concatenate((ids[:,[0,1]],ids[:,[1,2]],ids[:,[2,0]]))
        self.assertEqual(set(map(tuple,directed)),set(map(tuple,directed[:,::-1])))
        self.assertEqual(len(set(map(tuple,directed))),len(directed))

    def test_file_selection_and_existing_output(self):
        file = self.root/"new.stp"
        export(self.profile,file)
        self.assertTrue(file.read_bytes().startswith(b"ISO-10303-21"))
        with self.assertRaises(FileExistsError):
            export(self.profile,file)
        with self.assertRaises(ValueError):
            export(self.profile,self.root/"test.dxf")


if __name__=="__main__":
    unittest.main()
