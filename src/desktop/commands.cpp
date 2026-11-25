#include "window.hpp"
#include "jobs.hpp"
#include "topology.hpp"
#include "sketch_view.hpp"
#include <QtWidgets>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepGProp.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <GProp_GProps.hxx>
#include <numbers>

namespace raysculpt {
static QString choose(QWidget *window, QString title, const QList<QJsonObject> &items) {
    if (items.isEmpty())
        fail("Подходящих объектов нет");
    QStringList names;
    for (auto o : items)
        names << QString("%1 · %2").arg(o["name"].toString(), o["id"].toString().left(7));
    bool accepted = false;
    auto result = QInputDialog::getItem(window, title, "Объект", names, 0, false, &accepted);
    return accepted ? items[names.indexOf(result)]["id"].toString() : QString();
}
static std::optional<QList<double>> parameters(QWidget *parent, QString title, QStringList labels,
                                               QList<double> values) {
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    QFormLayout form(&dlg);
    QList<QDoubleSpinBox *> fields;
    for (int i = 0; i < labels.size(); ++i) {
        auto spin = new QDoubleSpinBox(&dlg);
        spin->setRange(-1e6, 1e6);
        spin->setDecimals(5);
        spin->setValue(values[i]);
        fields << spin;
        form.addRow(labels[i], spin);
    }
    QDialogButtonBox b(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&b);
    QObject::connect(&b, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(&b, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (!dlg.exec())
        return {};
    QList<double> v;
    for (auto spin : fields)
        v << spin->value();
    return v;
}
static QJsonObject workable(const QJsonObject &scene, const QString &id) {
    auto o = object(scene, id);
    if (!QStringList{"cad", "box", "sphere", "cylinder", "prism"}.contains(o["kind"].toString()))
        fail("Выберите объёмное справочное тело");
    return o;
}
void Window::section() {
    auto o = workable(scene, selected);
    auto vals = parameters(this, "Сечение в координатах тела",
                           {"X, мм", "Y, мм", "Z, мм", "Нормаль X", "Нормаль Y", "Нормаль Z"},
                           {0, 0, o["dims"].toObject()["h"].toDouble() / 2, 0, 0, 1});
    if (!vals)
        return;
    auto v = *vals;
    if (std::hypot(v[3], v[4], v[5]) < 1e-9)
        fail("Нормаль должна иметь направление");
    auto ops = o["operations"].toArray();
    ops.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::Id128)},
                           {"kind", "section"},
                           {"origin", vec(v[0], v[1], v[2])},
                           {"normal", vec(v[3], v[4], v[5])}});
    o["operations"] = ops;
    auto s = scene;
    put(s, o);
    commit(s);
}
void Window::booleanOp() {
    auto o = workable(scene, selected);
    QList<QJsonObject> options;
    for (auto v : scene["objects"].toArray()) {
        auto item = v.toObject();
        if (item["id"] != selected &&
            QStringList{"box", "cad", "cylinder", "sphere", "prism"}.contains(item["kind"].toString()) &&
            !consumed(scene).contains(item["id"].toString()))
            options << item;
    }
    auto tool = choose(this, "Инструмент булевой операции", options);
    if (tool.isEmpty())
        return;
    bool ok;
    QStringList labels{"Вычесть", "Объединить", "Пересечь"}, kinds{"subtract", "union", "intersect"};
    auto type = QInputDialog::getItem(this, "Действие", "Операция", labels, 0, false, &ok);
    if (!ok)
        return;
    auto ops = o["operations"].toArray();
    ops.append(QJsonObject{{"kind", kinds[labels.indexOf(type)]},
                           {"id", QUuid::createUuid().toString(QUuid::Id128)},
                           {"tool", tool}});
    o["operations"] = ops;
    auto s = scene;
    put(s, o);
    commit(s);
}
void Window::history() {
    auto o = object(scene, selected);
    auto ops = o["operations"].toArray();
    if (ops.isEmpty())
        fail("В выбранном теле нет операций");
    QStringList list;
    for (auto v : ops) {
        auto op = v.toObject();
        list << QString("%1. %2 %3")
                    .arg(list.size() + 1)
                    .arg(op["kind"].toString(), op["suppressed"].toBool() ? "(выключена)" : "");
    }
    bool ok;
    auto row = tree->currentItem();
    int current =
        row && row->data(0, Qt::UserRole + 1) == "operation" ? row->data(0, Qt::UserRole + 2).toInt() : 0;
    auto chosen = QInputDialog::getItem(this, "История тела", "Операция", list, current, false, &ok);
    if (!ok)
        return;
    int index = list.indexOf(chosen);
    auto op = ops[index].toObject();
    QStringList actions{"Включить / выключить", "Изменить", "Удалить"};
    auto mode = QInputDialog::getItem(this, "История тела", "Действие", actions, 0, false, &ok);
    if (!ok)
        return;
    if (mode == actions[2])
        ops.removeAt(index);
    else {
        if (mode == actions[0])
            op["suppressed"] = !op["suppressed"].toBool();
        else if (op["kind"] == "section") {
            auto p = op["origin"].toArray(), n = op["normal"].toArray();
            auto v = parameters(this, "Сечение", {"X", "Y", "Z", "Nx", "Ny", "Nz"},
                                {p[0].toDouble(), p[1].toDouble(), p[2].toDouble(), n[0].toDouble(),
                                 n[1].toDouble(), n[2].toDouble()});
            if (!v)
                return;
            op["origin"] = vec((*v)[0], (*v)[1], (*v)[2]);
            op["normal"] = vec((*v)[3], (*v)[4], (*v)[5]);
        } else {
            QList<QJsonObject> items;
            for (auto v : scene["objects"].toArray()) {
                auto item = v.toObject();
                if (item["id"] != selected && solid(item["kind"].toString()) && item["kind"] != "mesh")
                    items << item;
            }
            auto id = choose(this, "Тело-инструмент", items);
            if (id.isEmpty())
                return;
            op["tool"] = id;
        }
        ops[index] = op;
    }
    o["operations"] = ops;
    auto s = scene;
    put(s, o);
    commit(s);
}
void Window::exportBody() {
    auto o = object(scene, selected);
    auto k = o["kind"].toString();
    if (!solid(k))
        fail("Выберите тело");
    auto path = QFileDialog::getSaveFileName(
        this, "Экспорт тела", o["name"].toString(),
        k == "mesh" ? "STL (*.stl);;OBJ (*.obj);;3MF (*.3mf)"
                    : "STEP (*.step);;BREP (*.brep);;STL (*.stl);;OBJ (*.obj);;3MF (*.3mf)");
    if (path.isEmpty())
        return;
    int cells = 0;
    auto ext = QFileInfo(path).suffix().toLower();
    if (o.contains("profile") && QStringList{"stl", "obj", "3mf"}.contains(ext)) {
        auto h = profile(assetFile(assets.path(), o["profile"].toString())).plate;
        bool ok;
        cells = QInputDialog::getInt(this, "Точность экспорта", "Ячеек вдоль стороны",
                                     std::min(2048, std::max(512, 2 * (int(h.samples_x) - 1))),
                                     int(h.samples_x) - 1, 2048, 1, &ok);
        if (!ok)
            return;
    }
    launch({{"task", "export"}, {"id", selected}, {"destination", path}, {"cells", cells}});
}
void Window::mate() {
    auto o = object(scene, selected);
    if (o.isEmpty())
        return;
    auto s = scene;
    if (o.contains("parent")) {
        if (QMessageBox::question(this, "Сопряжение", "Снять связь с опорой?") != QMessageBox::Yes)
            return;
        o.remove("parent");
        o.remove("constraint");
    } else {
        if (o["locked"].toBool())
            fail("Снимите фиксацию");
        QList<QJsonObject> items;
        for (auto v : scene["objects"].toArray()) {
            auto p = v.toObject(), ancestor = p;
            QSet<QString> seen;
            while (!ancestor.isEmpty() && ancestor["id"] != selected &&
                   !seen.contains(ancestor["id"].toString())) {
                seen.insert(ancestor["id"].toString());
                ancestor = object(scene, ancestor["parent"].toString());
            }
            if (ancestor.isEmpty())
                items << p;
        }
        auto id = choose(this, "Зафиксировать относительно", items);
        if (id.isEmpty())
            return;
        o["parent"] = id;
    }
    put(s, o);
    commit(s);
}
void Window::mateElements() {
    auto target = object(scene, selected);
    auto prior = target["constraint"].toObject();
    auto base = anchorReference, own = selectedReference;
    if (own["object"] != selected || base.isEmpty() || own["object"] == base["object"]) {
        base = prior["base"].toObject();
        own = prior["own"].toObject();
    }
    if (base.isEmpty() || own.isEmpty() || own["object"] != selected || target["locked"].toBool())
        fail("Выберите опору, затем элемент другого подвижного тела");
    auto vals =
        parameters(this, "Сопряжение", {"Смещение вдоль нормали, мм", "Поворот вокруг оси X опоры, °"},
                   {prior["distance"].toDouble(), prior["angle"].toDouble()});
    if (!vals)
        return;
    target["parent"] = base["object"];
    target["constraint"] =
        QJsonObject{{"base", base}, {"own", own}, {"distance", (*vals)[0]}, {"angle", (*vals)[1]}};
    auto s = scene;
    put(s, target);
    commit(s);
}
void Window::measure() {
    if (feature.IsNull() || previousFeature.IsNull())
        fail("Выберите два элемента в модели. Тип элемента задаётся в меню «Вид».");
    previousFeature = element(scene, anchorReference, assets.path())
                          .Moved(TopLoc_Location(pose(object(scene, anchorReference["object"].toString()))));
    feature = element(scene, selectedReference, assets.path())
                  .Moved(TopLoc_Location(pose(object(scene, selectedReference["object"].toString()))));
    BRepExtrema_DistShapeShape dist(previousFeature, feature);
    if (!dist.IsDone() || !dist.NbSolution())
        fail("Не удалось определить расстояние");
    QString text = QString("Расстояние %1 мм").arg(dist.Value(), 0, 'g', 8);
    auto axis = [](const TopoDS_Shape &shape) -> std::optional<gp_Dir> {
        if (shape.ShapeType() == TopAbs_FACE) {
            BRepAdaptor_Surface f(TopoDS::Face(shape));
            if (f.GetType() == GeomAbs_Plane)
                return f.Plane().Axis().Direction();
        }
        if (shape.ShapeType() == TopAbs_EDGE) {
            BRepAdaptor_Curve e(TopoDS::Edge(shape));
            if (e.GetType() == GeomAbs_Line)
                return e.Line().Direction();
        }
        return {};
    };
    auto a = axis(feature), b = axis(previousFeature);
    if (a && b) {
        double angle = std::acos(std::clamp(std::abs(a->Dot(*b)), 0., 1.)) * 180 / std::numbers::pi;
        if (feature.ShapeType() != previousFeature.ShapeType())
            angle = 90 - angle;
        text += QString(" · угол %1°").arg(angle, 0, 'g', 7);
    }
    log->appendPlainText(text);
    QMessageBox::information(this, "Измерение", text);
}
void Window::inspect() { launch({{"task", "inspect"}}); }
void Window::placement() {
    QList<QJsonObject> screens;
    QJsonObject lamp, plate;
    for (auto v : scene["objects"].toArray()) {
        auto o = v.toObject();
        auto k = o["kind"].toString();
        if (k == "screen")
            screens << o;
        if (k == "plate" && !o["reference"].toBool())
            plate = o;
        if (k == "area" || k == "sun" || k == "point")
            lamp = o;
    }
    auto id = screens.size() == 1 ? screens[0]["id"].toString() : choose(this, "Экран для подбора", screens);
    if (id.isEmpty())
        return;
    auto recv = object(scene, id);
    if (plate.isEmpty() || lamp.isEmpty())
        fail("Добавьте пластину и источник");
    bool ok;
    QStringList modes{"Солнце", "Лампа"};
    auto mode = QInputDialog::getItem(this, "Установка", "Освещение", modes, lamp["kind"] == "sun" ? 0 : 1,
                                      false, &ok);
    if (!ok)
        return;
    auto v = parameters(this, "Диапазон расстояний",
                        {"Минимальный зазор, мм", "Максимальный зазор, мм", "Расстояние до лампы, мм",
                         "Размер источника, мм"},
                        {30, 180, 350, 5});
    if (!v)
        return;
    auto gap0 = (*v)[0], gap1 = (*v)[1], distance = (*v)[2], size = (*v)[3];
    if (gap0 <= 0 || gap1 < gap0 || distance <= 0 || size <= 0)
        fail("Проверьте диапазон расстояний");
    launch({{"task", "placement"},
            {"screen", id},
            {"mode", mode == modes[0] ? "sun" : "area"},
            {"size", size},
            {"min_gap", gap0},
            {"max_gap", gap1},
            {"max_lamp", distance}});
}
void Window::sketch() {
    auto current = object(scene, selected);
    auto o = current["kind"] == "prism" ? current : newBody("prism");
    QDialog dialog(this);
    dialog.setWindowTitle("Эскиз и выдавливание");
    QVBoxLayout layout(&dialog);
    QComboBox kind;
    kind.addItems({"Контур из отрезков и дуг", "Окружность"});
    kind.setCurrentIndex(o.contains("circle") ? 1 : 0);
    layout.addWidget(&kind);
    QTableWidget table(0, 3);
    table.setHorizontalHeaderLabels({"X, мм", "Y, мм", "Стрела дуги, мм"});
    layout.addWidget(&table);
    auto row = [&](double x, double y, double rise) {
        int i = table.rowCount();
        table.insertRow(i);
        int j = 0;
        for (double value : {x, y, rise})
            table.setItem(i, j++, new QTableWidgetItem(QString::number(value, 'g', 12)));
    };
    auto pts = o["outline"].toArray();
    auto arcs = o["arcs"].toObject();
    for (int i = 0; i < pts.size(); ++i) {
        auto pt = pts[i].toArray();
        row(pt[0].toDouble(), pt[1].toDouble(), arcs[QString::number(i)].toDouble());
    }
    SketchView canvas(table);
    layout.insertWidget(1, &canvas);
    canvas.fit();
    QHBoxLayout tools;
    layout.addLayout(&tools);
    QPushButton plus("+ Вершина"), minus("Удалить"), dimension("Размер отрезка…");
    for (auto btn : {&plus, &minus, &dimension})
        tools.addWidget(btn);
    connect(&plus, &QPushButton::clicked, &dialog, [&] { row(0, 0, 0); });
    connect(&minus, &QPushButton::clicked, &dialog, [&] {
        if (table.currentRow() >= 0)
            table.removeRow(table.currentRow());
    });
    connect(&dimension, &QPushButton::clicked, &dialog, [&] {
        int i = table.currentRow();
        if (i < 0 || table.rowCount() < 2)
            return;
        int j = (i + 1) % table.rowCount();
        auto x = table.item(i, 0)->text().toDouble(), y = table.item(i, 1)->text().toDouble(),
             dx = table.item(j, 0)->text().toDouble() - x, dy = table.item(j, 1)->text().toDouble() - y;
        auto d = parameters(&dialog, "Хорда", {"Длина, мм", "Угол, °"},
                            {std::hypot(dx, dy), std::atan2(dy, dx) * 180 / std::numbers::pi});
        if (!d || (*d)[0] <= 0)
            return;
        auto a = (*d)[1] * std::numbers::pi / 180;
        table.item(j, 0)->setText(QString::number(x + (*d)[0] * std::cos(a), 'g', 12));
        table.item(j, 1)->setText(QString::number(y + (*d)[0] * std::sin(a), 'g', 12));
    });
    QFormLayout form;
    layout.addLayout(&form);
    QDoubleSpinBox cx, cy, radius, height;
    auto circle = o["circle"].toArray();
    for (auto f : {&cx, &cy, &radius, &height}) {
        f->setRange(-1e6, 1e6);
        f->setDecimals(4);
    }
    radius.setMinimum(.001);
    height.setMinimum(.001);
    cx.setValue(circle.at(0).toDouble());
    cy.setValue(circle.at(1).toDouble());
    radius.setValue(circle.at(2).toDouble(10));
    height.setValue(o["dims"].toObject()["h"].toDouble());
    form.addRow("Центр X", &cx);
    form.addRow("Центр Y", &cy);
    form.addRow("Радиус, мм", &radius);
    form.addRow("Выдавливание, мм", &height);
    QDoubleSpinBox snap;
    snap.setRange(.001, 1000);
    snap.setDecimals(3);
    snap.setValue(1);
    form.addRow("Шаг сетки, мм", &snap);
    auto update = [&] {
        canvas.circle = kind.currentIndex() == 1;
        canvas.center = {cx.value(), cy.value()};
        canvas.radius = radius.value();
        canvas.snap = snap.value();
        canvas.update();
    };
    update();
    for (auto field : {&cx, &cy, &radius, &snap})
        connect(field, &QDoubleSpinBox::valueChanged, &dialog, update);
    connect(&kind, &QComboBox::currentIndexChanged, &dialog, update);
    QDialogButtonBox b(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout.addWidget(&b);
    connect(&b, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&b, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (!dialog.exec())
        return;
    QJsonArray outline;
    arcs = {};
    for (int i = 0; i < table.rowCount(); ++i) {
        double values[3];
        for (int j = 0; j < 3; ++j) {
            bool ok;
            values[j] = table.item(i, j)->text().toDouble(&ok);
            if (!ok || !std::isfinite(values[j]))
                fail("Некорректная координата");
        }
        outline.append(QJsonArray{values[0], values[1]});
        if (values[2])
            arcs[QString::number(i)] = values[2];
    }
    o["outline"] = outline;
    o["arcs"] = arcs;
    if (kind.currentIndex() == 1)
        o["circle"] = vec(cx.value(), cy.value(), radius.value());
    else
        o.remove("circle");
    o["dims"] = QJsonObject{{"h", height.value()}};
    if (current["kind"] != "prism") {
        QStringList planes{"XY", "XZ", "YZ"};
        bool supported = selectedReference["object"] == selected && selectedReference["kind"] == "face";
        if (supported)
            planes.prepend("Выбранная грань");
        bool ok;
        auto plane = QInputDialog::getItem(this, "Плоскость эскиза", "Опора", planes, 0, false, &ok);
        if (!ok)
            return;
        if (plane == "Выбранная грань") {
            o["parent"] = selected;
            o["constraint"] = QJsonObject{
                {"base", selectedReference},
                {"own", QJsonObject{{"object", o["id"]}, {"kind", "vertex_origin"}, {"index", 0}}},
                {"distance", 0},
                {"angle", 0}};
        } else
            o["rot"] = plane == "XZ" ? vec(90, 0, 0) : plane == "YZ" ? vec(90, 0, 90) : vec(0, 0, 0);
    }
    selected = o["id"].toString();
    auto s = scene;
    put(s, o);
    commit(s);
}
} // namespace raysculpt
