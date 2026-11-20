"""Regression tests for the material-aware CAD workflow on Windows."""
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from zipfile import ZipFile

import numpy as np
from PIL import Image
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
os.environ.setdefault("QT_QPA_PLATFORM","offscreen")
import cad_desktop as ui
import cad_scene as m
import cad_topology as topo
from cad_geometry import visual_geometry,export_body


class DesktopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app=ui.W.QApplication.instance() or ui.W.QApplication([])

    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)
        self.win=ui.Window()
        self.win.show()
        self.app.processEvents()

    def tearDown(self):
        self.win.saved=self.win.snapshot()
        self.win.close()
        self.app.processEvents()
        self.win.temp.cleanup()

    def finished(self):
        deadline=time.monotonic()+70
        while self.win.proc and time.monotonic()<deadline:
            self.app.processEvents()
            time.sleep(.01)
        self.assertIsNone(self.win.proc,self.win.journal.toPlainText())

    def test_assign_trace_switch_undo_and_portable_file(self):
        win=self.win
        plate,src,s=win.scene["objects"]
        profile=self.root/"surface.txt"
        y,x=np.mgrid[-1:1:13j,-1:1:13j]
        with profile.open("w") as f:
            f.write("13 40 8 1.491 600 260 50 50\n")
            np.savetxt(f,-.5*(x*x+y*y))
        win.mutate(lambda:win.attach_profile(plate,profile))
        win.ray_count,win.resolution=80000,64
        win.trace()
        self.finished()
        image1=np.asarray(Image.open(win.result[0]/"1.png")).copy()
        report1=win.result[1]
        geom=win.cam.geometry[plate["id"]]
        combo=next(c for c in win.props.findChildren(ui.W.QComboBox) if c.findData("sf11")>=0)
        combo.setCurrentIndex(combo.findData("sf11"))
        self.assertTrue(win.result_stale())
        self.assertIs(report1,win.result[1])
        self.assertIs(geom,win.cam.geometry[plate["id"]])
        win.trace()
        self.finished()
        image2=np.asarray(Image.open(win.result[0]/"1.png"))
        self.assertGreater(np.mean(abs(image1.astype(float)-image2)),2)
        self.assertEqual(report1["n"],1.491)
        self.assertEqual(win.result[1]["n"],1.78472)
        self.assertNotEqual(report1["screens"][0]["flux"],win.result[1]["screens"][0]["flux"])
        win.undo()
        self.assertEqual(m.index(win.scene,win.obj()),1.491)
        win.redo()
        self.assertEqual(m.index(win.scene,win.obj()),1.78472)
        # Include both the profile and a target, then reopen after removing the originals.
        image=self.root/"target.png"
        Image.fromarray(image1).save(image)
        win.scene["objects"][2]["image"]=m.store_asset(image,win.assets)
        path=self.root/"Проверка.ccad"
        m.save_scene(win.scene,win.assets,path)
        profile.unlink()
        image.unlink()
        other=self.root/"restored"
        other.mkdir()
        scene=m.load_scene(path,other)
        self.assertEqual(win.scene,scene)
        self.assertEqual(set(k.name for k in other.iterdir()),m.keys(scene))
        self.assertEqual(win.obj()["design_n"],1.491)

    def test_cut_export_import_and_lock(self):
        win=self.win
        win.add("sphere")
        sphere=win.obj()
        win.mutate(lambda:sphere.update(cut=0))
        volume=win.cam.geometry[sphere["id"]][2]
        self.assertAlmostEqual(volume,2*math.pi*20**3/3,places=6)
        for ext,kind in (("step","cad"),("brep","cad"),("stl","mesh")):
            target=self.root/("тело."+ext)
            sphere["pos"]=[100,0,5]
            export_body(sphere,win.assets,target)
            body=m.node(kind)
            body["asset"]=m.store_asset(target,win.assets)
            xyz,_,restored=visual_geometry(body,win.assets)
            self.assertAlmostEqual(restored/volume,1,delta=.006)
            self.assertAlmostEqual(float(xyz[:,0].mean()),100,delta=3.)
        win.mutate(lambda:sphere.update(locked=True))
        pos=sphere["pos"].copy()
        win.drag(sphere["id"],[30,30,30],True)
        self.assertEqual(sphere["pos"],pos)
        self.assertTrue(win.actions["trace"].isEnabled())
        win.mutate(lambda:sphere.update(reference=False))
        self.assertFalse(win.actions["trace"].isEnabled())
        self.assertIn(sphere["name"],win.status.text())
        win.mutate(lambda:sphere.update(reference=True))
        self.assertTrue(win.actions["trace"].isEnabled())

    def test_receivers_and_area_source(self):
        win=self.win
        p,light,s=win.scene["objects"]
        light.update(kind="area",dims={"w":8.,"d":7.})
        side=m.node("screen",260.)
        side["rot"]=topo.euler(m.rotation(s)@m.rotation({"rot":[0,90,0]}))
        side["pos"]=(np.array(s["pos"])+m.rotation(s)@np.array([25.,0.,25.])).tolist()
        win.mutate(lambda:win.scene["objects"].append(side))
        self.assertTrue(win.actions["trace"].isEnabled())
        win.ray_count,win.resolution=16000,48
        win.trace()
        self.finished()
        self.assertEqual(len(win.result[1]["screens"]),2)
        side["pos"]=s["pos"][:]
        win.refresh()
        self.assertFalse(win.actions["trace"].isEnabled())

    def test_design_uses_selected_material(self):
        win=self.win
        plate,_,screen=win.scene["objects"]
        path=self.root/"тест.png"
        gray=np.tile(np.linspace(85,215,80,dtype=np.uint8),(64,1))
        Image.fromarray(gray).save(path)
        screen["image"]=m.store_asset(path,win.assets)
        screen["dims"]["d"]=52.35
        win.mutate(lambda:plate.update(material="bk7",line="546.1"))
        with patch.object(ui.W.QInputDialog,"getItem",return_value=("Эскиз · 96 пикселей",True)):
            win.design()
        self.finished()
        self.assertIn("profile",plate,win.journal.toPlainText())
        self.assertEqual(plate["design_n"],1.51872)
        self.assertTrue((win.result[0]/"03_projection.png").is_file())
        self.assertEqual((win.assets/plate["profile"]).read_text().splitlines()[0].split()[-1],"52.35")
        self.assertEqual(win.picture.images[0].size(),win.picture.images[1].size())

    def test_custom_library(self):
        material=dict(name="Test",density=1.3,default="530",indices={"530":1.52},source="test")
        with patch.object(ui,"LIBRARY",self.root/"catalog"/"materials.json"):
            ui.save_material("local-test",material)
            self.assertEqual(ui.catalog()["local-test"],material)
            scene=m.new_scene(ui.catalog())
            file=self.root/"library.ccad"
            m.save_scene(scene,self.root,file)
            self.assertEqual(m.load_scene(file,self.root)["materials"]["local-test"],material)

    def test_place_target_preserves_screen_and_compares_snapshot(self):
        from cad_targets import screen_target
        win=self.win
        screen=win.scene["objects"][2]
        image=self.root/"portrait.png"
        photo=np.full((100,80,3),255,dtype=np.uint8)
        photo[:5]=0
        Image.fromarray(photo).save(image)
        self.assertFalse(win.actions["design"].isEnabled())
        self.assertTrue(win.actions["trace"].isEnabled())
        self.assertIn(screen["name"],win.status.text())
        with patch.object(ui.W.QFileDialog,"getOpenFileName",return_value=(str(image),"")), \
             patch.object(ui.TargetDialog,"exec",return_value=ui.W.QDialog.Accepted):
            win.target()
        self.assertEqual(win.selected,screen["id"])
        self.assertEqual(screen["dims"],{"w":50.,"d":50.})
        self.assertTrue(win.actions["design"].isEnabled())
        for mode,edge in (("fit",0.),("crop",1.)):
            goal=screen_target(image,(50,50),mode)
            self.assertEqual(goal[25,0],edge)
            self.assertEqual(goal[25,25],1.)
        win.ray_count,win.resolution=32000,50
        win.trace()
        self.finished()
        self.assertEqual(len(win.picture.images),3)
        self.assertEqual(win.picture.images[0].toImage().pixelColor(0,25).red(),0)
        win.mutate(lambda:screen.update(name="Портрет"))
        self.assertFalse(win.result_stale())
        self.assertEqual(win.screen_choice.currentText(),"Портрет")
        win.mutate(lambda:screen.update(image_fit="crop"))
        self.assertTrue(win.result_stale())
        self.assertEqual(win.picture.images[0].toImage().pixelColor(0,25).red(),0)
        win.undo()
        self.assertFalse(win.result_stale())
        win.replace(m.new_scene())
        self.assertIsNone(win.result)
        self.assertFalse(win.picture.images)

    def test_explicit_resize_and_cancel(self):
        win=self.win
        screen=win.scene["objects"][2]
        image=self.root/"portrait.png"
        Image.new("RGB",(80,100),"white").save(image)
        with patch.object(ui.TargetDialog,"exec",return_value=ui.W.QDialog.Rejected):
            win.place_target(screen,image)
        self.assertNotIn("image",screen)
        def resize(dialog):
            dialog.mode.setCurrentIndex(dialog.mode.findData("resize"))
            return ui.W.QDialog.Accepted
        with patch.object(ui.TargetDialog,"exec",resize):
            win.place_target(screen,image)
        self.assertEqual(screen["dims"],{"w":50.,"d":62.5})
        win.undo()
        self.assertNotIn("image",win.obj(screen["id"]))

    def test_rigid_relation(self):
        win=self.win
        plate,source,screen=win.scene["objects"]
        screen["parent"]=plate["id"]
        source["parent"]=screen["id"]
        old=m.relative(plate,screen)
        original_source=m.relative(plate,source)[0]
        m.move(win.scene,plate,[25.,7.,3.],[20.,35.,12.])
        new=m.relative(plate,screen)
        np.testing.assert_allclose(new[0],old[0],atol=1e-8)
        np.testing.assert_allclose(new[1],old[1],atol=1e-8)
        np.testing.assert_allclose(m.relative(plate,source)[0],original_source,atol=1e-8)
        plate["parent"]=source["id"]
        with self.assertRaises(ValueError):
            m.validate(win.scene)
        plate.pop("parent")

    def test_editable_extrusion(self):
        from cad_sketch import Sketch
        item=m.node("prism")
        dlg=Sketch(self.win,item["outline"])
        dlg.table.item(0,0).setText("-25")
        self.assertEqual(dlg.canvas.points[0][0],-25)
        item["outline"]=[[0,0],[20,0],[15,10],[0,10]]
        self.assertAlmostEqual(visual_geometry(item,self.root)[2],175*15,places=6)
        item["outline"]=[[0,0],[20,10],[0,10],[20,0]]
        with self.assertRaises(ValueError):
            visual_geometry(item,self.root)

    def test_shared_surface_two_angles(self):
        win=self.win
        plate,_,screen=win.scene["objects"]
        shape=m.ROOT/"examples/sample_asymmetric.obj"
        key=m.store_asset(shape,win.assets)
        win.mutate(lambda:win.scene.update(turntable=dict(asset=key,name=shape.name,
                   screen=screen["id"],angles=[-8.,8.])))
        win.design()
        self.finished()
        self.assertIn("profile",plate,win.journal.toPlainText())
        entries=win.result[1]["screens"]
        self.assertEqual([r["angle"] for r in entries],[-8,0,8])
        self.assertTrue(entries[1]["intermediate"])
        for i,row in enumerate(entries):
            win.screen_choice.setCurrentIndex(i)
            self.assertTrue((win.result[0]/row["target"]).is_file())
            self.assertTrue(all(win.picture.images))
        self.assertTrue(all(r["flux"]>0 and math.isfinite(r["error"]) for r in entries))
        win.ray_count,win.resolution=32000,48
        win.trace()
        self.finished()
        self.assertEqual(win.result[1]["screens"][0]["angle"],0.)
        self.assertTrue(all(win.picture.images))

    def test_fit_two_screens(self):
        win=self.win
        plate,_,screen=win.scene["objects"]
        other=m.node("screen",260.)
        other["rot"]=screen["rot"][:]
        center=np.array(screen["pos"])
        for j,s in enumerate([screen,other]):
            s["pos"]=(center+m.rotation(screen)[:,0]*(-12.5 if j==0 else 12.5)).tolist()
            s["dims"]={"w":25.,"d":40.}
            img=self.root/f"{j}.png"
            x,y=np.meshgrid(np.linspace(-1,1,40),np.linspace(-1,1,64))
            Image.fromarray(np.uint8(60+140*np.exp(-4*(x*x+(y-.45*(-1)**j)**2)))).save(img)
            s["image"]=m.store_asset(img,win.assets)
        win.mutate(lambda:win.scene["objects"].append(other))
        self.assertTrue(win.actions["design"].isEnabled())
        win.design()
        self.finished()
        self.assertIn("profile",plate,win.journal.toPlainText())
        self.assertEqual(len(win.result[1]["screens"]),2)
        self.assertTrue(all(s["flux"]>0 for s in win.result[1]["screens"]))

    def test_cancel_keeps_geometry(self):
        win=self.win
        win.ray_count=200000000
        state=win.snapshot()
        win.trace()
        win.cancel()
        self.finished()
        self.assertEqual(state,win.snapshot())
        self.assertIsNone(win.result)


if __name__=="__main__":
    unittest.main()
