#include "topology.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <ShapeAnalysis_Surface.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <gp_Quaternion.hxx>
#include <functional>
#include <numbers>
#include <optional>
namespace raysculpt {
static TopAbs_ShapeEnum typeOf(const QString &kind) {
    if (kind == "face")
        return TopAbs_FACE;
    if (kind == "edge")
        return TopAbs_EDGE;
    if (kind == "vertex")
        return TopAbs_VERTEX;
    fail("Неизвестный тип опоры");
    return TopAbs_SHAPE;
}
static auto subshapes(const TopoDS_Shape &shape, TopAbs_ShapeEnum type) {
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> table;
    TopExp::MapShapes(shape, type, table);
    return table;
}
static QJsonArray fingerprint(const TopoDS_Shape &sub, int count) {
    QJsonArray result{count, int(sub.Orientation())};
    if (sub.ShapeType() == TopAbs_FACE) {
        BRepAdaptor_Surface f(TopoDS::Face(sub));
        result.append(int(f.GetType()));
        if (f.GetType() == GeomAbs_Plane) {
            auto n = f.Plane().Axis().Direction();
            for (auto x : {n.X(), n.Y(), n.Z()})
                result.append(std::round(x * 1e8) / 1e8);
        }
    }
    if (sub.ShapeType() == TopAbs_EDGE && !BRep_Tool::Degenerated(TopoDS::Edge(sub)))
        result.append(int(BRepAdaptor_Curve(TopoDS::Edge(sub)).GetType()));
    return result;
}
static gp_Pnt vertex(const caustic::Mesh &mesh, int i) {
    if (i < 0 || size_t(i) >= mesh.vertices.size())
        fail("Утрачена вершина исходной сетки");
    auto v = mesh.vertices[i];
    return {v.x, v.y, v.z};
}
static std::array<int, 3> triangle(const caustic::Mesh &mesh, int i) {
    if (i < 0 || size_t(i) >= mesh.triangles.size())
        fail("Утрачена грань исходной сетки");
    auto t = mesh.triangles[i];
    return {int(t.a), int(t.b), int(t.c)};
}
static double segment(const gp_Pnt &p, const gp_Pnt &a, const gp_Pnt &b) {
    gp_Vec ab(a, b);
    auto t = std::clamp(gp_Vec(a, p).Dot(ab) / std::max(ab.SquareMagnitude(), 1e-30), 0., 1.);
    return p.SquareDistance(a.Translated(ab * t));
}
static std::pair<double, int> nearestSide(const gp_Pnt &p, const std::array<gp_Pnt, 3> &nodes) {
    std::pair<double, int> best{1e100, 0};
    for (int i = 0; i < 3; ++i)
        best = std::min(best, std::pair{segment(p, nodes[i], nodes[(i + 1) % 3]), i});
    return best;
}
static double distanceToTriangle(const gp_Pnt &p, const std::array<gp_Pnt, 3> &v) {
    auto u = gp_Vec(v[0], v[1]), w = gp_Vec(v[0], v[2]), t = gp_Vec(v[0], p), n = u ^ w;
    auto den = n.SquareMagnitude();
    if (den > 1e-24) {
        auto a = ((t ^ w) * n) / den, b = ((u ^ t) * n) / den;
        if (a >= 0 && b >= 0 && a + b <= 1)
            return std::pow(t * n, 2) / den;
    }
    return nearestSide(p, v).first;
}
QJsonObject pickReference(const QJsonObject &body, const QString &assets, const TopoDS_Shape &shape,
                          TopAbs_ShapeEnum type, const gp_Pnt &p) {
    QString kind = type == TopAbs_FACE     ? "face"
                   : type == TopAbs_EDGE   ? "edge"
                   : type == TopAbs_VERTEX ? "vertex"
                                           : "center";
    QJsonObject ref{{"object", body["id"]}, {"kind", kind}, {"index", 0}};
    if (kind == "center")
        return ref;
    if (body["kind"] == "mesh") {
        auto mesh = meshFile(assetFile(assets, body["asset"].toString()));
        double best = 1e100;
        int chosen = -1;
        for (int i = 0; i < int(mesh.triangles.size()); ++i) {
            auto t = triangle(mesh, i);
            std::array pts{vertex(mesh, t[0]), vertex(mesh, t[1]), vertex(mesh, t[2])};
            auto d = distanceToTriangle(p, pts);
            if (d < best) {
                chosen = i;
                best = d;
            }
        }
        auto t = triangle(mesh, chosen);
        std::array pts{vertex(mesh, t[0]), vertex(mesh, t[1]), vertex(mesh, t[2])};
        ref["index"] = chosen;
        if (kind == "edge") {
            auto i = nearestSide(p, pts).second;
            ref["nodes"] = QJsonArray{t[i], t[(i + 1) % 3]};
        }
        if (kind == "vertex") {
            int i = 0;
            for (int j = 1; j < 3; ++j)
                if (p.SquareDistance(pts[j]) < p.SquareDistance(pts[i]))
                    i = j;
            ref["nodes"] = QJsonArray{t[i]};
        }
        return ref;
    }
    auto bodyShape = body.contains("profile") ? build_body(body, assets.toStdWString()) : shape;
    auto table = subshapes(bodyShape, type);
    auto target = BRepBuilderAPI_MakeVertex(p).Vertex();
    int chosen = 0;
    double best = 1e100;
    for (int i = 1; i <= table.Extent(); ++i) {
        BRepExtrema_DistShapeShape d(target, table(i));
        if (d.IsDone() && d.NbSolution() > 0 && d.Value() < best) {
            best = d.Value();
            chosen = i;
        }
    }
    if (!chosen)
        fail("Выбранный элемент недоступен");
    ref["index"] = chosen - 1;
    auto sub = table(chosen);
    ref["signature"] = fingerprint(sub, table.Extent());
    if (type == TopAbs_FACE) {
        TopLoc_Location loc;
        auto surf = BRep_Tool::Surface(TopoDS::Face(sub), loc);
        ShapeAnalysis_Surface projection(surf);
        auto uv = projection.ValueOfUV(p.Transformed(loc.Transformation().Inverted()), 1e-7);
        ref["uv"] = QJsonArray{uv.X(), uv.Y()};
    }
    return ref;
}
TopoDS_Shape element(const QJsonObject &s, const QJsonObject &ref, const QString &assets) {
    auto o = object(s, ref["object"].toString());
    if (o.isEmpty())
        fail("Опорное тело удалено");
    auto k = ref["kind"].toString();
    if (k == "center" || k == "vertex_origin") {
        double h =
            k == "center" && QStringList{"plate", "box", "cylinder", "prism"}.contains(o["kind"].toString())
                ? o["dims"].toObject()["h"].toDouble() / 2
                : 0;
        return BRepBuilderAPI_MakeVertex(gp_Pnt(0, 0, h)).Vertex();
    }
    int i = ref["index"].toInt(-1);
    if (o["kind"] == "mesh") {
        auto m = meshFile(assetFile(assets, o["asset"].toString()));
        auto t = triangle(m, i);
        if (k == "face") {
            BRepBuilderAPI_MakePolygon wire;
            for (auto j : t)
                wire.Add(vertex(m, j));
            wire.Close();
            return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
        }
        auto n = ref["nodes"].toArray();
        if (n.size() != (k == "edge" ? 2 : 1))
            fail("Некорректная привязка к сетке");
        if (k == "edge")
            return BRepBuilderAPI_MakeEdge(vertex(m, n.at(0).toInt(-1)), vertex(m, n.at(1).toInt(-1))).Edge();
        return BRepBuilderAPI_MakeVertex(vertex(m, n.at(0).toInt(-1))).Vertex();
    }
    auto body = solid(o["kind"].toString()) ? build_body(resolved(s, o), assets.toStdWString())
                                            : displayShape(o, assets);
    auto table = subshapes(body, typeOf(k));
    if (i < 0 || i >= table.Extent())
        fail("Опорный элемент изменился");
    auto sub = table(i + 1);
    if (ref.contains("signature") && ref["signature"].toArray() != fingerprint(sub, table.Extent()))
        fail("Опорный элемент изменился. Выберите его заново.");
    return sub;
}
gp_Trsf localFrame(const QJsonObject &scene, const QJsonObject &ref, const QString &assets) {
    auto sub = element(scene, ref, assets);
    gp_Pnt origin;
    gp_Vec z(0, 0, 1), x(1, 0, 0);
    if (sub.ShapeType() == TopAbs_VERTEX)
        origin = BRep_Tool::Pnt(TopoDS::Vertex(sub));
    else if (sub.ShapeType() == TopAbs_EDGE) {
        BRepAdaptor_Curve c(TopoDS::Edge(sub));
        c.D1((c.FirstParameter() + c.LastParameter()) / 2, origin, x);
        x.Normalize();
        if (std::abs(z * x) > .9999)
            z = gp_Vec(0, 1, 0);
        z -= x * (x * z);
    } else {
        BRepAdaptor_Surface f(TopoDS::Face(sub));
        if (f.GetType() == GeomAbs_Plane) {
            GProp_GProps props;
            BRepGProp::SurfaceProperties(sub, props);
            origin = props.CentreOfMass();
            z = f.Plane().Axis().Direction();
        } else {
            auto uv = ref["uv"].toArray();
            if (uv.size() != 2)
                fail("Нужна точка на криволинейной грани");
            gp_Vec du, dv;
            f.D1(uv[0].toDouble(), uv[1].toDouble(), origin, du, dv);
            z = du ^ dv;
        }
        if (sub.Orientation() == TopAbs_REVERSED)
            z.Reverse();
        z.Normalize();
        if (std::abs(x * z) > .999999)
            x = std::abs(z.Y()) <= std::abs(z.Z()) ? gp_Vec(0, 1, 0) : gp_Vec(0, 0, 1);
    }
    gp_Trsf tr;
    tr.SetDisplacement(gp_Ax3(), gp_Ax3(origin, gp_Dir(z), gp_Dir(x)));
    return tr;
}
void rebuildRelations(QJsonObject &s, const QString &assets) {
    QSet<QString> done, active;
    std::function<void(QString)> visit = [&](QString id) {
        if (done.contains(id))
            return;
        if (active.contains(id))
            fail("Цикл операций или привязок");
        active.insert(id);
        auto o = object(s, id);
        if (o.isEmpty())
            fail("Утеряно зависимое тело");
        for (auto v : o["operations"].toArray()) {
            auto op = v.toObject();
            if (op.contains("tool") && !op["suppressed"].toBool())
                visit(op["tool"].toString());
        }
        auto parent = o["parent"].toString();
        if (!parent.isEmpty())
            visit(parent);
        o = object(s, id);
        if (o.contains("constraint")) {
            auto mate = o["constraint"].toObject(), base = mate["base"].toObject(),
                 own = mate["own"].toObject();
            if (parent != base["object"].toString() || id != own["object"].toString() || o["locked"].toBool())
                fail("Неверная связь объектов");
            auto anchor = pose(object(s, parent)) * localFrame(s, base, assets);
            gp_Trsf twist, offset;
            twist.SetRotation(gp_Ax1(gp::Origin(), gp::DX()),
                              mate["angle"].toDouble() * std::numbers::pi / 180);
            offset.SetTranslation(gp_Vec(0, 0, mate["distance"].toDouble()));
            auto dest = anchor * offset * twist * localFrame(s, own, assets).Inverted();
            auto delta = dest * pose(o).Inverted();
            if (delta.TranslationPart().Modulus() > 1e-9 ||
                std::abs(delta.GetRotation().GetRotationAngle()) > 1e-9) {
                o.remove("parent");
                put(s, o);
                moveBody(s, id, dest);
                o = object(s, id);
                o["parent"] = parent;
                put(s, o);
            }
        }
        active.remove(id);
        done.insert(id);
    };
    for (auto v : s["objects"].toArray())
        visit(v.toObject()["id"].toString());
}
} // namespace raysculpt
