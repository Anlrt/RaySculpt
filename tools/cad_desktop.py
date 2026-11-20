"""Caustic CAD desktop application."""
import sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
VERSION=(ROOT/"VERSION").read_text(encoding="ascii").strip()
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
from PySide6 import QtCore as C, QtGui as G, QtWidgets as W
from PySide6.QtCore import Qt

import cad_scene as model
import cad_topology as topo
from cad_commands import ModelingCommands
from cad_compare import Comparison, TargetDialog, comparisons
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
QToolButton:checked {background:#c4deef;color:#14384f;border-color:#6592ad}
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
ICON_DIR=ROOT/"tools/icons"


def setup_style(app):
    if os.name=="nt" and "Segoe UI" not in G.QFontDatabase.families():
        fonts=Path(os.environ.get("WINDIR","C:/Windows"))/"Fonts"
        for name in ("segoeui.ttf","seguisb.ttf","segoeuib.ttf"):
            G.QFontDatabase.addApplicationFont(str(fonts/name))
    app.setStyle("Fusion")
    app.styleHints().setColorScheme(Qt.ColorScheme.Light)
    palette=G.QPalette()
    colors={"Window":"#eff3f5","WindowText":"#233844","Base":"#ffffff","AlternateBase":"#f0f5f8",
            "Text":"#233844","Button":"#f3f7fa","ButtonText":"#233844","Highlight":"#c8e2f1",
            "HighlightedText":"#15394c","ToolTipBase":"#fff7dc","ToolTipText":"#213541",
            "PlaceholderText":"#566975","Link":"#165b85","BrightText":"#ffffff",
            "Light":"#ffffff","Midlight":"#e6edf1","Mid":"#a8bac4","Dark":"#6a818d","Shadow":"#475964"}
    for role,color in colors.items():
        palette.setColor(getattr(G.QPalette.ColorRole,role),G.QColor(color))
    for role in ("WindowText","Text","ButtonText","PlaceholderText"):
        palette.setColor(G.QPalette.Disabled,getattr(G.QPalette.ColorRole,role),G.QColor("#5b6b73"))
    app.setPalette(palette)
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


class Window(ModelingCommands,W.QMainWindow):
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
        self.document_epoch=0
        self.proc=None
        self.result=None
        self.result_signature=None
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
        self.picture=Comparison()
        self.result_status=W.QLabel("Выполните проверку проекции")
        select_result=W.QHBoxLayout()
        select_result.addWidget(self.screen_choice,1)
        reset=W.QPushButton("Вписать")
        reset.clicked.connect(self.picture.reset)
        select_result.addWidget(reset)
        self.save_result=W.QPushButton("Сохранить проекцию…")
        self.save_result.clicked.connect(lambda:self.guard(self.save_projection))
        select_result.addWidget(self.save_result)
        result_layout.addLayout(select_result)
        result_layout.addWidget(self.result_status)
        result_layout.addWidget(self.picture,1)
        self.legend=W.QLabel()
        self.legend.setWordWrap(True)
        result_layout.addWidget(self.legend)
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
        self.modeling_actions()
        self.refresh()
        C.QTimer.singleShot(0,lambda:self.cam.fit(True))

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
        if key and (ICON_DIR/(key+".svg")).is_file():
            a.setIcon(G.QIcon(str(ICON_DIR/(key+".svg"))))
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
        bar.setIconSize(C.QSize(22,22))
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
        move=self.act(edit,"Переместить",self.toggle_move,"M","move",bar)
        move.setCheckable(True)
        move.setToolTip("Стрелки X, Y, Z перемещают вдоль оси. Перетаскивание тела перемещает в плоскости вида (M).")
        bar.addSeparator()
        insert=W.QToolButton()
        insert.setIcon(G.QIcon(str(ICON_DIR/"solid.svg")))
        insert.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
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
        self.optics_bar=self.addToolBar("Оптика")
        self.insertToolBarBreak(self.optics_bar)
        self.optics_bar.setMovable(False)
        self.optics_bar.setIconSize(C.QSize(22,22))
        self.optics_bar.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        source=W.QToolButton()
        source.setText("Источник")
        source.setIcon(G.QIcon(str(ICON_DIR/"light.svg")))
        source.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        source.setPopupMode(W.QToolButton.InstantPopup)
        sources=W.QMenu(source)
        for kind in ("point","area","sun"):
            self.act(sources,model.LABELS[kind],partial(self.set_source,kind))
            self.act(optics,"Добавить · "+model.LABELS[kind],partial(self.add,kind))
        source.setToolTip("Выбрать тип источника в оптической установке")
        source.setMenu(sources)
        self.optics_bar.addWidget(source)
        self.act(optics,"Добавить экран",partial(self.add,"screen"),key="screen",bar=self.optics_bar)
        self.act(optics,"Назначить цель…",self.target,key="target",bar=self.optics_bar)
        self.act(optics,"3D-цель и углы поворота…",self.turntable)
        self.act(optics,"Снять 3D-цель",self.clear_turntable,key="clear3d")
        self.act(optics,"Подобрать расстояния…",self.placement,key="placement",bar=self.optics_bar)
        settings=self.act(optics,"Параметры проверки…",self.settings,key="setup")
        self.optics_bar.addSeparator()
        self.act(optics,"Выровнять по оси пластины",self.align,key="align")
        self.act(optics,"Построить рельеф…",self.design,key="design",bar=self.optics_bar)
        self.act(optics,"Проверить проекцию",self.trace,"F5","trace",self.optics_bar)
        self.act(optics,"Сравнить",lambda:self.tabs.setCurrentIndex(1),key="compare",bar=self.optics_bar)
        self.optics_bar.addAction(settings)
        self.act(optics,"Остановить",self.cancel,key="cancel",bar=self.optics_bar)
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

    def toggle_move(self):
        self.cam.move_mode=self.actions["move"].isChecked()
        if self.cam.move_mode:
            self.pick_selector.setCurrentIndex(0)
        self.cam.setCursor(Qt.OpenHandCursor if self.cam.move_mode else Qt.ArrowCursor)
        self.cam.update()
        if self.cam.move_mode:
            self.statusBar().showMessage("Тяните стрелку X, Y или Z вдоль оси, либо само тело в плоскости вида.",6000)

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
        self.invalidate(old)

    def invalidate(self,old=None):
        self.revision+=1
        self.inspection=[]
        if old is None or model.optical_signature(json.loads(old))!=model.optical_signature(self.scene):
            self.optical_issues=[]

    def mutate(self,fn):
        old=self.snapshot()
        try:
            fn()
            self.scene["version"]=2
            model.validate(self.scene)
            topo.rebuild_mates(self.scene,self.assets)
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
            self.invalidate(self.future[-1])
            self.refresh()

    def redo(self):
        if self.future:
            self.history.append(self.snapshot())
            self.scene=json.loads(self.future.pop())
            self.invalidate(self.history[-1])
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
            step=current.data(0,Qt.UserRole+1)
            self.active_step=(key,step) if step else None
            if key:
                self.select(key)

    def rebuild_geometry(self):
        geometry={}
        used=set()
        for obj in self.scene["objects"]:
            if obj["kind"] not in model.SOLIDS:
                continue
            data=topo.resolved(self.scene,obj)
            signature=self.geometry_key(data)
            used.add(signature)
            if signature not in self.cache:
                self.cache[signature]=visual_geometry(data,self.assets)
            geometry[obj["id"]]=self.cache[signature]
        self.cache={k:v for k,v in self.cache.items() if k in used}
        self.cam.geometry=geometry

    @staticmethod
    def geometry_key(obj):
        return json.dumps([obj.get(k) for k in ("kind","dims","profile","asset","cut","outline","operations","circle","arcs")],sort_keys=True)

    def refresh(self):
        self.rebuild_geometry()
        items=self.scene["objects"]
        if not self.obj():
            self.selected=items[0]["id"] if items else None
        if self.active_step:
            obj=self.obj()
            steps=({op["id"] for op in obj.get("operations",[])} | set(obj) | {"base","height"}) if obj else set()
            if self.active_step[0]!=self.selected or self.active_step[1] not in steps:
                self.active_step=None
        self.cam.items=items
        self.cam.consumed=model.consumed_tools(self.scene)
        self.cam.selected=self.selected
        issues=model.diagnostics(self.scene)
        self.cam.errors={key for error,ids,_ in issues if error for key in ids}
        self.cam.errors.update(k for issue in self.inspection for k in issue["objects"])
        self.cam.errors.update(key for key,_ in self.optical_issues)
        self.cam.update()
        self.tree.blockSignals(True)
        expanded=set()
        for i in range(self.tree.topLevelItemCount()):
            group=self.tree.topLevelItem(i)
            for j in range(group.childCount()):
                row=group.child(j)
                if row.isExpanded():
                    expanded.add(row.data(0,Qt.UserRole))
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
            role=" · инструмент" if obj["id"] in self.cam.consumed else (" · справочное" if obj.get("reference") else "")
            row=W.QTreeWidgetItem(groups[group],[obj["name"]+role])
            row.setData(0,Qt.UserRole,obj["id"])
            row.setExpanded(obj["id"] in expanded)
            self.feature_tree(row,obj)
            if obj["id"] in self.cam.errors:
                row.setForeground(0,G.QColor("#b34136"))
            if obj["id"]==self.selected and not (self.active_step and self.active_step[0]==obj["id"]):
                self.tree.setCurrentItem(row)
        self.tree.blockSignals(False)
        self.properties()
        self.refresh_picks()
        messages={m for _,_,m in issues}
        for bad,ids,message in issues:
            if message not in self.prev_issues:
                self.log(message,"error" if bad else "warning",ids)
        self.prev_issues=messages
        busy=self.proc is not None
        blocked=any(error for error,_,_ in issues)
        obj=self.obj() or {}
        self.actions["trace"].setEnabled(not busy and not blocked)
        screens=[i for i in items if i["kind"]=="screen"]
        turn="turntable" in self.scene
        missing=[s for s in screens if "image" not in s and not turn]
        self.actions["design"].setEnabled(not busy and not blocked and not missing)
        self.actions["target"].setEnabled(not busy and bool(screens) and not turn)
        self.actions["screen"].setEnabled(not turn)
        self.actions["placement"].setEnabled(not busy and bool(screens))
        self.actions["compare"].setEnabled(self.result is not None)
        self.save_result.setEnabled(self.result is not None)
        self.update_result_status()
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
                            +(" *" if self.snapshot()!=self.saved else "")+"  —  Caustic CAD "+VERSION)
        status=next((msg for bad,_,msg in issues if bad),None)
        if status is None:
            if missing:
                status=f'Назначьте цель для «{missing[0]["name"]}». Проверка текущей пластины доступна (F5).'
            else:
                status=issues[0][2] if issues else (self.optical_issues[0][1] if self.optical_issues else "Готово к расчёту")
        self.actions["design"].setToolTip(status if blocked or missing else "Рассчитать поверхность по назначенным целям")
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
            visible=W.QCheckBox("Показывать тело")
            available=obj["id"] not in self.cam.consumed
            visible.setChecked(available and not obj.get("hidden"))
            visible.setEnabled(available)
            visible.toggled.connect(lambda v:self.guard(lambda:setval("hidden",not v)))
            form.addRow(visible)
            reference=W.QCheckBox("Справочное тело · вне оптики")
            reference.setChecked(not available or bool(obj.get("reference")))
            reference.setEnabled(available)
            reference.toggled.connect(lambda v:self.guard(lambda:setval("reference",v)))
            form.addRow(reference)
            if obj.get("reference") and available:
                note=W.QLabel("Служит для компоновки. Лучи проходят сквозь это тело.")
                note.setWordWrap(True)
                form.addRow(note)
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
            if obj.get("image"):
                mapping=W.QPushButton("Размещение изображения…")
                mapping.clicked.connect(lambda:self.guard(lambda:self.place_target(obj,self.assets/obj["image"])))
                form.addRow(mapping)
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
        if not done:
            issues=model.diagnostics(self.scene)
            self.cam.errors={k for bad,ids,_ in issues if bad for k in ids}
            self.status.setText(issues[0][2] if issues else "Перемещение · отпустите кнопку для перестроения")
            self.cam.update()
            return
        try:
            topo.rebuild_mates(self.scene,self.assets)
            self.rebuild_geometry()
        except Exception as exc:
            self.scene=json.loads(self.drag_start)
            self.log(str(exc),"error",[key])
        else:
            self.remember(self.drag_start)
        self.drag_start=None
        self.refresh()

    def add(self,kind):
        obj=model.node(kind)
        count=sum(i["kind"]==kind for i in self.scene["objects"])
        if count:
            obj["name"]+=" "+str(count+1)
        plate=next((i for i in self.scene["objects"] if i["kind"]=="plate"),None)
        if kind in ("point","area","sun","screen") and plate:
            axis=model.rotation(plate)
            offset=[65.*count if kind=="screen" else 0,0,125. if kind=="screen" else -240.]
            obj.update(pos=(np.asarray(plate["pos"])+axis@offset).tolist(),rot=plate["rot"][:])
        elif kind in model.SOLIDS and plate:
            obj["pos"]=(np.asarray(plate["pos"])+model.rotation(plate)[:,0]*(45+count*45)).tolist()
        self.selected=obj["id"]
        self.mutate(lambda:self.scene["objects"].append(obj))
        self.tabs.setCurrentIndex(0)
        self.cam.fit(True)

    def set_source(self,kind):
        lamps=[i for i in self.scene["objects"] if i["kind"] in ("point","area","sun")]
        if not lamps:
            return self.add(kind)
        lamp=self.obj() if self.obj() in lamps else lamps[0]
        if lamp["kind"]!=kind:
            def change():
                if lamp["name"]==model.LABELS[lamp["kind"]]:
                    lamp["name"]=model.LABELS[kind]
                lamp.update(kind=kind,dims=model.DIMS.get(kind,{}).copy())
            self.mutate(change)
        self.select(lamp["id"])

    def remove(self):
        if self.active_step:
            return self.delete_step()
        obj=self.obj()
        if obj and not obj["locked"]:
            def remove():
                if self.scene.get("turntable",{}).get("screen")==obj["id"]:
                    self.scene.pop("turntable")
                for child in self.scene["objects"]:
                    if child.get("parent")==obj["id"]:
                        child.pop("parent")
                        child.pop("constraint",None)
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
            def attach():
                obj.pop("constraint",None)
                obj.update(parent=parents[names.index(choice)]["id"])
            self.mutate(attach)

    def unmate(self):
        obj=self.obj()
        self.mutate(lambda:(obj.pop("parent"),obj.pop("constraint",None)))

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

    def sketch(self,editing,support=None,plane=None):
        from cad_sketch import Sketch
        obj=deepcopy(self.obj()) if editing else model.node("prism")
        dialog=Sketch(self,obj["outline"] if editing else [],obj.get("circle"),obj.get("arcs"))
        if plane:
            obj["rot"]={"XY":[0.,0.,0.],"XZ":[90.,0.,0.],"YZ":[90.,0.,90.]}[plane]
            dialog.setWindowTitle("Эскиз на плоскости "+plane)
        if support:
            anchor,b=topo.world_frame(self.scene,support,self.assets)
            obj.update(pos=anchor.tolist(),rot=topo.euler(b),parent=support["object"])
            obj["constraint"]=dict(base=support,own=dict(object=obj["id"],kind="vertex_origin",index=0),angle=0.,distance=0.)
            dialog.setWindowTitle("Эскиз на выбранной грани")
        while dialog.exec()==W.QDialog.Accepted:
            obj["outline"]=deepcopy(dialog.canvas.points)
            obj["arcs"]=dict(dialog.canvas.arcs)
            if dialog.canvas.circle:
                obj["circle"]=dialog.canvas.circle[:]
            else:
                obj.pop("circle",None)
            try:
                resolved=topo.resolved(self.scene,obj)
                data=visual_geometry(resolved,self.assets)
            except Exception as exc:
                W.QMessageBox.warning(self,"Проверьте эскиз",str(exc))
                continue
            self.cache[self.geometry_key(resolved)]=data
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
        topo.rebuild_mates(scene,self.assets)
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
        self.result=None
        self.result_signature=None
        self.document_epoch+=1
        self.screen_choice.clear()
        self.picture.set_images([])
        self.legend.clear()
        self.path=path
        self.selected=scene["objects"][0]["id"] if scene["objects"] else None
        self.saved=self.snapshot()
        self.tabs.setCurrentIndex(0)
        self.picks.clear()
        self.refresh()
        self.cam.fit(True)

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
        axis=model.rotation(plate)[:,2]
        src["pos"]=(np.asarray(plate["pos"])-distance*axis).tolist()
        screen["pos"]=(np.asarray(plate["pos"])+z*axis).tolist()
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
            screens=[i for i in self.scene["objects"] if i["kind"]=="screen"]
            obj=next((i for i in screens if "image" not in i),screens[0] if screens else None)
        if obj is None:
            raise ValueError("Добавьте экран")
        name,_=W.QFileDialog.getOpenFileName(self,"Изображение для экрана","","Изображения (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.pgm)")
        if not name:
            return
        path=Path(name)
        self.place_target(obj,path)

    def place_target(self,obj,path):
        dialog=TargetDialog(self,path,obj)
        if dialog.exec()!=W.QDialog.Accepted:
            return
        mode=dialog.mode.currentData()
        key=model.store_asset(path,self.assets)
        def apply():
            obj["image"]=key
            obj["image_fit"]="crop" if mode=="crop" else "fit"
            if mode=="resize":
                obj["dims"]["d"]=dialog.height_mm
        self.mutate(apply)
        self.select(obj["id"])
        self.log(f'{obj["name"]} · {dialog.mode.currentText()}')

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
        epoch=self.document_epoch
        signature=model.optical_signature(self.scene)
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
            if task in ("trace","checked_trace","design","fit"):
                changed=model.optical_signature(self.scene)!=signature
            else:
                changed=revision!=self.revision
            if stopped[0]:
                self.log("Операция остановлена.")
            elif code or status==C.QProcess.CrashExit:
                details=output.decode("utf-8",errors="replace").strip().splitlines()
                self.log("Операция завершилась ошибкой. "+"\n".join(details[-6:]))
            elif task!="export" and (epoch!=self.document_epoch or changed):
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
        export_item=dict(topo.resolved(self.scene,obj),pos=obj["pos"],rot=obj["rot"])
        self.worker("export","Сохранение геометрии…",dict(object=export_item,assets=str(self.assets),path=str(path),cells=cells),
                    lambda:self.log("Модель сохранена · "+str(path)))

    def trace(self):
        problems=[msg for bad,_,msg in model.diagnostics(self.scene) if bad]
        if problems:
            raise ValueError(problems[0])
        plate,_,screens=model.optical_setup(self.scene)
        screens=deepcopy(screens)
        rays,max_px=self.quality()
        out=self.work/uuid4().hex
        cfg=out.with_suffix(".txt")
        cfg.write_text(model.trace_settings(self.scene,max_px,rays),encoding="ascii")
        profile=self.assets/plate["profile"] if "profile" in plate else "-"
        names=[s["name"] for s in screens]
        issues_file=out.with_suffix(".issues.json")
        def finished():
            if issues_file.is_file():
                self.show_inspection(json.loads(issues_file.read_text(encoding="utf-8")))
                return
            report=json.loads((out/"report.json").read_text())
            for i,(row,screen) in enumerate(zip(report["screens"],screens)):
                row["screen"]=screen["id"]
                if "image" in screen or self.scene.get("turntable"):
                    row["target"]=f"target{i}.pgm"
                if self.scene.get("turntable"):
                    row["angle"]=0.
            self.set_result(out,report)
            self.optical_issues=[]
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
        args=["trace-scene",str(cfg),str(profile),str(out)]
        nearby=model.potential_relief_collisions(self.scene)
        if nearby or "turntable" in self.scene or any("image" in s for s in screens):
            self.worker("checked_trace","Проверка проекции и целей…",
                        dict(scene=dict(objects=nearby),assets=str(self.assets),path=str(issues_file),
                             screens=screens,turntable=self.scene.get("turntable"),resolution=max_px,out=str(out),
                             cli=str(CLI),args=args),finished)
        else:
            self.launch("trace","Проверка проекции…",CLI,args,out,finished)

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
        out=self.work/uuid4().hex
        thick=plate["dims"]["h"]
        args=[ROOT/"tools/design_photo.py",path,"--out",out,"--grid",grid,"--map-size",mapsize,
              "--resolution",res,"--rays",rays,"--cells",grid,"--corrections",passes,
              "--index",model.index(self.scene,plate),"--plate",plate["dims"]["w"],"--thickness",thick,
              "--width",screen["dims"]["w"],"--height",screen["dims"]["d"],"--image-fit",screen.get("image_fit","fit"),
              "--source",source,"--screen",dist,
              "--max-relief",min(thick*.9,(dist-thick)*.9),"--formats","stl"]
        def finished():
            report=json.loads((out/"report.json").read_text())
            self.mutate(lambda:self.attach_profile(self.obj(plate["id"]),out/"surface.txt"))
            self.log(f'Рельеф построен · n = {self.obj(plate["id"])["design_n"]:g}, ошибка яркости (относительная RMS) '
                     f'{report["picture_relative_rmse"]:.3f}.')
            self.set_result(out,{"screens":[dict(screen=screen["id"],image="03_projection.png",
                            target="01_monochrome.png",display_gain=1.)]})
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
            screens[0].pop("image_fit",None)
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
                self.mutate(lambda:self.attach_profile(self.obj(plate["id"]),out/"surface.txt"))
            else:
                self.log("Подбор не улучшил плоскую заготовку. Рельеф в модели сохранён.")
                return
            self.set_result(out,report)
            for row in report["screens"]:
                tag=" · промежуточный вид" if row["intermediate"] else ""
                name=f'{row["name"]} · {row["angle"]:g}°{tag}'
                self.log(f'{name} · ошибка яркости {row["error"]:.3f}, поток {row["flux"]:.5g}')
            self.log(f'Эскизный подбор общей поверхности · {report["evaluations"]} оценок, '
                     f'критерий {report["initial"]:.4g} → {report["final"]:.4g}.')
        self.worker("fit","Эскизный подбор общей поверхности для всех целей…",
                    dict(scene=self.scene,assets=str(self.assets),folder=str(folder)),finished)

    def set_result(self,out,report):
        self.result=(out,report)
        self.result_signature=model.optical_signature(self.scene)
        self.screen_choice.blockSignals(True)
        self.screen_choice.clear()
        for row in report["screens"]:
            obj=self.obj(row["screen"])
            label=obj["name"] if obj else row.get("name","Экран")
            row["screen_label"]=label
            self.screen_choice.addItem(label)
        self.screen_choice.blockSignals(False)
        self.update_result_status()
        self.show_image()
        self.tabs.setCurrentIndex(1)
        self.actions["compare"].setEnabled(True)
        self.save_result.setEnabled(True)

    def result_stale(self):
        return self.result is not None and model.optical_signature(self.scene)!=self.result_signature

    def update_result_status(self):
        if not self.result:
            self.result_status.setText("Выполните проверку проекции (F5)")
            return
        stale=self.result_stale()
        self.result_status.setText("Предыдущий расчёт · оптическая схема изменилась. F5 — рассчитать заново."
                                   if stale else "Расчёт соответствует текущей оптической схеме")
        self.result_status.setStyleSheet("color:#864813;font-weight:600" if stale else "color:#316041")
        for i,row in enumerate(self.result[1]["screens"]):
            obj=self.obj(row["screen"])
            label=obj["name"] if obj else row["screen_label"]
            if "angle" in row:
                label+=f' · {row["angle"]:g}°'
                if row.get("intermediate"):
                    label+=" · промежуточный вид"
            self.screen_choice.setItemText(i,label)

    def show_image(self,*ignored):
        if not self.result:
            return
        idx=self.screen_choice.currentIndex()
        if idx<0:
            return
        out,report=self.result
        row=report["screens"][idx]
        target=out/row["target"] if row.get("target") else None
        self.picture.set_images(comparisons(out/row["image"],target,
            gain=row.get("display_gain")))
        self.legend.setText(("Синий — недостаток света, оранжевый — избыток, светлый — совпадение. "
                            +("Экспозиция из расчёта." if "display_gain" in row else "Экспозиция подобрана для этого экрана."))
                            if target else "Проверка светового пятна. У этого расчёта нет изображения для сравнения.")

    def save_projection(self):
        if not self.result:
            raise ValueError("Сначала выполните проверку проекции")
        out,report=self.result
        src=out/report["screens"][self.screen_choice.currentIndex()]["image"]
        title="Проекция предыдущего расчёта.png" if self.result_stale() else "Проекция.png"
        path,_=W.QFileDialog.getSaveFileName(self,"Сохранить проекцию",title,"PNG (*.png)")
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
