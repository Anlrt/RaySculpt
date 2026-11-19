"""End-to-end checks for the horizontal optical bench and source-geometry commands."""
from copy import deepcopy
from pathlib import Path
import math
import os
import sys
import tempfile
import unittest

import numpy as np

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cad_desktop as ui
import cad_scene as model
import cad_topology as topo
from cad_geometry import visual_geometry
from cad_inspect import inspect
from cad_placement import apply_layout, search


class ModelingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = ui.W.QApplication.instance() or ui.W.QApplication([])

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.folder = Path(tmp.name)
        self.scene = model.new_scene()
        self.plate, self.light, self.screen = self.scene["objects"]

    def test_horizontal_scene_and_legacy_coordinates(self):
        for obj in (self.light, self.screen):
            np.testing.assert_allclose(obj["pos"][1:], self.plate["pos"][1:])
        np.testing.assert_allclose(model.rotation(self.plate)[:, 2], [1, 0, 0], atol=1e-12)
        self.scene["version"] = 1
        for obj, z in zip(self.scene["objects"], [0, -600, 260]):
            obj.update(pos=[0., 0., z], rot=[0., 0., 0.])
        path = self.folder / "legacy.ccad"
        model.save_scene(self.scene, self.folder, path)
        self.assertEqual(self.scene, model.load_scene(path, self.folder))

    def test_readable_palette_and_drag_command(self):
        from PySide6.QtTest import QTest
        self.app.styleHints().setColorScheme(ui.Qt.ColorScheme.Dark)
        dark = ui.G.QPalette()
        dark.setColor(ui.G.QPalette.Text, ui.G.QColor("white"))
        dark.setColor(ui.G.QPalette.Window, ui.G.QColor("#151515"))
        self.app.setPalette(dark)
        ui.setup_style(self.app)
        win = ui.Window()
        try:
            win.show()
            win.add("box")
            self.app.processEvents()
            for widget in (win.tree, win.journal, win.props.findChild(ui.W.QLineEdit), win.menuBar()):
                palette = widget.palette()
                roles=ui.G.QPalette
                for group in (roles.Active, roles.Inactive, roles.Disabled):
                    self.assertGreater(palette.color(group, roles.Base).lightness() -
                                       palette.color(group, roles.Text).lightness(), 100)
            obj = win.obj()
            origin = np.array(obj["pos"])
            win.actions["move"].trigger()
            self.assertTrue(win.cam.move_mode)
            win.cam.repaint()
            index = win.scene["objects"].index(obj) + 1
            yy, xx = np.where(win.cam.hit == index)
            self.assertGreater(len(xx), 0)
            pt = ui.C.QPoint(int(np.median(xx)), int(np.median(yy)))
            self.assertEqual(win.cam.at(ui.C.QPointF(pt)), obj["id"])
            end = pt + ui.C.QPoint(22, -13)
            QTest.mousePress(win.cam, ui.Qt.LeftButton, pos=pt)
            QTest.mouseMove(win.cam, end)
            QTest.mouseRelease(win.cam, ui.Qt.LeftButton, pos=end)
            self.assertGreater(np.linalg.norm(np.array(obj["pos"]) - origin), 1)
            win.undo()
            np.testing.assert_allclose(win.obj()["pos"], origin)
        finally:
            win.saved = win.snapshot()
            win.close()
            win.temp.cleanup()

    def test_mate_face_and_right_angle_survive_rebuild(self):
        axis = np.array([0., 0., -1.])
        def face(o):
            return topo.ref_for(self.scene, o, self.folder, "face", np.array([0.,0.,50.]), axis, .01)
        base, own = face(self.plate), face(self.screen)
        self.screen.update(parent=self.plate["id"], constraint=dict(base=base, own=own, distance=48., angle=90.))
        topo.rebuild_mates(self.scene, self.folder)
        for height in (8., 13.):
            self.plate["dims"]["h"] = height
            topo.rebuild_mates(self.scene, self.folder)
            anchor, basis = topo.world_frame(self.scene, base, self.folder)
            child, _ = topo.world_frame(self.scene, own, self.folder)
            np.testing.assert_allclose(child, anchor + 48*basis[:,2], atol=1e-8)
            distance, angle = topo.measure(self.scene, base, own, self.folder)
            self.assertAlmostEqual(angle, 90.)
            self.assertAlmostEqual(distance, 48. - self.screen["dims"]["d"]/2)
        path = self.folder/"assembly.ccad"
        model.save_scene(self.scene, self.folder, path)
        restored = model.load_scene(path, self.folder)
        topo.rebuild_mates(restored, self.folder)
        self.assertEqual(restored, self.scene)

    def test_curve_boolean_and_feature_dependencies(self):
        part, tool = model.node("box"), model.node("prism")
        tool.update(circle=[0., 0., 5.], outline=[])
        self.scene["objects"].extend([part, tool])
        volume = lambda o: visual_geometry(topo.resolved(self.scene, o), self.folder)[2]
        base = volume(part)
        part["operations"] = [dict(id="hole", kind="subtract", tool=tool["id"])]
        self.assertAlmostEqual(volume(part), base - math.pi*25*15, places=6)
        tool["circle"][2] = 7.
        self.assertAlmostEqual(volume(part), base - math.pi*49*15, places=6)
        part["operations"][0]["kind"] = "intersect"
        self.assertAlmostEqual(volume(part), math.pi*49*15, places=6)
        part["operations"][0]["kind"] = "union"
        self.assertAlmostEqual(volume(part), base, places=6)
        part["operations"][0]["suppressed"] = True
        self.assertEqual(model.consumed_tools(self.scene), set())
        part["operations"].append(dict(id="cut",kind="section",origin=[0.,0.,12.],normal=[1.,0.,1.]))
        self.assertAlmostEqual(volume(part), base/2, places=6)
        arc = model.node("prism")
        raw = volume(arc)
        arc["arcs"] = {"0": -4.}
        self.assertGreater(volume(arc), raw)
        tool["operations"] = [dict(id="back", kind="union", tool=part["id"])]
        part["operations"][0]["suppressed"] = False
        with self.assertRaises(ValueError):
            topo.rebuild_mates(self.scene, self.folder)

    def test_inspection_actual_spline_and_open_mesh(self):
        path = self.folder / "profile.txt"
        with path.open("w") as f:
            f.write("7 40 8 1.49 600 260 50 50\n")
            y,x=np.mgrid[-1:1:7j,-1:1:7j]
            np.savetxt(f, np.exp(-6*(x*x+y*y)))
        self.plate["profile"] = model.store_asset(path,self.folder)
        self.screen.update(pos=(np.array(self.plate["pos"])+model.rotation(self.plate)[:,2]*8.2).tolist())
        self.assertFalse(any(bad for bad,_,_ in model.diagnostics(self.scene)))
        self.assertTrue(any(set(r["objects"])=={self.plate["id"],self.screen["id"]} for r in inspect(self.scene,self.folder)))
        mesh = model.node("mesh")
        source = self.folder/"open.obj"
        source.write_text("v 0 0 0\nv 3 0 0\nv 0 3 0\nf 1 2 3\n")
        mesh["asset"] = model.store_asset(source,self.folder)
        self.scene["objects"].append(mesh)
        reports = inspect(self.scene,self.folder)
        issue = next(r for r in reports if "открытые кромки" in r["message"])
        self.assertEqual(len(issue["lines"]),3)

    def test_tabletop_placement(self):
        base = deepcopy(self.scene)
        for mode in ("area", "sun"):
            request=dict(scene=self.scene,mode=mode,assets=str(self.folder),path=str(self.folder/(mode+".json")),
                         screen=self.screen["id"],min_gap=45.,max_gap=125.,max_lamp=300.,size=3.)
            choices=search(request)
            self.assertEqual(self.scene,base)
            first=choices[0]
            self.assertTrue(45.<=first["gap"]<=125.)
            self.assertGreater(first["blur"],0.)
            trial=deepcopy(self.scene)
            apply_layout(trial,self.screen["id"],mode,first["gap"],first["lamp"],first["size"])
            p,l,s=model.optical_setup(trial)
            self.assertEqual(l["kind"],mode)
            self.assertAlmostEqual(model.relative(p,s[0])[0][2]-p["dims"]["h"],first["gap"])
            self.assertFalse(any(bad for bad,_,_ in model.diagnostics(trial)))
        self.screen["locked"]=True
        with self.assertRaises(ValueError):
            apply_layout(self.scene,self.screen["id"],"area",50.,150.,3.)
