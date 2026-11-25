#include "view.hpp"
#include <WNT_Window.hxx>
#include <Aspect_Grid.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <map>
#include <OpenGl_GraphicDriver.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <AIS_Triangulation.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <SelectMgr_ViewerSelector.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS.hxx>
#include <Quantity_Color.hxx>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QMessageBox>
#include <QTimer>
#include <cmath>
namespace raysculpt {
View::View(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}
void View::initialize() {
    if (!camera.IsNull())
        return;
    occ::handle<Aspect_DisplayConnection> display = new Aspect_DisplayConnection;
    occ::handle<OpenGl_GraphicDriver> driver = new OpenGl_GraphicDriver(display);
    viewer = new V3d_Viewer(driver);
    viewer->SetDefaultLights();
    viewer->SetLightOn();
    context = new AIS_InteractiveContext(viewer);
    camera = viewer->CreateView();
    occ::handle<WNT_Window> window = new WNT_Window(reinterpret_cast<Aspect_Handle>(winId()));
    camera->SetWindow(window);
    camera->SetBackgroundColor(Quantity_Color(.91, .94, .96, Quantity_TOC_sRGB));
    camera->SetProj(V3d_XposYnegZpos);
    camera->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_STEELBLUE, .08, V3d_ZBUFFER);
    viewer->SetRectangularGridValues(0, 0, 20, 20, 0);
    viewer->Grid(Aspect_GT_Rectangular)
        ->SetColors(Quantity_Color(.82, .86, .89, Quantity_TOC_sRGB),
                    Quantity_Color(.71, .77, .81, Quantity_TOC_sRGB));
    setGrid(grid);
    manipulator = new AIS_Manipulator();
    manipulator->SetPart(AIS_MM_Scaling, false);
    manipulator->SetModeActivationOnDetection(true);
    camera->MustBeResized();
}
View::~View() {
    if (!context.IsNull())
        context->RemoveAll(false);
    entries.clear();
    if (!camera.IsNull())
        camera->Remove();
}
void View::paintEvent(QPaintEvent *) {
    if (!camera.IsNull())
        camera->Redraw();
}
void View::resizeEvent(QResizeEvent *) {
    if (!camera.IsNull())
        camera->MustBeResized();
}
void View::setGrid(bool enabled) {
    grid = enabled;
    if (viewer.IsNull())
        return;
    if (grid)
        viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
    else
        viewer->DeactivateGrid();
    camera->Redraw();
}
void View::setMove(bool enabled) {
    moveMode = enabled;
    redrawManipulator();
}
void View::setRelief(bool enabled) {
    relief = enabled;
    sync(current, folder);
}
void View::direction(V3d_TypeOfOrientation o) {
    if (camera.IsNull())
        return;
    camera->SetProj(o);
    camera->Redraw();
}
void View::fit(bool only) {
    if (camera.IsNull())
        return;
    if (only && entries.contains(selected)) {
        Bnd_Box box;
        entries[selected].item->BoundingBox(box);
        if (!box.IsVoid())
            camera->FitAll(box, .15, false);
    } else
        camera->FitAll(.08, false);
    camera->ZFitAll();
    camera->Redraw();
}
void View::redrawManipulator() {
    if (manipulator.IsNull())
        return;
    manipulator->Detach();
    auto o = object(current, selected);
    if (moveMode && entries.contains(selected) && !o["locked"].toBool() && !o.contains("parent")) {
        auto item = entries[selected].item;
        manipulator->Attach(item);
        manipulator->SetPosition(gp_Ax2(gp_Pnt(item->LocalTransformation().TranslationPart()), gp::DZ()));
    }
    camera->Redraw();
}
void View::pickType(TopAbs_ShapeEnum t) {
    selection = t;
    if (context.IsNull())
        return;
    for (auto &e : entries) {
        context->Deactivate(e.item);
        context->Activate(
            e.item, AIS_Shape::SelectionMode(e.item->Shape().ShapeType() == TopAbs_FACE &&
                                                     !BRep_Tool::IsGeometric(TopoDS::Face(e.item->Shape()))
                                                 ? TopAbs_SHAPE
                                                 : t));
    }
}
void View::select(const QString &id) {
    selected = id;
    if (context.IsNull())
        return;
    context->ClearSelected(false);
    if (entries.contains(id))
        context->SetSelected(entries[id].item, false);
    redrawManipulator();
}
void View::sync(const QJsonObject &scene, const QString &assets) {
    initialize();
    current = scene;
    folder = assets;
    auto used = consumed(scene);
    QSet<QString> kept;
    for (auto v : scene["objects"].toArray()) {
        auto o = v.toObject();
        auto id = o["id"].toString(), k = o["kind"].toString();
        if (used.contains(id) || o["hidden"].toBool())
            continue;
        kept.insert(id);
        auto data = resolved(scene, o);
        QJsonObject key;
        for (auto field :
             {"kind", "asset", "profile", "dims", "cut", "operations", "circle", "arcs", "outline"})
            if (data.contains(field))
                key[field] = data[field];
        auto hash = QJsonDocument(key).toJson(QJsonDocument::Compact);
        auto &e = entries[id];
        if (e.item.IsNull() || e.geometry != hash) {
            auto shape = displayShape(data, folder);
            e.volume = 0;
            if (k == "plate" && o.contains("profile")) {
                auto h = profile(assetFile(folder, o["profile"].toString())).plate;
                int n = h.samples_x;
                std::vector<double> weight(n);
                double d = 1. / (n - 1);
                // Integrals of the cardinal basis with linearly extrapolated end coefficients.
                for (int i = 0; i < n - 1; ++i) {
                    double a = d / 24., b = 11 * d / 24.;
                    weight[i] += b;
                    weight[i + 1] += b;
                    if (i > 0)
                        weight[i - 1] += a;
                    else {
                        weight[i] += 2 * a;
                        weight[i + 1] -= a;
                    }
                    if (i + 2 < n)
                        weight[i + 2] += a;
                    else {
                        weight[i + 1] += 2 * a;
                        weight[i] -= a;
                    }
                }
                double height = h.thickness_mm;
                for (int j = 0; j < n; ++j)
                    for (int i = 0; i < n; ++i)
                        height += weight[i] * weight[j] * h.offsets_mm[size_t(j) * n + i];
                e.volume = height * h.width_mm * h.height_mm;
            } else if (k == "mesh") {
                auto m = shapeMesh(shape);
                std::map<std::array<double, 3>, int> vertices;
                std::vector<int> ids;
                for (auto p : m.vertices)
                    ids.push_back(
                        vertices.emplace(std::array{p.x, p.y, p.z}, int(vertices.size())).first->second);
                std::map<std::pair<int, int>, std::pair<int, int>> edges;
                double sum = 0;
                auto origin = m.vertices.front();
                for (auto t : m.triangles) {
                    sum += caustic::dot(m.vertices[t.a] - origin,
                                        caustic::cross(m.vertices[t.b] - origin, m.vertices[t.c] - origin));
                    for (auto [a, b] : {std::pair{ids[t.a], ids[t.b]}, std::pair{ids[t.b], ids[t.c]},
                                        std::pair{ids[t.c], ids[t.a]}}) {
                        auto &e = edges[std::minmax(a, b)];
                        ++e.first;
                        e.second += a < b ? 1 : -1;
                    }
                }
                if (std::all_of(edges.begin(), edges.end(),
                                [](auto &e) { return e.second.first == 2 && e.second.second == 0; }))
                    e.volume = std::abs(sum) / 6;
            } else if (solid(k)) {
                GProp_GProps props;
                BRepGProp::VolumeProperties(shape, props);
                e.volume = std::abs(props.Mass());
            }
            if (!e.item.IsNull())
                context->Remove(e.item, false);
            if (!e.relief.IsNull()) {
                context->Remove(e.relief, false);
                e.relief.Nullify();
            }
            e.geometry = hash;
            e.item = new AIS_Shape(shape);
            e.issue = false;
            e.overlayVisible = false;
            e.item->SetColor(k == "screen"                               ? Quantity_NOC_STEELBLUE
                             : k == "area" || k == "sun" || k == "point" ? Quantity_NOC_GOLD
                                                                         : Quantity_NOC_LIGHTSTEELBLUE);
            if (k == "screen" || k == "area")
                e.item->SetTransparency(.68);
            context->Display(e.item, AIS_Shaded, AIS_Shape::SelectionMode(selection), false);
            if (o.contains("profile") && shape.ShapeType() == TopAbs_FACE) {
                TopLoc_Location loc;
                auto tri = BRep_Tool::Triangulation(TopoDS::Face(shape), loc);
                if (!tri.IsNull()) {
                    auto colors = new NCollection_HArray1<int>(1, tri->NbNodes());
                    double base = o["dims"].toObject()["h"].toDouble(),
                           span = std::max({1e-9, std::abs(o["min_relief"].toDouble()),
                                            std::abs(o["max_relief"].toDouble())});
                    for (int i = 1; i <= tri->NbNodes(); ++i) {
                        double z = tri->Node(i).Z();
                        double t = z > 0 ? std::clamp((z - base) / span, -1., 1.) : 0.;
                        QColor c = QColor::fromRgbF(.88 - .65 * std::max(0., -t), .9 - .55 * std::abs(t),
                                                    .91 - .7 * std::max(0., t));
                        colors->SetValue(i, int(0xff000000u | (c.blue() << 16) | (c.green() << 8) | c.red()));
                    }
                    occ::handle<AIS_Triangulation> overlay = new AIS_Triangulation(tri);
                    overlay->SetColors(colors);
                    e.relief = overlay;
                }
            }
        }
        if (issueObjects.contains(id) != e.issue) {
            e.issue = issueObjects.contains(id);
            const auto color = e.issue         ? Quantity_NOC_RED
                               : k == "screen" ? Quantity_NOC_STEELBLUE
                               : QStringList{"point", "sun", "area"}.contains(k)
                                   ? Quantity_NOC_GOLD
                                   : Quantity_NOC_LIGHTSTEELBLUE;
            context->SetColor(e.item, color, false);
        }
        context->SetLocation(e.item, TopLoc_Location(pose(o)));
        if (!e.relief.IsNull()) {
            const bool showOverlay = relief && !e.issue;
            if (showOverlay)
                context->SetLocation(e.relief, TopLoc_Location(pose(o)));
            if (showOverlay == e.overlayVisible)
                continue;
            e.overlayVisible = showOverlay;
            if (showOverlay) {
                context->Display(e.relief, 0, -1, false);
                e.item->SetTransparency(1.);
                context->Redisplay(e.item, false);
            } else {
                context->Erase(e.relief, false);
                e.item->UnsetTransparency();
                context->Redisplay(e.item, false);
            }
        }
    }
    for (auto it = entries.begin(); it != entries.end();)
        if (!kept.contains(it.key())) {
            context->Remove(it->item, false);
            if (!it->relief.IsNull())
                context->Remove(it->relief, false);
            it = entries.erase(it);
        } else
            ++it;
    redrawManipulator();
}
void View::mousePressEvent(QMouseEvent *ev) {
    if (camera.IsNull())
        return;
    auto p = (ev->position() * devicePixelRatioF()).toPoint();
    press = last = p;
    if (ev->button() == Qt::RightButton)
        camera->StartRotation(p.x(), p.y());
    if (ev->button() != Qt::LeftButton)
        return;
    context->MoveTo(p.x(), p.y(), camera, false);
    if (moveMode && manipulator->HasActiveMode()) {
        manipulator->StartTransform(p.x(), p.y(), camera);
        dragging = true;
        return;
    }
    if (!context->HasDetected())
        return;
    auto hit = context->DetectedInteractive();
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        if (it->item == hit) {
            if (picked) {
                auto obj = object(current, it.key());
                auto point = context->MainSelector()->PickedPoint(1).Transformed(pose(obj).Inverted());
                picked(it.key(), it->item->Shape(), point, selection);
                if ((moveMode || ev->modifiers().testFlag(Qt::ShiftModifier)) && !obj["locked"].toBool() &&
                    !obj.contains("parent")) {
                    planeDrag = it.key();
                    dragStart = pose(obj);
                }
            }
            break;
        }
}
void View::mouseMoveEvent(QMouseEvent *ev) {
    if (camera.IsNull())
        return;
    auto p = (ev->position() * devicePixelRatioF()).toPoint();
    if (!planeDrag.isEmpty()) {
        double x0, y0, z0, x1, y1, z1;
        camera->Convert(press.x(), press.y(), x0, y0, z0);
        camera->Convert(p.x(), p.y(), x1, y1, z1);
        gp_Trsf tr = dragStart;
        tr.SetTranslationPart(dragStart.TranslationPart() + gp_XYZ(x1 - x0, y1 - y0, z1 - z0));
        context->SetLocation(entries[planeDrag].item, TopLoc_Location(tr));
        if (!entries[planeDrag].relief.IsNull())
            context->SetLocation(entries[planeDrag].relief, TopLoc_Location(tr));
        camera->Redraw();
    } else if (dragging) {
        manipulator->Transform(p.x(), p.y(), camera);
        camera->Redraw();
    } else if (ev->buttons() & Qt::RightButton)
        camera->Rotation(p.x(), p.y());
    else if (ev->buttons() & Qt::MiddleButton)
        camera->Pan(p.x() - last.x(), last.y() - p.y());
    else if (!ev->buttons())
        context->MoveTo(p.x(), p.y(), camera, true);
    last = p;
}
void View::mouseReleaseEvent(QMouseEvent *) {
    if (!planeDrag.isEmpty()) {
        auto id = planeDrag;
        planeDrag.clear();
        auto tr = entries[id].item->LocalTransformation();
        if (moved)
            moved(id, tr);
    } else if (dragging) {
        dragging = false;
        auto tr = entries[selected].item->LocalTransformation();
        manipulator->StopTransform(false);
        if (moved)
            moved(selected, tr);
    }
}
void View::wheelEvent(QWheelEvent *ev) {
    if (!camera.IsNull()) {
        camera->SetZoom(std::pow(1.18, ev->angleDelta().y() / 120.));
        camera->Redraw();
    }
}
QString View::gpuInfo() const {
    if (camera.IsNull())
        return {};
    NCollection_IndexedDataMap<TCollection_AsciiString, TCollection_AsciiString> info;
    camera->DiagnosticInformation(info, Graphic3d_DiagnosticInfo_Basic);
    QStringList lines;
    for (int i = 1; i <= info.Extent(); ++i)
        lines << QString::fromUtf8(info.FindKey(i).ToCString()) + " " + info.FindFromIndex(i).ToCString();
    return lines.join(" · ");
}
} // namespace raysculpt
