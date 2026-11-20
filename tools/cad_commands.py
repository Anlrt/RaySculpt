"""Modeling commands and feature history for the desktop editor."""
from copy import deepcopy
import json
from pathlib import Path
from uuid import uuid4
import numpy as np
from PySide6 import QtWidgets as W, QtCore as C, QtGui as G
from PySide6.QtCore import Qt

import cad_scene as model
import cad_topology as topo


def parameters(parent,title,fields):
    dlg=W.QDialog(parent)
    dlg.setWindowTitle(title)
    layout=W.QFormLayout(dlg)
    boxes=[]
    for name,value,lo,hi in fields:
        box=W.QDoubleSpinBox()
        box.setDecimals(4)
        box.setRange(lo,hi)
        box.setValue(float(value))
        layout.addRow(name,box)
        boxes.append(box)
    buttons=W.QDialogButtonBox(W.QDialogButtonBox.Ok|W.QDialogButtonBox.Cancel)
    buttons.accepted.connect(dlg.accept)
    buttons.rejected.connect(dlg.reject)
    layout.addRow(buttons)
    return [b.value() for b in boxes] if dlg.exec()==W.QDialog.Accepted else None


class ModelingCommands:
    def modeling_actions(self):
        self.picks=[]
        self.inspection=[]
        self.active_step=None
        bar=self.addToolBar("Геометрия")
        self.insertToolBarBreak(bar)
        bar.setMovable(False)
        bar.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        bar.addWidget(W.QLabel(" Выбор "))
        self.pick_selector=W.QComboBox()
        for key,title in (("object","Объект"),*topo.NAMES.items()):
            self.pick_selector.addItem(title,key)
        bar.addWidget(self.pick_selector)
        self.pick_selector.currentIndexChanged.connect(self.pick_mode)
        self.cam.subpicked.connect(lambda *a:self.guard(lambda:self.pick_part(*a)))
        ops=self.menuBar().addMenu("Построения")
        self.act(ops,"Измерить",self.measure_parts,key="measure",bar=bar)
        self.act(ops,"Сопряжение…",self.mate_parts,key="snap",bar=bar)
        self.act(ops,"Эскиз на грани…",self.supported_sketch)
        self.act(ops,"Эскиз на базовой плоскости…",self.plane_sketch)
        self.act(ops,"Срез плоскостью…",self.section)
        for k in ("union","subtract","intersect"):
            self.act(ops,topo.OPS[k]+"…",lambda k=k:self.boolean(k))
        self.act(ops,"Изменить операцию…",self.edit_step)
        self.act(ops,"Подавить / восстановить операцию",self.suppress_step)
        self.act(ops,"Удалить операцию",self.delete_step)
        self.act(ops,"Проверить геометрию",self.inspect_geometry,key="inspect",bar=bar)
        self.tree.itemDoubleClicked.connect(lambda row,col:self.guard(self.edit_step) if row.data(0,Qt.UserRole+1) else None)

    def pick_mode(self,*_):
        self.cam.pick_mode=self.pick_selector.currentData()
        if self.cam.pick_mode!="object":
            self.actions["move"].setChecked(False)
            self.cam.move_mode=False
            self.cam.setCursor(Qt.CrossCursor)
            self.statusBar().showMessage("Выберите опору, затем элемент второго объекта. Тип выбора можно менять.",8000)
        else:
            self.cam.setCursor(Qt.ArrowCursor)
            self.picks.clear()
            self.refresh_picks()

    def pick_part(self,key,origin,direction,tolerance):
        item=self.obj(key)
        rot=model.rotation(item)
        ref=topo.ref_for(self.scene,item,self.assets,self.cam.pick_mode,
                         rot.T@(origin-item["pos"]),rot.T@direction,tolerance)
        self.picks=(self.picks+[ref])[-2:]
        self.select(key)
        self.refresh_picks()
        names=[self.obj(r["object"])["name"]+" · "+topo.NAMES[r["kind"]]+" "+str(r["index"]+1) for r in self.picks]
        self.statusBar().showMessage(" → ".join(names),10000)

    def refresh_picks(self):
        lines=[]
        valid=[]
        for ref in self.picks:
            item=self.obj(ref["object"])
            if item:
                try:
                    world=[p@model.rotation(item).T+item["pos"] for p in topo.outlines(self.scene,ref,self.assets)]
                except (ValueError,IndexError):
                    continue
                lines.extend(world)
                valid.append(ref)
        self.picks=valid
        self.cam.highlights=lines+[np.asarray(p) for issue in self.inspection for p in issue["lines"]]
        self.cam.update()

    def pair(self):
        if len(self.picks)!=2:
            raise ValueError("На панели «Выбор» укажите тип элемента. Щёлкните сначала по опоре, затем по второму элементу.")
        return self.picks

    def measure_parts(self):
        a,b=self.pair()
        dist,angle=topo.measure(self.scene,a,b,self.assets)
        text=f"Минимальное расстояние {dist:.4f} мм"
        if angle is not None:
            text+=f" · меньший угол {angle:.3f}°"
        self.log(text)
        self.statusBar().showMessage(text,16000)

    def mate_parts(self):
        a,b=self.pair()
        item=self.obj(b["object"])
        if a["object"]==item["id"] or item["locked"]:
            raise ValueError("Второй элемент должен принадлежать другому подвижному объекту.")
        values=parameters(self,"Сопряжение выбранных элементов",[
            ("Смещение вдоль нормали опоры, мм",0,-1e6,1e6),("Поворот вокруг оси X опоры / ребра, °",0,-360,360)])
        if values:
            self.mutate(lambda:item.update(parent=a["object"],constraint=dict(base=deepcopy(a),own=deepcopy(b),
                                                      distance=values[0],angle=values[1])))

    def supported_sketch(self):
        if not self.picks or self.picks[-1]["kind"]!="face":
            raise ValueError("Выберите грань в рабочем виде. Для кривой поверхности используется касательная плоскость.")
        self.sketch(False,deepcopy(self.picks[-1]))

    def plane_sketch(self):
        plane,ok=W.QInputDialog.getItem(self,"Плоскость эскиза","Базовая плоскость",["XY","XZ","YZ"],0,False)
        if ok:
            self.sketch(False,plane=plane)

    def body_for_op(self):
        o=self.obj()
        if not o or o["kind"] not in ("box","sphere","cylinder","cad","prism"):
            raise ValueError("Выберите объёмное CAD-тело в дереве модели")
        return o

    def section(self):
        obj=self.body_for_op()
        op=dict(id=uuid4().hex,kind="section",origin=[0.,0.,obj["dims"].get("h",0)/2],normal=[0.,0.,1.])
        self.section_fields(obj,op,True)

    def section_fields(self,obj,op,add):
        normal=np.array(op["normal"])
        elev=np.degrees(np.arcsin(np.clip(normal[2],-1,1)))
        az=np.degrees(np.arctan2(normal[1],normal[0]))
        values=parameters(self,"Срез в локальных координатах тела",[
            *((name+" точки, мм",v,-1e6,1e6) for name,v in zip("XYZ",op["origin"])),
            ("Азимут нормали, °",az,-360,360),("Подъём нормали, °",elev,-90,90)])
        if values:
            az,el=np.radians(values[3:])
            def apply():
                op.update(origin=values[:3],normal=[np.cos(az)*np.cos(el),np.sin(az)*np.cos(el),np.sin(el)])
                if add:
                    obj.setdefault("operations",[]).append(op)
            self.mutate(apply)

    def boolean(self,kind,existing=None):
        obj=self.body_for_op()
        tools=[i for i in self.scene["objects"] if i!=obj and i["kind"] in ("box","sphere","cylinder","prism","cad")]
        if not tools:
            raise ValueError("Добавьте второе объёмное тело и задайте его положение")
        names=[f'{j+1}. {i["name"]}' for j,i in enumerate(tools)]
        name,ok=W.QInputDialog.getItem(self,topo.OPS[kind],f'Тело-инструмент для «{obj["name"]}»',names,0,False)
        if ok:
            tool=tools[names.index(name)]
            def apply():
                if existing is None:
                    obj.setdefault("operations",[]).append(dict(id=uuid4().hex,kind=kind,tool=tool["id"]))
                else:
                    existing["tool"]=tool["id"]
            self.mutate(apply)

    def feature_tree(self,row,obj):
        steps=[]
        if obj["kind"] in model.SOLIDS:
            steps.append(("base","Эскиз" if obj["kind"]=="prism" else model.LABELS[obj["kind"]]))
        if obj["kind"]=="prism":
            steps.append(("height","Выдавливание"))
        if "cut" in obj:
            steps.append(("cut","Срез XY"))
        steps.extend((op["id"],topo.OPS[op["kind"]]+(" · подавлена" if op.get("suppressed") else ""))
                     for op in obj.get("operations",[]))
        if "constraint" in obj:
            steps.append(("constraint","Сопряжение"))
        for key,name in steps:
            child=W.QTreeWidgetItem(row,[name])
            child.setData(0,Qt.UserRole,obj["id"])
            child.setData(0,Qt.UserRole+1,key)
            if self.active_step==(obj["id"],key):
                self.tree.setCurrentItem(child)
                row.setExpanded(True)

    def step(self):
        if not self.active_step:
            raise ValueError("Раскройте тело в дереве и выберите операцию")
        key,step=self.active_step
        obj=self.obj(key)
        return obj,step,next((op for op in obj.get("operations",[]) if op["id"]==step),None)

    def edit_step(self):
        obj,step,op=self.step()
        if op:
            if op["kind"]=="section":
                self.section_fields(obj,op,False)
            else:
                self.boolean(op["kind"],op)
        elif step=="constraint":
            mate=obj["constraint"]
            values=parameters(self,"Параметры сопряжения",[("Смещение, мм",mate["distance"],-1e6,1e6),
                                                         ("Угол, °",mate["angle"],-360,360)])
            if values:
                self.mutate(lambda:mate.update(distance=values[0],angle=values[1]))
        elif step=="base" and obj["kind"]=="prism":
            self.sketch(True)
        elif step=="cut":
            self.cut()
        elif obj["dims"] and not obj.get("profile"):
            dims=list(obj["dims"])
            labels={"h":"Высота","w":"Ширина","r":"Радиус","d":"Глубина"}
            values=parameters(self,"Размеры построения",[(labels[k]+", мм",obj["dims"][k],.001,1e6) for k in dims])
            if values:
                self.mutate(lambda:obj["dims"].update(zip(dims,values)))

    def suppress_step(self):
        obj,step,op=self.step()
        if op is None:
            raise ValueError("Выберите срез плоскостью или булеву операцию")
        self.mutate(lambda:op.update(suppressed=not op.get("suppressed")))

    def delete_step(self):
        obj,step,op=self.step()
        if op:
            self.mutate(lambda:obj["operations"].remove(op))
        elif step=="constraint":
            self.unmate()
        elif step=="cut":
            self.uncut()
        else:
            raise ValueError("Базовое построение удаляется вместе с телом")

    def inspect_geometry(self):
        out=self.work/(uuid4().hex+".json")
        def finished():
            self.show_inspection(json.loads(out.read_text(encoding="utf-8")))
        self.worker("inspect","Проверка исходной геометрии…",dict(scene=self.scene,assets=str(self.assets),path=str(out)),finished)

    def show_inspection(self,reports):
        self.inspection=reports
        if not reports:
            self.log("Проверка завершена. CAD-тела, границы сеток и пересечения с экранами прошли проверку.")
        for report in reports:
            self.log(report["message"],report["severity"],report["objects"])
        self.refresh()

    def placement(self):
        from cad_placement import apply_layout
        plate,lamp,screens=model.optical_setup(self.scene)
        screen=self.obj() if self.obj() in screens else screens[0]
        dlg=W.QDialog(self)
        dlg.setWindowTitle("Расстановка источника и экрана")
        layout=W.QFormLayout(dlg)
        choice=W.QComboBox()
        choice.addItems([s["name"] for s in screens])
        choice.setCurrentIndex(screens.index(screen))
        layout.addRow("Экран",choice)
        mode=W.QComboBox()
        mode.addItems(["Лампа с протяжённым излучателем","Прямой солнечный свет"])
        mode.setCurrentIndex(int(lamp["kind"]=="sun"))
        layout.addRow("Освещение",mode)
        boxes=[]
        for title,start in (("От выходной грани, минимум",30),("От выходной грани, максимум",180),
                            ("От лампы до пластины, максимум",350),("Ширина излучающей области лампы",lamp["dims"].get("w",3))):
            box=W.QDoubleSpinBox()
            box.setRange(.1 if len(boxes)==3 else 10,10000)
            box.setValue(start)
            box.setSuffix(" мм")
            layout.addRow(title,box)
            boxes.append(box)
        def lamp_fields():
            for field in boxes[2:]:
                field.setEnabled(mode.currentIndex()==0)
        mode.currentIndexChanged.connect(lamp_fields)
        lamp_fields()
        buttons=W.QDialogButtonBox(W.QDialogButtonBox.Ok|W.QDialogButtonBox.Cancel)
        buttons.accepted.connect(dlg.accept)
        buttons.rejected.connect(dlg.reject)
        layout.addRow(buttons)
        if dlg.exec()!=W.QDialog.Accepted:
            return
        lo,hi,far,size=[b.value() for b in boxes]
        selected=screens[choice.currentIndex()]
        out=self.work/(uuid4().hex+".json")
        def finished():
            proposals=json.loads(out.read_text(encoding="utf-8"))
            labels=[]
            for r in proposals:
                title=f'Экран {r["gap"]:.1f} мм'
                if r["mode"]=="area":
                    title+=f' · лампа {r["lamp"]:.1f} мм'
                title+=f' · оценка размытия {r["blur"]:.2f} мм'
                if "error" in r:
                    title+=f' · ошибка яркости {r["error"]:.3f}'
                labels.append(title)
            prompt="Положения по результатам трассировки" if plate.get("profile") else "Оценка для будущего рельефа"
            if np.isclose(proposals[0]["gap"],lo) or np.isclose(proposals[0]["gap"],hi):
                prompt+=". Лучший вариант на границе заданного диапазона"
            answer,ok=W.QInputDialog.getItem(self,"Применить расстановку",prompt,labels,0,False)
            if ok:
                r=proposals[labels.index(answer)]
                self.mutate(lambda:apply_layout(self.scene,r["screen"],r["mode"],r["gap"],r["lamp"],r["size"]))
                self.cam.fit(True)
                self.log(prompt+" · "+answer)
        self.worker("placement","Поиск компактной расстановки…",dict(scene=self.scene,assets=str(self.assets),
                    path=str(out),mode="sun" if mode.currentIndex() else "area",screen=selected["id"],
                    min_gap=lo,max_gap=hi,max_lamp=far,size=size),finished)
