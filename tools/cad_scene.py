"""Portable scene documents. Lengths in mm, density in g/cm³, Euler angles in degrees."""
from copy import deepcopy
from hashlib import sha256
import json
import math
import os
from pathlib import Path
import re
import shutil
import tempfile
from uuid import uuid4
from zipfile import ZipFile, ZIP_DEFLATED

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
SOLIDS = {"plate", "box", "sphere", "cylinder", "prism", "cad", "mesh"}
LABELS = {"plate":"Оптическая пластина", "box":"Блок", "sphere":"Сфера", "cylinder":"Цилиндр",
          "sun":"Солнечный свет",
          "screen":"Экран", "point":"Точечный источник", "area":"Прямоугольный источник",
          "cad":"Тело CAD", "mesh":"Полигональная модель", "prism":"Выдавливание"}
DIMS = {"plate":{"w":40., "h":8.}, "box":{"w":40., "d":32., "h":24.},
        "sphere":{"r":20.}, "cylinder":{"r":18., "h":32.}, "area":{"w":10., "d":10.},
        "screen":{"w":50., "d":50.}, "prism":{"h":15.}}
BUILTINS = json.loads((ROOT/"tools/materials.json").read_text(encoding="utf-8"))
ASSET = re.compile(r"[a-f0-9]{64}\.[a-z0-9]{1,8}")
PROJECT_LIMIT = 1024**3


def node(kind, z=0.):
    item = dict(id=uuid4().hex, kind=kind, name=LABELS[kind], pos=[0.,0.,z],
                rot=[0.,0.,0.], locked=False, dims=DIMS.get(kind, {}).copy())
    if kind in SOLIDS:
        item.update(material="pmma", line=BUILTINS["pmma"]["default"])
    if kind=="prism":
        item["outline"]=[[-20.,-15.],[20.,-15.],[20.,15.],[-20.,15.]]
    return item


def new_scene(catalog=None):
    objects=[node("plate"),node("point"),node("screen")]
    for item,x in zip(objects,(0.,-240.,125.)):
        item.update(pos=[x,0.,25.],rot=[90.,0.,90.])
    return dict(version=2, materials=deepcopy(BUILTINS | (catalog or {})),objects=objects)


def rotation(item):
    x,y,z = np.radians(item["rot"])
    a,b,c = np.cos([x,y,z])
    d,e,f = np.sin([x,y,z])
    return np.array([[b*c, c*e*d-f*a, c*e*a+f*d],
                     [b*f, f*e*d+c*a, f*e*a-c*d], [-e,b*d,b*a]])


def index(scene, optic):
    return scene["materials"][optic["material"]]["indices"][optic["line"]]


def descendants(scene,root):
    found=set()
    pending=[root]
    by_parent={}
    for item in scene["objects"]:
        by_parent.setdefault(item.get("parent"),[]).append(item["id"])
    while pending:
        current=pending.pop()
        for key in by_parent.get(current,[]):
            if key==root or key in found:
                raise ValueError("Цикл в связях объектов")
            found.add(key)
            pending.append(key)
    return found


def move(scene,item,pos,rot):
    if item["locked"] or item.get("parent"):
        raise ValueError("Положение объекта зафиксировано")
    affected=descendants(scene,item["id"])
    old_pos=np.array(item["pos"])
    delta=rotation({"rot":rot}) @ rotation(item).T
    for child in scene["objects"]:
        if child["id"] in affected:
            child["pos"]=(np.asarray(pos)+delta@(np.asarray(child["pos"])-old_pos)).tolist()
            if rot!=item["rot"]:
                basis=delta@rotation(child)
                y=np.arcsin(np.clip(-basis[2,0],-1,1))
                if abs(np.cos(y))>1e-8:
                    x,z=np.arctan2(basis[2,1],basis[2,2]),np.arctan2(basis[1,0],basis[0,0])
                else:
                    x,z=0.,np.arctan2(-basis[0,1],basis[1,1])
                child["rot"]=np.degrees([x,y,z]).tolist()
    item.update(pos=list(pos),rot=list(rot))


def validate_material(m):
    if not isinstance(m["name"],str) or not m["name"].strip() or m["default"] not in m["indices"]:
        raise ValueError("Некорректная карточка материала")
    if m["density"] is not None and not 0 < m["density"] < 100:
        raise ValueError("Проверьте плотность материала")
    for wave, value in m["indices"].items():
        if (wave != "nominal" and not 0 < float(wave) < 100000) or not 1 < value <= 6:
            raise ValueError("Проверьте длину волны и показатель преломления")


def keys(scene):
    assets={item[k] for item in scene["objects"] for k in ("asset","profile","image") if k in item}
    if "turntable" in scene:
        assets.add(scene["turntable"]["asset"])
    return assets


def validate(scene):
    if scene["version"] not in (1,2):
        raise ValueError("Неизвестная версия проекта")
    for m in scene["materials"].values():
        validate_material(m)
    items = scene["objects"]
    if len({i["id"] for i in items}) != len(items):
        raise ValueError("Повторяющийся идентификатор объекта")
    ids={i["id"] for i in items}
    for i in items:
        if "parent" in i:
            if i["parent"] not in ids or i["locked"]:
                raise ValueError("Некорректная связь объектов")
            descendants(scene,i["id"])
        k = i["kind"]
        if k not in LABELS or not isinstance(i["name"], str) or not i["name"].strip():
            raise ValueError("Неизвестный объект")
        for field in ("rot","pos"):
            if len(i[field]) != 3 or not all(math.isfinite(x) for x in i[field]):
                raise ValueError("Некорректное положение объекта")
        for d in DIMS.get(k, {}):
            if not math.isfinite(i["dims"][d]) or i["dims"][d] <= 0:
                raise ValueError("Размер должен быть положительным")
        if k=="prism":
            points=np.array(i["outline"],dtype=float)
            if "circle" in i:
                if len(i["circle"])!=3 or not all(math.isfinite(v) for v in i["circle"]) or i["circle"][2]<=0:
                    raise ValueError("Некорректная окружность")
            elif points.ndim!=2 or points.shape[1]!=2 or len(points)<3 or not np.isfinite(points).all():
                raise ValueError("Проверьте координаты замкнутого эскиза")
        for op in i.get("operations",[]):
            if k not in ("box","sphere","cylinder","prism","cad"):
                raise ValueError("История построений доступна для объёмного CAD-тела")
            if op["kind"]=="section":
                if any(len(op[k])!=3 or not all(math.isfinite(v) for v in op[k]) for k in ("origin","normal")) or np.linalg.norm(op["normal"])<1e-9:
                    raise ValueError("Проверьте плоскость сечения")
            elif op["kind"] not in ("union","intersect","subtract") or op.get("tool") not in ids:
                raise ValueError("Отсутствует тело для булевой операции")
        if k in SOLIDS:
            index(scene,i)
        if k in ("cad","mesh") and "asset" not in i:
            raise ValueError("Файл модели отсутствует")
    if not all(isinstance(k,str) and ASSET.fullmatch(k) for k in keys(scene)):
        raise ValueError("Некорректное имя вложения")
    if "turntable" in scene:
        cfg=scene["turntable"]
        screens=[i for i in items if i["kind"]=="screen"]
        if len(screens)!=1 or screens[0]["id"]!=cfg["screen"] or "image" in screens[0]:
            raise ValueError("Серия видов 3D-цели закреплена за одним экраном")
        angles=cfg["angles"]
        if not 1<=len(angles)<=32 or any(not math.isfinite(a) for a in angles) or len(set(a%360 for a in angles))!=len(angles):
            raise ValueError("Нужны от 1 до 32 различных углов поворота")


def store_asset(path, folder):
    path = Path(path)
    with path.open("rb") as stream:
        digest = sha256()
        while block := stream.read(1024**2):
            digest.update(block)
    key = digest.hexdigest()+path.suffix.lower()
    if not ASSET.fullmatch(key):
        raise ValueError("Расширение файла не поддерживается")
    if not (folder/key).exists():
        shutil.copyfile(path,folder/key)
    return key


def save_scene(scene, folder, path):
    validate(scene)
    with tempfile.NamedTemporaryFile(dir=path.parent,delete=False) as tmp:
        staging = Path(tmp.name)
    try:
        with ZipFile(staging,"w",ZIP_DEFLATED,compresslevel=2) as z:
            z.writestr("scene.json",json.dumps(scene,ensure_ascii=False,allow_nan=False))
            for key in sorted(keys(scene)):
                z.write(folder/key,"assets/"+key)
            check_project_budget(z)
        os.replace(staging,path)
    finally:
        staging.unlink(missing_ok=True)


def check_project_budget(archive):
    if (sum(f.file_size for f in archive.infolist()) > PROJECT_LIMIT
            or archive.getinfo("scene.json").file_size > 8*1024**2):
        raise ValueError("Проект превышает бюджет загрузки")


def load_scene(path, folder):
    with ZipFile(path) as z:
        check_project_budget(z)
        scene = json.loads(z.read("scene.json"))
        validate(scene)
        for key in keys(scene):
            with z.open("assets/"+key) as source, tempfile.NamedTemporaryFile(dir=folder,delete=False) as out:
                staging = Path(out.name)
                try:
                    shutil.copyfileobj(source,out)
                    out.close()
                    with staging.open("rb") as verify:
                        import hashlib
                        if hashlib.file_digest(verify,"sha256").hexdigest()!=key.split(".")[0]:
                            raise ValueError("Повреждено вложение проекта")
                    os.replace(staging,folder/key)
                finally:
                    out.close()
                    staging.unlink(missing_ok=True)
    return scene


def consumed_tools(scene):
    return {op["tool"] for i in scene["objects"] for op in i.get("operations",[])
            if "tool" in op and not op.get("suppressed")}


def optical_setup(scene):
    consumed=consumed_tools(scene)
    solids = [i for i in scene["objects"] if i["kind"] in SOLIDS and not i.get("reference") and i["id"] not in consumed]
    lights = [i for i in scene["objects"] if i["kind"] in ("point","area","sun")]
    screens = [i for i in scene["objects"] if i["kind"]=="screen"]
    if len(solids)!=1 or solids[0]["kind"]!="plate":
        raise ValueError("Для оптического расчёта в сцене нужна одна оптическая пластина")
    if len(lights)!=1 or not screens:
        raise ValueError("Для расчёта нужны один источник и хотя бы один экран")
    return solids[0], lights[0], screens


def relative(plate, item):
    basis = rotation(plate).T
    return basis @ (np.array(item["pos"])-plate["pos"]), basis @ rotation(item)


def potential_relief_collisions(scene):
    plate,light,screens=optical_setup(scene)
    width=plate["dims"]["w"]
    height=plate["dims"]["h"]+plate.get("max_relief",math.inf if "profile" in plate else 0.)
    candidates=[]
    for face in [*screens,*([light] if light["kind"]=="area" else [])]:
        c,b=relative(plate,face)
        extent=(abs(b[:,0])*face["dims"]["w"]+abs(b[:,1])*face["dims"]["d"])/2
        if np.all(c+extent>=[-width/2,-width/2,0]) and np.all(c-extent<=[width/2,width/2,height]):
            candidates.append(face)
    return [plate,*candidates] if candidates else []


def rect_intersects(a,b):
    # Separating axes for two finite rectangles, incl. in-plane axes for coplanar cases.
    axes_a, axes_b = rotation(a).T, rotation(b).T
    delta = np.array(a["pos"])-b["pos"]
    for axis in [*axes_a,*axes_b,*(np.cross(u,v) for u in axes_a[:2] for v in axes_b[:2])]:
        length = np.linalg.norm(axis)
        if length < 1e-9:
            continue
        axis = axis/length
        def radius(body,axes):
            dim = body["dims"]
            return abs(axes[0]@axis)*dim["w"]/2+abs(axes[1]@axis)*dim["d"]/2
        ra,rb = radius(a,axes_a),radius(b,axes_b)
        # A shared edge is legal. Two coincident planes still need in-plane tests.
        if ra+rb > 1e-9 and abs(delta@axis) >= ra+rb-1e-8:
            return False
        if ra+rb <= 1e-9 and abs(delta@axis)>1e-8:
            return False
    return True


def diagnostics(scene):
    issues = []
    items = scene["objects"]
    screens = [i for i in items if i["kind"]=="screen"]
    for j,b in enumerate(screens):
        for a in screens[:j]:
            if rect_intersects(a,b):
                issues.append((True,[a["id"],b["id"]],f'{a["name"]} и {b["name"]} пересекаются. Сместите один из экранов.'))
    try:
        plate, source, screens = optical_setup(scene)
    except ValueError as exc:
        return issues+[(True,[],str(exc))]
    p, axes = relative(plate,source)
    if source["kind"]=="area":
        dims = source["dims"]
        extent = (abs(axes[2,0])*dims["w"]+abs(axes[2,1])*dims["d"])/2
        if np.dot(axes[:,2],-p)<=0:
            issues.append((False,[source["id"]],"Источник повёрнут от центра пластины. Проверьте его направление."))
    else:
        extent = 0
    if source["kind"]=="sun":
        if axes[2,2]<=0:
            issues.append((True,[source["id"]],"Направьте солнечный свет на входную грань пластины."))
    elif p[2]-extent>=0:
        issues.append((True,[source["id"]],"Источник должен освещать входную грань. Переместите его перед пластиной по локальной оси Z."))
    elif p[2]+extent>=0:
        issues.append((False,[source["id"]],"Часть площадного источника выходит за плоскость входной грани."))
    for s in screens:
        c, basis = relative(plate,s)
        # SAT against the minimum guaranteed solid slab; varying relief beyond it is not proven.
        slab = plate["dims"]["h"] + plate.get("min_relief",0.)
        test_plate = dict(plate, dims={"w":plate["dims"]["w"],"d":plate["dims"]["w"]})
        corners = [c+basis[:,0]*u*s["dims"]["w"]/2+basis[:,1]*v*s["dims"]["d"]/2
                   for u,v in [(-1,-1),(-1,1),(1,-1),(1,1)]]
        for level in np.clip([c[2],*(pnt[2] for pnt in corners),0.,slab],0.,slab):
            test_plate["pos"] = np.array(plate["pos"])+rotation(plate)[:,2]*level
            if rect_intersects(test_plate,s):
                issues.append((True,[s["id"],plate["id"]],f'{s["name"]} пересекает пластину. Сместите экран.'))
                break
        if np.linalg.norm(c)>0 and abs(c @ basis[:,2])/np.linalg.norm(c)<.06:
            issues.append((False,[s["id"]],f'{s["name"]} почти ребром к пластине. Поверните его к выходной грани.'))
        if source["kind"]=="area" and rect_intersects(source,s):
            issues.append((True,[source["id"],s["id"]],"Источник пересекает поверхность экрана."))
        if source["kind"]=="point":
            offset=rotation(s).T @ (np.array(source["pos"])-s["pos"])
            if abs(offset[2])<1e-7 and abs(offset[0])<=s["dims"]["w"]/2 and abs(offset[1])<=s["dims"]["d"]/2:
                issues.append((True,[source["id"],s["id"]],"Источник лежит на поверхности экрана."))
    return issues


def trace_settings(scene, resolution=256, rays=2000000):
    plate, lamp, screens = optical_setup(scene)
    c,b = relative(plate,lamp)
    values = [plate["dims"]["w"],plate["dims"]["h"],index(scene,plate),rays,{"point":0,"area":1,"sun":2}[lamp["kind"]],
              *c,*b[:,0],*b[:,1],lamp["dims"].get("w",0),lamp["dims"].get("d",0),len(screens)]
    for screen in screens:
        c,b = relative(plate,screen)
        w,h = screen["dims"]["w"],screen["dims"]["d"]
        values += [*c,*b[:,0],*(-b[:,1]),w,h,max(1,round(resolution*w/max(w,h))),max(1,round(resolution*h/max(w,h)))]
    return " ".join(str(v) for v in values)+"\n"


def design_mode(scene):
    for error, _, message in diagnostics(scene):
        if error:
            return "INVALID", message
    plate, lamp, screens = optical_setup(scene)
    if "turntable" in scene:
        return "SKETCH", "Эскизный подбор для серии углов"
    if any("image" not in screen for screen in screens):
        return "INVALID", "Назначьте изображение каждому экрану или выберите 3D-цель"
    if len(screens)!=1 or lamp["kind"]!="point":
        return "SKETCH", "Эскизный подбор для нескольких экранов или неточечного источника"
    screen = screens[0]
    source,_ = relative(plate,lamp)
    target,basis = relative(plate,screen)
    if target[2]<=plate["dims"]["h"] and np.allclose(basis,np.eye(3),atol=1e-7):
        return "INVALID", "Экран должен находиться за выходной гранью пластины"
    if max(abs(source[:2]).max(),abs(target[:2]).max())>1e-6 or not np.allclose(basis,np.eye(3),atol=1e-7):
        return "SKETCH", "Эскизный подбор для внеосевой компоновки"
    return "PHOTO", "Фотосинтез для одного экрана и точечного источника на оси"


def design_geometry(scene):
    plate, lamp, screens = optical_setup(scene)
    if len(screens)!=1 or lamp["kind"]!="point" or "image" not in screens[0]:
        raise ValueError("Для синтеза по фотографии нужны точечный источник и один экран с изображением")
    screen = screens[0]
    source,_ = relative(plate,lamp)
    target,basis = relative(plate,screen)
    if max(abs(source[:2]).max(),abs(target[:2]).max())>1e-6 or not np.allclose(basis,np.eye(3),atol=1e-7):
        raise ValueError("Для синтеза выровняйте источник и экран по оси Z пластины, а оси экрана параллельно пластине")
    if source[2]>=0 or target[2]<=plate["dims"]["h"]:
        raise ValueError("Проверьте расстояние от источника и экрана до пластины")
    return plate,screen,-source[2],target[2]
