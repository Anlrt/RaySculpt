"""Caustic CAD desktop application."""
import sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
if (ROOT/"build/ui-libs").is_dir():
    sys.path.insert(0,str(ROOT/"build/ui-libs"))

from copy import deepcopy
from functools import partial
import json
import os
import tempfile
import shutil
from uuid import uuid4

import numpy as np
from PIL import Image, ImageOps
from PySide6 import QtCore as C, QtGui as G, QtWidgets as W
from PySide6.QtCore import Qt

import cad_scene as model
from cad_geometry import visual_geometry
from cad_view import View
from export_plate import Profile

DIMS={"w":"Ширина","d":"Глубина / высота","h":"Высота по Z","r":"Радиус"}
CLI=ROOT/"build/Release/caustic_cli.exe"
LIBRARY=Path(C.QStandardPaths.writableLocation(C.QStandardPaths.AppDataLocation))/"CausticCAD"/"materials.json"
STYLES="""
QMainWindow, QDialog {background:#eff3f5;color:#233844}
QMenuBar,QMenu,QToolBar {background:#fff;color:#233844}
QToolBar {border:0;border-bottom:1px solid #c9d4da;spacing:5px;padding:4px}
QToolButton {padding:5px 8px;border:1px solid transparent;border-radius:3px}
QToolButton:hover,QToolButton:pressed {background:#e4f0f6;border-color:#bfd8e5}
QTreeWidget,QTextEdit,QListWidget,QScrollArea {background:#fff;border:0}
QTreeWidget::item {padding:5px}
QTreeWidget::item:selected {background:#d9eaf4;color:#173a52}
QDockWidget::title {background:#e6edf1;color:#304d60;padding:7px;font-weight:600}
QLineEdit,QDoubleSpinBox,QComboBox,QSpinBox {background:#fff;border:1px solid #bdcbd3;border-radius:3px;padding:3px;min-height:19px}
QPushButton {background:#f3f7fa;border:1px solid #b7c8d2;border-radius:3px;padding:5px 9px}
QPushButton:hover {background:#e0eef7}
QTabBar::tab {background:#e5edf2;color:#426070;padding:6px 16px}
QTabBar::tab:selected {background:#fff;color:#135a7e}
QStatusBar {background:#e4ecf1;color:#2f5266}
QSplitter::handle {background:#d1dce2}
"""
ICONS={"new":W.QStyle.SP_FileIcon,"open":W.QStyle.SP_DialogOpenButton,"save":W.QStyle.SP_DialogSaveButton,
       "import":W.QStyle.SP_ArrowDown,"export":W.QStyle.SP_ArrowUp,"trace":W.QStyle.SP_MediaPlay,
       "cancel":W.QStyle.SP_MediaStop,"design":W.QStyle.SP_BrowserReload,"library":W.QStyle.SP_FileDialogDetailedView}


def setup_style(app):
    if os.name=="nt" and "Segoe UI" not in G.QFontDatabase.families():
        fonts=Path(os.environ.get("WINDIR","C:/Windows"))/"Fonts"
        for name in ("segoeui.ttf","seguisb.ttf","segoeuib.ttf"):
            G.QFontDatabase.addApplicationFont(str(fonts/name))
    app.setStyle("Fusion")
    app.setStyleSheet(STYLES)
    app.setFont(G.QFont("Segoe UI",9))


def number(value,lo,hi,digits=3,suffix=""):
    w=W.QDoubleSpinBox()
    w.setDecimals(digits)
    w.setRange(lo,hi)
    w.setValue(value)
    w.setSuffix(suffix)
    w.setKeyboardTracking(False)
    return w


def catalog():
    if not LIBRARY.is_file():
        return {}
    data=json.loads(LIBRARY.read_text(encoding="utf-8"))
    for m in data.values():
        model.validate_material(m)
    return {k:v for k,v in data.items() if k not in model.BUILTINS}


def save_material(key,material):
    entries=catalog()
    entries[key]=material
    LIBRARY.parent.mkdir(exist_ok=True,parents=True)
    file=C.QSaveFile(str(LIBRARY))
    if not file.open(C.QIODevice.WriteOnly):
        raise OSError(file.errorString())
    payload=json.dumps(entries,ensure_ascii=False).encode("utf-8")
    if file.write(payload)!=len(payload) or not file.commit():
        raise OSError(file.errorString())


class Window(W.QMainWindow):
    def __init__(self):
        super().__init__()
        self.resize(1320,840)
        self.temp=tempfile.TemporaryDirectory(prefix="caustic-cad-")
        self.work=Path(self.temp.name)
        self.assets=self.work/"assets"
        self.assets.mkdir()
        self.scene=model.new_scene(catalog())
        self.selected=self.scene["objects"][0]["id"]
        self.path=None
        self.history=[]
        self.future=[]
        self.saved=self.snapshot()
        self.cache={}
        self.revision=0
        self.proc=None
        self.result=None
        self.optical_issues=[]
        self.events=[]
        self.drag_start=None
        self.cam=View()
        self.cam.picked.connect(self.select)
        self.cam.dragged.connect(self.drag)
        self.tabs=W.QTabWidget()
        self.tabs.addTab(self.cam,"Модель")
        self.result_page=W.QWidget()
        result_layout=W.QVBoxLayout(self.result_page)
        self.screen_choice=W.QComboBox()
        self.screen_choice.currentIndexChanged.connect(self.show_image)
        self.picture=W.QLabel("Выполните проверку проекции")
        self.picture.setAlignment(Qt.AlignCenter)
        self.picture.setMinimumSize(128,128)
        select_result=W.QHBoxLayout()
        select_result.addWidget(self.screen_choice,1)
        save_result=W.QPushButton("Сохранить проекцию…")
        save_result.clicked.connect(lambda:self.guard(self.save_projection))
        select_result.addWidget(save_result)
        result_layout.addLayout(select_result)
        result_layout.addWidget(self.picture,1)
        self.tabs.addTab(self.result_page,"Проекция")
        self.setCentralWidget(self.tabs)
        self.tree=W.QTreeWidget()
        self.tree.setHeaderHidden(True)
        self.tree.currentItemChanged.connect(self.tree_selected)
        self.dock("Дерево модели",self.tree,Qt.LeftDockWidgetArea,210)
        self.props=W.QScrollArea()
        self.props.setWidgetResizable(True)
        self.props.setMinimumWidth(235)
        self.dock("Параметры объекта",self.props,Qt.RightDockWidgetArea,275)
        self.journal=W.QTextEdit()
        self.journal.setReadOnly(True)
        self.journal.document().setMaximumBlockCount(500)
        self.journal.setMaximumHeight(120)
        self.dock("Сообщения",self.journal,Qt.BottomDockWidgetArea,0)
        self.status=W.QLabel()
        self.statusBar().addWidget(self.status,1)
        self.statusBar().addPermanentWidget(W.QLabel("мм    °   "))
        self.prev_issues=set()
        self.actions={}
        self.make_actions()
        self.refresh()
        C.QTimer.singleShot(0,self.cam.fit)

    def dock(self,title,widget,area,width):
        d=W.QDockWidget(title,self)
        d.setObjectName(title)
        d.setFeatures(W.QDockWidget.DockWidgetMovable|W.QDockWidget.DockWidgetFloatable)
        d.setWidget(widget)
        self.addDockWidget(area,d)
        if width:
            self.resizeDocks([d],[width],Qt.Horizontal)

    def act(self,menu,name,fn,shortcut=None,key=None,bar=None):
        a=G.QAction(name,self)
        if key in ICONS:
            a.setIcon(self.style().standardIcon(ICONS[key]))
        if shortcut:
            a.setShortcut(shortcut)
        a.triggered.connect(lambda checked=False:self.guard(fn))
        menu.addAction(a)
        if bar:
            bar.addAction(a)
        if key:
            self.actions[key]=a
        return a

    def make_actions(self):
        file,edit,solid,optics,view=[self.menuBar().addMenu(s) for s in
                                   ("Файл","Правка","Модель","Оптика","Вид")]
        bar=self.addToolBar("Команды")
        bar.setMovable(False)
        bar.setIconSize(C.QSize(17,17))
        bar.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        self.act(file,"Создать",self.new,"Ctrl+N","new",bar)
        self.act(file,"Открыть…",self.open,"Ctrl+O","open",bar)
        self.act(file,"Сохранить",self.save,"Ctrl+S","save",bar)
        self.act(file,"Сохранить как…",lambda:self.save(True),"Ctrl+Shift+S")
        file.addSeparator()
        self.act(file,"Открыть расчёт по профилю…",self.open_profile)
        self.act(file,"Импорт модели…",self.import_shape,key="import")
        self.act(file,"Экспорт тела…",self.export,key="export")
        self.act(file,"Сохранить журнал…",self.save_log)
        self.act(edit,"Отменить",self.undo,"Ctrl+Z","undo")
        self.act(edit,"Вернуть",self.redo,"Ctrl+Y","redo")
        self.act(edit,"Удалить объект",self.remove,"Delete","delete")
        bar.addSeparator()
        insert=W.QToolButton()
        insert.setText("Добавить тело")
        insert.setPopupMode(W.QToolButton.InstantPopup)
        primitives=W.QMenu(insert)
        for kind in ("plate","box","sphere","cylinder"):
            a=self.act(solid,model.LABELS[kind],partial(self.add,kind))
            primitives.addAction(a)
        insert.setMenu(primitives)
        bar.addWidget(insert)
        primitives.addAction(self.act(solid,"Эскиз → выдавливание…",lambda:self.sketch(False)))
        self.act(solid,"Редактировать эскиз…",lambda:self.sketch(True),key="sketch")
        self.act(solid,"Срез плоскостью XY…",self.cut,key="cut")
        self.act(solid,"Убрать срез",self.uncut,key="uncut")
        self.act(solid,"Зафиксировать относительно…",self.mate,key="mate")
        self.act(solid,"Снять связь",self.unmate,key="unmate")
        self.act(solid,"Материалы…",self.materials,key="library",bar=bar)
        for kind in ("point","area","screen"):
            self.act(optics,"Добавить · "+model.LABELS[kind],partial(self.add,kind))
        self.act(optics,"Изображение экрана…",self.target)
        self.act(optics,"3D-цель и углы поворота…",self.turntable)
        self.act(optics,"Снять 3D-цель",self.clear_turntable,key="clear3d")
        self.act(optics,"Параметры проверки…",self.settings)
        self.act(optics,"Выровнять по оси пластины",self.align,key="align")
        self.act(optics,"Построить рельеф…",self.design,key="design",bar=bar)
        self.act(optics,"Проверить проекцию",self.trace,"F5","trace",bar)
        self.act(optics,"Остановить",self.cancel,key="cancel",bar=bar)
        self.act(view,"Вписать сцену",lambda:self.cam.fit(True),"F")
        self.act(view,"Приблизить выбранное",self.cam.fit,"Shift+F")
        for name,a,b in (("Изометрия",36,26),("Сверху",0,90),("Спереди",0,0),("Справа",90,0)):
            self.act(view,name,partial(self.orientation,a,b))
        grid=self.act(view,"Сетка",self.grid)
        grid.setCheckable(True)
        grid.setChecked(True)
        relief=self.act(view,"Карта высот пластины",self.color_relief)
        relief.setCheckable(True)

    def color_relief(self):
        self.cam.color_relief=not self.cam.color_relief
        self.cam.update()

    def grid(self):
        self.cam.show_grid=not self.cam.show_grid
        self.cam.update()

    def orientation(self,yaw,pitch):
        self.cam.yaw,self.cam.pitch=yaw,pitch
        self.cam.update()

    def guard(self,fn):
        try:
            return fn()
        except Exception as exc:
            self.log(str(exc))
            W.QMessageBox.warning(self,"Caustic CAD",str(exc))
            return False

    def log(self,message,level="info",objects=()):
        self.events.append(dict(time=C.QDateTime.currentDateTime().toString(Qt.ISODate),
                                revision=self.revision,level=level,objects=list(objects),message=message))
        self.journal.append(G.QTextDocumentFragment.fromPlainText(
            C.QTime.currentTime().toString("HH:mm:ss")+"   "+message).toHtml())

    def save_log(self):
        name,_=W.QFileDialog.getSaveFileName(self,"Сохранить журнал","Журнал.json","JSON (*.json)")
        if name:
            Path(name).write_text(json.dumps(self.events,ensure_ascii=False,indent=2),encoding="utf-8")

    def snapshot(self):
        return json.dumps(self.scene,sort_keys=True,ensure_ascii=False,allow_nan=False)

    def remember(self,old):
        if self.snapshot()==old:
            return
        self.history.append(old)
        self.history=self.history[-80:]
        self.future.clear()
        self.invalidate()

    def invalidate(self):
        self.revision+=1
        self.optical_issues=[]
        if self.result:
            self.result=None
            self.screen_choice.clear()
            self.picture.setText("Сцена изменена. Выполните проверку проекции.")

    def mutate(self,fn):
        old=self.snapshot()
        try:
            fn()
            model.validate(self.scene)
            self.rebuild_geometry()
        except Exception:
            self.scene=json.loads(old)
            self.refresh()
            raise
        self.remember(old)
        self.refresh()

    def undo(self):
        if self.history:
            self.future.append(self.snapshot())
            self.scene=json.loads(self.history.pop())
            self.invalidate()
            self.refresh()

    def redo(self):
        if self.future:
            self.history.append(self.snapshot())
            self.scene=json.loads(self.future.pop())
            self.invalidate()
            self.refresh()

    def obj(self,key=None):
        return next((i for i in self.scene["objects"] if i["id"]==(key or self.selected)),None)

    def select(self,key):
        if self.obj(key) is not None and self.selected!=key:
            self.selected=key
            self.refresh()

    def tree_selected(self,current,old):
        if current is not None:
            key=current.data(0,Qt.UserRole)
            if key:
                self.select(key)

    def rebuild_geometry(self):
        geometry={}
        used=set()
        for obj in self.scene["objects"]:
            if obj["kind"] not in model.SOLIDS:
                continue
            signature=self.geometry_key(obj)
            used.add(signature)
            if signature not in self.cache:
                self.cache[signature]=visual_geometry(obj,self.assets)
            geometry[obj["id"]]=self.cache[signature]
        self.cache={k:v for k,v in self.cache.items() if k in used}
        self.cam.geometry=geometry

    @staticmethod
    def geometry_key(obj):
        return json.dumps([obj.get(k) for k in ("kind","dims","profile","asset","cut","outline")],sort_keys=True)

    def refresh(self):
        self.rebuild_geometry()
        items=self.scene["objects"]
        if not self.obj():
            self.selected=items[0]["id"] if items else None
        self.cam.items=items
        self.cam.selected=self.selected
        issues=model.diagnostics(self.scene)
        self.cam.errors={key for error,ids,_ in issues if error for key in ids}
        self.cam.errors.update(key for key,_ in self.optical_issues)
        self.cam.update()
        self.tree.blockSignals(True)
        self.tree.clear()
        groups={}
        for label in ("Тела","Освещение","Экраны"):
            group=W.QTreeWidgetItem(self.tree,[label])
            group.setFlags(Qt.ItemIsEnabled)
            group.setExpanded(True)
            groups[label]=group
        for obj in items:
            kind=obj["kind"]
            group="Тела" if kind in model.SOLIDS else ("Экраны" if kind=="screen" else "Освещение")
            row=W.QTreeWidgetItem(groups[group],[obj["name"]+(" · справочное" if obj.get("reference") else "")])
            row.setData(0,Qt.UserRole,obj["id"])
            if obj["id"] in self.cam.errors:
                row.setForeground(0,G.QColor("#b34136"))
            if obj["id"]==self.selected:
                self.tree.setCurrentItem(row)
        self.tree.blockSignals(False)
        self.properties()
        messages={m for _,_,m in issues}
        for bad,ids,message in issues:
            if message not in self.prev_issues:
                self.log(message,"error" if bad else "warning",ids)
        self.prev_issues=messages
        busy=self.proc is not None
        blocked=any(error for error,_,_ in issues)
        obj=self.obj() or {}
        self.actions["trace"].setEnabled(not busy and not blocked)
        self.actions["design"].setEnabled(not busy and not blocked)
        self.actions["cancel"].setEnabled(busy)
        self.actions["clear3d"].setEnabled("turntable" in self.scene)
        self.actions["export"].setEnabled(not busy and obj.get("kind") in model.SOLIDS)
        self.actions["cut"].setEnabled(obj.get("kind") in ("box","sphere","cylinder","cad","prism"))
        self.actions["sketch"].setEnabled(obj.get("kind")=="prism")
        self.actions["align"].setEnabled(obj.get("kind") in ("point","area","screen") and not obj["locked"] and "parent" not in obj)
        self.actions["uncut"].setEnabled("cut" in obj)
        self.actions["mate"].setEnabled(bool(obj) and not obj.get("locked") and len(items)>1)
        self.actions["unmate"].setEnabled("parent" in obj)
        self.actions["undo"].setEnabled(bool(self.history))
        self.actions["redo"].setEnabled(bool(self.future))
        self.actions["delete"].setEnabled(bool(obj) and not obj.get("locked"))
        self.setWindowTitle((self.path.stem if self.path else "Новый проект")
                            +(" *" if self.snapshot()!=self.saved else "")+"  —  Caustic CAD")
        status=issues[0][2] if issues else (self.optical_issues[0][1] if self.optical_issues else "Готово к расчёту")
        self.status.setText(self.job_label if busy else status)

    def properties(self):
        previous=self.props.takeWidget()
        if previous:
            previous.deleteLater()
        pane=W.QWidget()
        form=W.QFormLayout(pane)
        form.setContentsMargins(12,12,12,12)
        form.setFieldGrowthPolicy(W.QFormLayout.AllNonFixedFieldsGrow)
        self.props.setWidget(pane)
        obj=self.obj()
        if not obj:
            return
        def heading(txt):
            w=W.QLabel(txt)
            w.setStyleSheet("font-weight:600;color:#39647d;padding-top:8px")
            form.addRow(w)
        def prop(name,value,fn,lo,hi,suffix="",digits=3,enabled=True):
            w=number(value,lo,hi,digits,suffix)
            initial=w.value()
            w.setEnabled(enabled)
            w.editingFinished.connect(lambda:self.guard(lambda:fn(w.value())) if w.value()!=initial else None)
            form.addRow(name,w)
        def setval(field,value):
            self.mutate(lambda:obj.update({field:value}))
        name=W.QLineEdit(obj["name"])
        name.editingFinished.connect(lambda:self.guard(lambda:setval("name",name.text().strip())) if name.text()!=obj["name"] else None)
        form.addRow("Имя",name)
        heading("Геометрия")
        form.addRow("Тип",W.QLabel(model.LABELS[obj["kind"]]))
        for key,val in obj["dims"].items():
            title="Высота экрана" if key=="d" and obj["kind"]=="screen" else DIMS[key]
            prop(title,val,lambda v,k=key:self.mutate(lambda:obj["dims"].update({k:v})),.001,1e6," мм",enabled="profile" not in obj)
        if "cut" in obj:
            form.addRow("Срез, Z",W.QLabel(f'{obj["cut"]:g} мм'))
        if obj["kind"] in model.SOLIDS:
            reference=W.QCheckBox("Справочное тело · вне оптики")
            reference.setChecked(bool(obj.get("reference")))
            reference.toggled.connect(lambda v:self.guard(lambda:setval("reference",v)))
            form.addRow(reference)
            heading("Материал")
            combo=W.QComboBox()
            for key,m in self.scene["materials"].items():
                combo.addItem(m["name"],key)
            combo.setCurrentIndex(combo.findData(obj["material"]))
            def assign(i):
                k=combo.itemData(i)
                self.mutate(lambda:obj.update(material=k,line=self.scene["materials"][k]["default"]))
            combo.currentIndexChanged.connect(lambda i:self.guard(lambda:assign(i)))
            form.addRow("Марка",combo)
            m=self.scene["materials"][obj["material"]]
            wave=W.QComboBox()
            for k in m["indices"]:
                wave.addItem("Номинальное" if k=="nominal" else k+" нм",k)
            wave.setCurrentIndex(wave.findData(obj["line"]))
            wave.currentIndexChanged.connect(lambda i:self.guard(lambda:setval("line",wave.itemData(i))))
            form.addRow("Длина волны",wave)
            form.addRow("Преломление n",W.QLabel(f'{model.index(self.scene,obj):.5f}'))
            vol=self.cam.geometry[obj["id"]][2]
            rho=m["density"]
            form.addRow("Плотность",W.QLabel("Не задана" if rho is None else f"{rho:g} г/см³"))
            if vol is not None:
                form.addRow("Объём",W.QLabel(f"{vol/1000:.4g} см³"))
                if rho is not None:
                    form.addRow("Масса",W.QLabel(f"{vol*rho/1000:.4g} г"))
            if "profile" in obj:
                form.addRow("n при синтезе",W.QLabel(f'{obj["design_n"]:.5f}'))
        if obj["kind"]=="screen":
            turn=self.scene.get("turntable")
            if turn:
                form.addRow("3D-цель",W.QLabel(turn.get("name","3D-модель")))
                form.addRow("Углы по Y",W.QLabel(", ".join(f"{a:g}°" for a in turn["angles"])))
            b=W.QPushButton("Изменить углы…" if turn else ("Заменить изображение…" if "image" in obj else "Назначить изображение…"))
            b.clicked.connect(lambda:self.guard(self.turntable if turn else self.target))
            form.addRow(b)
        heading("Положение")
        if obj.get("parent"):
            form.addRow("Связано с",W.QLabel(self.obj(obj["parent"])["name"]))
        lock=W.QCheckBox("Зафиксировать положение")
        lock.setChecked(obj["locked"])
        lock.setEnabled("parent" not in obj)
        lock.toggled.connect(lambda v:self.guard(lambda:setval("locked",v)))
        form.addRow(lock)
        for field,suffix,description in (("pos"," мм",""),("rot"," °","Угол ")):
            for j,axis in enumerate("XYZ"):
                def change(v,k=field,n=j):
                    update={f:list(obj[f]) for f in ("pos","rot")}
                    update[k][n]=v
                    self.mutate(lambda:model.move(self.scene,obj,**update))
                prop(description+axis,obj[field][j],change,-1e6,1e6,suffix,enabled=not obj["locked"] and "parent" not in obj)

    def drag(self,key,location,done):
        obj=self.obj(key)
        if not obj or obj["locked"] or "parent" in obj:
            return
        if self.drag_start is None:
            self.drag_start=self.snapshot()
        model.move(self.scene,obj,location,obj["rot"])
        if done:
            self.remember(self.drag_start)
            self.drag_start=None
        self.refresh()

    def add(self,kind):
        obj=model.node(kind,260 if kind=="screen" else (-600 if kind in ("point","area") else 0))
        count=sum(i["kind"]==kind for i in self.scene["objects"])
        if count:
            obj["name"]+=" "+str(count+1)
        if kind=="screen":
            obj["pos"][0]=65.*count
        self.selected=obj["id"]
        self.mutate(lambda:self.scene["objects"].append(obj))
        self.tabs.setCurrentIndex(0)
        self.cam.fit()

    def remove(self):
        obj=self.obj()
        if obj and not obj["locked"]:
            def remove():
                if self.scene.get("turntable",{}).get("screen")==obj["id"]:
                    self.scene.pop("turntable")
                for child in self.scene["objects"]:
                    if child.get("parent")==obj["id"]:
                        child.pop("parent")
                self.scene["objects"].remove(obj)
            self.mutate(remove)

    def mate(self):
        obj=self.obj()
        excluded=model.descendants(self.scene,obj["id"])|{obj["id"]}
        parents=[i for i in self.scene["objects"] if i["id"] not in excluded]
        names=[f'{n+1}. {item["name"]}' for n,item in enumerate(parents)]
        if not names:
            raise ValueError("Все остальные объекты уже связаны с выбранным")
        choice,ok=W.QInputDialog.getItem(self,"Жёсткая связь","Следовать за объектом",names,0,False)
        if ok:
            self.mutate(lambda:obj.update(parent=parents[names.index(choice)]["id"]))

    def unmate(self):
        obj=self.obj()
        self.mutate(lambda:obj.pop("parent"))

    def align(self):
        plate,_,_=model.optical_setup(self.scene)
        obj=self.obj()
        local,_=model.relative(plate,obj)
        distance=float(np.linalg.norm(local))
        if obj["kind"]=="screen":
            z=max(plate["dims"]["h"]+plate.get("max_relief",0.)+1,distance)
        else:
            z=-max(1,distance)
        pos=np.array(plate["pos"])+model.rotation(plate)[:,2]*z
        self.mutate(lambda:model.move(self.scene,obj,pos.tolist(),plate["rot"]))
        self.log(f'{obj["name"]} на оптической оси · локальная координата Z {z:.3f} мм')

    def cut(self):
        obj=self.obj()
        pts=self.cam.geometry[obj["id"]][0]
        low,high=pts[:,2].min(),pts[:,2].max()
        value,ok=W.QInputDialog.getDouble(self,"Срез тела","Сохранить часть ниже локальной координаты Z, мм",
                                          obj.get("cut",(low+high)/2),-1e6,1e6,4)
        if ok:
            self.mutate(lambda:obj.update(cut=value))

    def sketch(self,editing):
        from cad_sketch import Sketch
        obj=deepcopy(self.obj()) if editing else model.node("prism")
        dialog=Sketch(self,obj["outline"] if editing else [])
        while dialog.exec()==W.QDialog.Accepted:
            obj["outline"]=deepcopy(dialog.canvas.points)
            try:
                data=visual_geometry(obj,self.assets)
            except Exception as exc:
                W.QMessageBox.warning(self,"Проверьте эскиз",str(exc))
                continue
            self.cache[self.geometry_key(obj)]=data
            self.selected=obj["id"]
            def apply():
                if editing:
                    self.obj().update(obj)
                else:
                    self.scene["objects"].append(obj)
            self.mutate(apply)
            self.tabs.setCurrentIndex(0)
            self.cam.fit()
            return

    def uncut(self):
        obj=self.obj()
        self.mutate(lambda:obj.pop("cut"))

    def confirm(self):
        if self.snapshot()==self.saved:
            return True
        b=W.QMessageBox.question(self,"Проект изменён","Сохранить изменения проекта?",
                                 W.QMessageBox.Save|W.QMessageBox.Discard|W.QMessageBox.Cancel,W.QMessageBox.Save)
        return self.save() if b==W.QMessageBox.Save else b==W.QMessageBox.Discard

    def replace(self,scene,path=None):
        model.validate(scene)
        previous=self.scene
        self.scene=scene
        try:
            self.rebuild_geometry()
        except Exception:
            self.scene=previous
            self.rebuild_geometry()
            raise
        self.history.clear()
        self.future.clear()
        self.invalidate()
        self.path=path
        self.selected=scene["objects"][0]["id"] if scene["objects"] else None
        self.saved=self.snapshot()
        self.tabs.setCurrentIndex(0)
        self.refresh()
        self.cam.fit()

    def new(self):
        if self.confirm():
            self.replace(model.new_scene(catalog()))

    def open(self):
        name,_=W.QFileDialog.getOpenFileName(self,"Открыть проект","","Проект Caustic CAD (*.ccad)")
        if name and self.confirm():
            path=Path(name)
            self.replace(model.load_scene(path,self.assets),path)

    def save(self,choose=False):
        path=self.path
        if choose or not path:
            name,_=W.QFileDialog.getSaveFileName(self,"Сохранить проект",str(path or "Проект.ccad"),"Проект Caustic CAD (*.ccad)")
            if not name:
                return False
            path=Path(name)
            if not path.suffix:
                path=path.with_suffix(".ccad")
        model.save_scene(self.scene,self.assets,path)
        self.saved=self.snapshot()
        self.path=path
        self.refresh()
        return True

    def open_profile(self):
        name,_=W.QFileDialog.getOpenFileName(self,"Открыть расчёт пластины","","Оптический профиль (surface.txt *.txt)")
        if not name or not self.confirm():
            return
        path=Path(name)
        scene=model.new_scene(catalog())
        plate,src,screen=scene["objects"]
        _,width,h,n,distance,z,w,d=self.attach_profile(plate,path)
        matching=next(((key,k) for key,m in scene["materials"].items() for k,v in m["indices"].items()
                       if abs(v-n)<1e-8),None)
        if matching:
            plate["material"],plate["line"]=matching
        else:
            key=uuid4().hex
            scene["materials"][key]=dict(name=f"Из профиля · n {n:g}",density=None,default="nominal",indices={"nominal":n},source="")
            plate.update(material=key,line="nominal")
        src["pos"][2]=-distance
        screen["pos"][2]=z
        screen["dims"]={"w":w,"d":d}
        self.replace(scene)
        self.saved=""
        self.refresh()
        self.log("Открыта рассчитанная поверхность. Исходный показатель преломления сохранён.")

    def attach_profile(self,plate,path):
        with path.open(encoding="ascii") as f:
            header=list(map(float,f.readline().split()))
        p=Profile.read(path)
        plate.update(profile=model.store_asset(path,self.assets),design_n=header[3],min_relief=float(p.relief.min()),max_relief=float(p.relief.max()),
                     dims={"w":p.width,"h":p.thickness})
        return header

    def import_shape(self):
        name,_=W.QFileDialog.getOpenFileName(self,"Импорт модели","","Модели (*.step *.stp *.brep *.stl *.obj)")
        if not name:
            return
        path=Path(name)
        obj=model.node("mesh" if path.suffix.lower() in (".obj",".stl") else "cad")
        obj.update(name=path.stem,asset=model.store_asset(path,self.assets))
        out=self.work/(uuid4().hex+".npz")
        def finished():
            signature=self.geometry_key(obj)
            with np.load(out) as data:
                volume=float(data["volume"])
                self.cache[signature]=(data["xyz"],data["tri"],None if np.isnan(volume) else volume)
            self.selected=obj["id"]
            self.mutate(lambda:self.scene["objects"].append(obj))
            self.tabs.setCurrentIndex(0)
            self.cam.fit()
        self.worker("import","Подготовка импортированной модели…",
                    dict(object=obj,assets=str(self.assets),path=str(out)),finished)

    def target(self):
        if "turntable" in self.scene:
            raise ValueError("Сначала снимите 3D-цель в меню «Оптика»")
        obj=self.obj()
        if not obj or obj["kind"]!="screen":
            raise ValueError("Выберите экран в дереве модели")
        name,_=W.QFileDialog.getOpenFileName(self,"Изображение для экрана","","Изображения (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.pgm)")
        if not name:
            return
        path=Path(name)
        with Image.open(path) as im:
            width,height=ImageOps.exif_transpose(im).size
        key=model.store_asset(path,self.assets)
        def apply():
            obj["image"]=key
            obj["dims"]["d"]=obj["dims"]["w"]*height/width
        self.mutate(apply)
        self.log("Пропорции экрана приведены к исходному изображению.")

    def materials(self):
        dlg=W.QDialog(self)
        dlg.setWindowTitle("Справочник материалов")
        dlg.resize(650,350)
        layout=W.QVBoxLayout(dlg)
        rows=list((catalog()|self.scene["materials"]).items())
        table=W.QTableWidget(len(rows),4)
        table.setHorizontalHeaderLabels(["Материал","n","λ, нм","ρ, г/см³"])
        table.setEditTriggers(W.QAbstractItemView.NoEditTriggers)
        table.setSelectionBehavior(W.QAbstractItemView.SelectRows)
        table.setSelectionMode(W.QAbstractItemView.SingleSelection)
        table.horizontalHeader().setSectionResizeMode(0,W.QHeaderView.Stretch)
        table.verticalHeader().hide()
        selected=(self.obj() or {}).get("material")
        for row,(key,m) in enumerate(rows):
            line=m["default"]
            values=(m["name"],f'{m["indices"][line]:.5f}',line if line!="nominal" else "—",
                    str(m["density"]) if m["density"] is not None else "—")
            for col,value in enumerate(values):
                table.setItem(row,col,W.QTableWidgetItem(value))
            if key==selected:
                table.selectRow(row)
        layout.addWidget(table)
        source=W.QLabel()
        source.setTextInteractionFlags(Qt.TextSelectableByMouse)
        source.setWordWrap(True)
        edit=W.QPushButton("Изменить карточку…")
        def details():
            r=table.currentRow()
            if r>=0:
                m=rows[r][1]
                lines=", ".join(f"{k} нм · {v:.5f}" if k!="nominal" else f"n = {v:.5f}" for k,v in m["indices"].items())
                source.setText(lines+"\n"+str(m.get("source","")))
                edit.setEnabled(rows[r][0] not in model.BUILTINS and len(m["indices"])==1 and m["density"] is not None)
            else:
                edit.setEnabled(False)
        table.itemSelectionChanged.connect(details)
        details()
        layout.addWidget(source)
        buttons=W.QDialogButtonBox(W.QDialogButtonBox.Close)
        new=buttons.addButton("Добавить материал…",W.QDialogButtonBox.ActionRole)
        buttons.addButton(edit,W.QDialogButtonBox.ActionRole)
        use=buttons.addButton("Назначить телу",W.QDialogButtonBox.AcceptRole)
        use.setEnabled((self.obj() or {}).get("kind") in model.SOLIDS)
        buttons.rejected.connect(dlg.reject)
        use.clicked.connect(lambda:dlg.done(1))
        new.clicked.connect(lambda:dlg.done(2))
        edit.clicked.connect(lambda:dlg.done(3))
        layout.addWidget(buttons)
        result=dlg.exec()
        if result==2:
            self.custom_material()
        elif result==3 and table.currentRow()>=0:
            self.custom_material(*rows[table.currentRow()])
        elif result==1 and table.currentRow()>=0:
            key,m=rows[table.currentRow()]
            body=self.obj()
            def assign():
                self.scene["materials"][key]=deepcopy(m)
                body.update(material=key,line=m["default"])
            self.mutate(assign)

    def custom_material(self,key=None,old=None):
        dlg=W.QDialog(self)
        dlg.setWindowTitle("Карточка материала" if old else "Новый материал")
        form=W.QFormLayout(dlg)
        name=W.QLineEdit()
        index=number(1.5,1.00001,6,5)
        wave=number(589.3,1,99999,2," нм")
        rho=number(1.2,.0001,99,4," г/см³")
        source=W.QLineEdit()
        if old:
            name.setText(old["name"])
            index.setValue(old["indices"][old["default"]])
            if old["default"]!="nominal":
                wave.setValue(float(old["default"]))
            rho.setValue(old["density"])
            source.setText(old.get("source",""))
        for label,field in (("Название",name),("Показатель преломления",index),("Длина волны",wave),
                            ("Плотность",rho),("Паспорт / источник",source)):
            form.addRow(label,field)
        buttons=W.QDialogButtonBox(W.QDialogButtonBox.Ok|W.QDialogButtonBox.Cancel)
        buttons.accepted.connect(dlg.accept)
        buttons.rejected.connect(dlg.reject)
        buttons.button(W.QDialogButtonBox.Ok).setEnabled(bool(name.text().strip()))
        name.textChanged.connect(lambda s:buttons.button(W.QDialogButtonBox.Ok).setEnabled(bool(s.strip())))
        form.addRow(buttons)
        if dlg.exec()!=W.QDialog.Accepted:
            return
        key=key or uuid4().hex
        line=f"{wave.value():g}"
        material=dict(name=name.text().strip(),default=line,indices={line:index.value()},
                      density=rho.value(),source=source.text().strip())
        model.validate_material(material)
        save_material(key,material)
        def apply():
            self.scene["materials"][key]=material
            obj=self.obj()
            if old:
                for body in self.scene["objects"]:
                    if body.get("material")==key:
                        body["line"]=line
            elif obj and obj["kind"] in model.SOLIDS:
                obj.update(material=key,line=line)
        self.mutate(apply)

    def launch(self,task,label,program,args,out,callback):
        if self.proc:
            raise ValueError("Дождитесь завершения текущей операции")
        if not Path(program).is_file():
            raise ValueError("Исполняемый файл не найден: "+str(program))
        proc=C.QProcess(self)
        proc.setProcessChannelMode(C.QProcess.MergedChannels)
        self.job_label=label
        self.proc=proc
        revision=self.revision
        output=bytearray()
        stopped=[False]
        self.stop_state=stopped
        def read():
            output.extend(bytes(proc.readAllStandardOutput()))
            if len(output)>32000:
                del output[:-32000]
        def done(code,status):
            read()
            if self.proc is not proc:
                return
            self.proc=None
            self.refresh()
            proc.deleteLater()
            if stopped[0]:
                self.log("Операция остановлена.")
            elif code or status==C.QProcess.CrashExit:
                details=output.decode("utf-8",errors="replace").strip().splitlines()
                self.log("Операция завершилась ошибкой. "+"\n".join(details[-6:]))
            elif task!="export" and revision!=self.revision:
                self.log("Сцена изменилась во время расчёта. Запустите расчёт для текущей версии.")
            else:
                self.guard(callback)
        proc.readyReadStandardOutput.connect(read)
        proc.finished.connect(done)
        proc.errorOccurred.connect(lambda error:done(1,C.QProcess.CrashExit) if error==C.QProcess.FailedToStart else None)
        self.log(label)
        self.refresh()
        proc.start(str(program),[str(a) for a in args])
        C.QTimer.singleShot(600000,proc,lambda:self.cancel() if self.proc is proc else None)

    def worker(self,task,label,request,callback):
        file=self.work/(uuid4().hex+".json")
        file.write_text(json.dumps({"task":task,**request},ensure_ascii=False),encoding="utf-8")
        # pythonw on Windows lacks a console; the worker uses the console interpreter with redirected pipes.
        python=Path(sys.executable)
        if python.stem.lower()=="pythonw":
            python=python.with_name("python.exe")
        self.launch(task,label,python,[ROOT/"tools/cad_worker.py",file],None,callback)

    def export(self):
        obj=self.obj()
        if not obj or obj["kind"] not in model.SOLIDS:
            raise ValueError("Выберите тело для экспорта")
        formats=["STEP (*.step *.stp)","BREP (*.brep)","STL (*.stl)"]
        if obj["kind"]=="mesh":
            formats=["STL (*.stl)","OBJ (*.obj)"]
        if obj["kind"]=="plate":
            formats+=["3MF (*.3mf)","OBJ (*.obj)"]
        name,fmt=W.QFileDialog.getSaveFileName(self,"Экспорт тела",obj["name"],";;".join(formats))
        if not name:
            return
        path=Path(name)
        if not path.suffix:
            path=path.with_suffix("."+fmt.split("*.")[1].split()[0].rstrip(")"))
        cells=None
        if obj.get("profile") and path.suffix.lower() in (".stl",".obj",".3mf"):
            with (self.assets/obj["profile"]).open(encoding="ascii") as f:
                n=int(f.readline().split()[0])
            cells,ok=W.QInputDialog.getInt(self,"Точность сетки","Ячеек на сторону пластины",
                                           min(2048,max(512,2*(n-1))),n-1,2048)
            if not ok:
                return
        self.worker("export","Сохранение геометрии…",dict(object=obj,assets=str(self.assets),path=str(path),cells=cells),
                    lambda:self.log("Модель сохранена · "+str(path)))

    def trace(self):
        problems=[msg for bad,_,msg in model.diagnostics(self.scene) if bad]
        if problems:
            raise ValueError(problems[0])
        plate,_,screens=model.optical_setup(self.scene)
        rays,max_px=self.quality()
        out=self.work/uuid4().hex
        cfg=out.with_suffix(".txt")
        cfg.write_text(model.trace_settings(self.scene,max_px,rays),encoding="ascii")
        profile=self.assets/plate["profile"] if "profile" in plate else "-"
        names=[s["name"] for s in screens]
        def finished():
            report=json.loads((out/"report.json").read_text())
            self.result=(out,report)
            self.optical_issues=[]
            self.screen_choice.clear()
            self.screen_choice.addItems(names)
            self.show_image()
            self.tabs.setCurrentIndex(1)
            self.log(f'Проверено {report["rays"]:,} лучей при n = {report["n"]:g}. '
                     f'На экранах {report["hit"]:,}, мимо экранов {report["miss"]:,}, '
                     f'через боковую грань {report["side"]:,}, полное внутреннее отражение {report["tir"]:,}, '
                     f'отклонено на входе {report["rejected"]:,}.')
            for obj,name,s in zip(screens,names,report["screens"]):
                self.log(f'{name} · относительный световой поток {s["flux"]:.6g}')
                if s["flux"]==0:
                    msg=f'{name}: ни один проверочный луч не попал на экран. Проверьте положение и ориентацию.'
                    self.optical_issues.append((obj["id"],msg))
                    self.log(msg,"warning",[obj["id"]])
            self.refresh()
        self.launch("trace","Проверка проекции…",CLI,["trace-scene",cfg,profile,out],out,finished)

    def quality(self):
        return getattr(self,"ray_count",2000000),getattr(self,"resolution",256)

    def settings(self):
        dlg=W.QDialog(self)
        dlg.setWindowTitle("Параметры проверки")
        form=W.QFormLayout(dlg)
        rays=W.QSpinBox()
        rays.setRange(1000,200000000)
        rays.setSingleStep(100000)
        rays.setValue(self.quality()[0])
        resolution=W.QSpinBox()
        resolution.setRange(16,1024)
        resolution.setValue(self.quality()[1])
        form.addRow("Лучи",rays)
        form.addRow("Длинная сторона экрана, пикс.",resolution)
        buttons=W.QDialogButtonBox(W.QDialogButtonBox.Ok|W.QDialogButtonBox.Cancel)
        buttons.accepted.connect(dlg.accept)
        buttons.rejected.connect(dlg.reject)
        form.addRow(buttons)
        if dlg.exec()==W.QDialog.Accepted:
            self.ray_count,self.resolution=rays.value(),resolution.value()

    def design(self):
        plate,lamp,screens=model.optical_setup(self.scene)
        if any("image" not in s for s in screens) and "turntable" not in self.scene:
            raise ValueError("Назначьте изображение каждому экрану или выберите 3D-цель")
        if "turntable" in self.scene:
            return self.fit_scene(plate)
        try:
            plate,screen,source,dist=model.design_geometry(self.scene)
        except ValueError:
            return self.fit_scene(plate)
        quality,ok=W.QInputDialog.getItem(self,"Построение рельефа","Точность синтеза",
                 ["Эскиз · 96 пикселей","Стандарт · 256 пикселей","Детально · 448 пикселей"],1,False)
        if not ok:
            return
        settings={"Эскиз":(96,96,96,1000000,0),"Стандарт":(256,320,320,8000000,1),
                  "Детально":(448,512,640,36000000,2)}
        res,grid,mapsize,rays,passes=settings[quality.split(" ·")[0]]
        path=self.assets/screen["image"]
        with Image.open(path) as im:
            w,h=ImageOps.exif_transpose(im).size
        if not np.isclose(screen["dims"]["w"]*h/w,screen["dims"]["d"],rtol=.005):
            raise ValueError("Восстановите пропорции экрана по изображению, назначив его повторно.")
        out=self.work/uuid4().hex
        thick=plate["dims"]["h"]
        args=[ROOT/"tools/design_photo.py",path,"--out",out,"--grid",grid,"--map-size",mapsize,
              "--resolution",res,"--rays",rays,"--cells",grid,"--corrections",passes,
              "--index",model.index(self.scene,plate),"--plate",plate["dims"]["w"],"--thickness",thick,
              "--width",screen["dims"]["w"],"--source",source,"--screen",dist,
              "--max-relief",min(thick*.9,(dist-thick)*.9),"--formats","stl"]
        def finished():
            report=json.loads((out/"report.json").read_text())
            self.mutate(lambda:self.attach_profile(plate,out/"surface.txt"))
            self.log(f'Рельеф построен · n = {plate["design_n"]:g}, ошибка яркости (относительная RMS) '
                     f'{report["picture_relative_rmse"]:.3f}.')
            self.result=(out,{"screens":[{"image":"03_projection.png"}]})
            self.screen_choice.clear()
            self.screen_choice.addItem(screen["name"])
            self.show_image()
            self.tabs.setCurrentIndex(1)
        self.worker("design","Построение оптической поверхности…",dict(args=list(map(str,args))),finished)

    def turntable(self):
        screens=[i for i in self.scene["objects"] if i["kind"]=="screen"]
        if len(screens)!=1:
            raise ValueError("Для серии видов оставьте один экран")
        previous=self.scene.get("turntable")
        if previous:
            key,name=previous["asset"],previous["name"]
        else:
            filename,_=W.QFileDialog.getOpenFileName(self,"Модель для серии видов","","3D-модели (*.obj *.stl *.step *.stp *.brep)")
            if not filename:
                return
            source=Path(filename)
            key,name=model.store_asset(source,self.assets),source.name
        angles,ok=W.QInputDialog.getText(self,"Виды модели","Углы вокруг оси Y в градусах, через пробел",
                 text=" ".join(f"{a:g}" for a in previous["angles"]) if previous else "-12 0 12")
        if not ok:
            return
        values=[float(a.replace(",",".")) for a in angles.split()]
        def apply():
            self.scene["turntable"]=dict(asset=key,name=name,screen=screens[0]["id"],angles=values)
            screens[0].pop("image",None)
        self.mutate(apply)
        self.select(screens[0]["id"])

    def clear_turntable(self):
        if "turntable" in self.scene:
            self.mutate(lambda:self.scene.pop("turntable"))

    def fit_scene(self,plate):
        folder=self.work/uuid4().hex
        def finished():
            out=folder/"result"
            report=json.loads((out/"validation.json").read_text())
            if report["final"]<report["initial"] and any(s["flux"]>0 for s in report["screens"]):
                self.mutate(lambda:self.attach_profile(plate,out/"surface.txt"))
            else:
                self.log("Подбор не улучшил плоскую заготовку. Рельеф в модели сохранён.")
                return
            self.result=(out,report)
            self.screen_choice.clear()
            for row in report["screens"]:
                tag=" · промежуточный вид" if row["intermediate"] else ""
                name=f'{row["name"]} · {row["angle"]:g}°{tag}'
                self.screen_choice.addItem(name)
                self.log(f'{name} · ошибка яркости {row["error"]:.3f}, поток {row["flux"]:.5g}')
            self.show_image()
            self.tabs.setCurrentIndex(1)
            self.log(f'Эскизный подбор общей поверхности · {report["evaluations"]} оценок, '
                     f'критерий {report["initial"]:.4g} → {report["final"]:.4g}.')
        self.worker("fit","Эскизный подбор общей поверхности для всех целей…",
                    dict(scene=self.scene,assets=str(self.assets),folder=str(folder)),finished)

    def show_image(self,*ignored):
        if not self.result:
            return
        idx=self.screen_choice.currentIndex()
        if idx<0:
            return
        out,report=self.result
        path=out/report["screens"][idx]["image"]
        pix=G.QPixmap(str(path))
        self.picture.setPixmap(pix.scaled(max(160,self.tabs.width()-60),max(140,self.tabs.height()-115),
                                          Qt.KeepAspectRatio,Qt.SmoothTransformation))

    def save_projection(self):
        if not self.result:
            raise ValueError("Сначала выполните проверку проекции")
        out,report=self.result
        src=out/report["screens"][self.screen_choice.currentIndex()]["image"]
        path,_=W.QFileDialog.getSaveFileName(self,"Сохранить проекцию","Проекция.png","PNG (*.png)")
        if path:
            shutil.copyfile(src,path if Path(path).suffix else path+".png")

    def cancel(self):
        if self.proc:
            self.stop_state[0]=True
            self.proc.kill()

    def closeEvent(self,event):
        if not self.guard(self.confirm):
            event.ignore()
            return
        if self.proc:
            proc=self.proc
            self.cancel()
            proc.waitForFinished(5000)
        super().closeEvent(event)


def main():
    app=W.QApplication(sys.argv)
    app.setApplicationName("Caustic CAD")
    app.setOrganizationName("Caustic CAD")
    setup_style(app)
    window=Window()
    if len(sys.argv)>1:
        path=Path(sys.argv[1]).resolve()
        window.guard(lambda:window.replace(model.load_scene(path,window.assets),path))
    window.show()
    sys.exit(app.exec())


if __name__=="__main__":
    main()
