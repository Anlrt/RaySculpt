#include "geometry.hpp"
#include <BRepCheck_Analyzer.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <TopExp_Explorer.hxx>
#include <Bnd_Box.hxx>
#include <map>
#include <array>

namespace raysculpt {
static bool cutsScreen(const std::array<gp_Pnt, 3> &points, double w, double h) {
    std::vector<gp_Pnt> polygon(points.begin(), points.end());
    // Clip the triangle to the four sides before testing its signed distance to the screen.
    for (int axis : {1, 2})
        for (int sign : {-1, 1}) {
            std::vector<gp_Pnt> next;
            if (polygon.empty())
                return false;
            auto a = polygon.back();
            auto side = [&](gp_Pnt p) { return sign * p.Coord(axis) - (axis == 1 ? w : h) * .5; };
            for (auto b : polygon) {
                double sa = side(a), sb = side(b);
                if ((sa > 0) != (sb > 0))
                    next.push_back(a.Translated(gp_Vec(a, b) * (sa / (sa - sb))));
                if (sb <= 1e-9)
                    next.push_back(b);
                a = b;
            }
            polygon = std::move(next);
        }
    if (polygon.empty())
        return false;
    double low = 1e100, high = -1e100;
    for (auto p : polygon) {
        low = std::min(low, p.Z());
        high = std::max(high, p.Z());
    }
    return low <= 1e-7 && high >= -1e-7;
}
QJsonArray inspectScene(const QJsonObject &scene, const QString &assets, bool opticsOnly) {
    QJsonArray issues;
    struct Body {
        QJsonObject object;
        TopoDS_Shape shape;
        Bnd_Box bounds;
    };
    QList<Body> bodies;
    auto used = consumed(scene);
    auto issue = [&](QString message, QJsonObject a, QJsonObject b = {}) {
        QJsonArray ids{a["id"]};
        if (!b.isEmpty())
            ids.append(b["id"]);
        issues.append(QJsonObject{{"message", message}, {"objects", ids}});
    };
    QList<QJsonObject> screens;
    for (auto v : scene["objects"].toArray()) {
        auto o = v.toObject();
        if (o["kind"] == "screen" || o["kind"] == "area")
            screens << o;
    }
    for (auto v : scene["objects"].toArray()) {
        auto o = v.toObject();
        auto k = o["kind"].toString(), name = o["name"].toString();
        if (used.contains(o["id"].toString()) || (!solid(k) && k != "screen" && k != "area"))
            continue;
        if (opticsOnly && solid(k) && (k != "plate" || o["reference"].toBool() || !o.contains("profile")))
            continue;
        if (k == "mesh") {
            auto mesh = meshFile(assetFile(assets, o["asset"].toString()));
            std::map<std::array<double, 3>, int> vertices;
            std::vector<int> remap;
            for (auto p : mesh.vertices) {
                auto it = vertices.emplace(std::array{p.x, p.y, p.z}, int(vertices.size())).first;
                remap.push_back(it->second);
            }
            std::map<std::pair<int, int>, std::pair<int, int>> edges;
            int degenerate = 0;
            for (auto t : mesh.triangles) {
                if (caustic::length_squared(caustic::cross(mesh.vertices[t.b] - mesh.vertices[t.a],
                                                           mesh.vertices[t.c] - mesh.vertices[t.a])) < 1e-24)
                    ++degenerate;
                for (auto pair : {std::pair{t.a, t.b}, std::pair{t.b, t.c}, std::pair{t.c, t.a}}) {
                    int a = remap[pair.first], b = remap[pair.second];
                    auto &e = edges[std::minmax(a, b)];
                    ++e.first;
                    e.second += a < b ? 1 : -1;
                }
            }
            int open = 0, nonmanifold = 0, reversed = 0;
            for (auto &[edge, stat] : edges) {
                open += stat.first == 1;
                nonmanifold += stat.first > 2;
                reversed += stat.first == 2 && stat.second != 0;
            }
            for (auto [msg, count] : {std::pair{"открытые кромки", open},
                                      {"немногообразные рёбра", nonmanifold},
                                      {"ориентация граней", reversed},
                                      {"вырожденные грани", degenerate}})
                if (count)
                    issue(QString("%1 · %2: %3").arg(name, msg).arg(count), o);
            for (auto screen : screens) {
                auto tr = pose(screen).Inverted() * pose(o);
                auto d = screen["dims"].toObject();
                std::vector<gp_Pnt> pts;
                for (auto p : mesh.vertices)
                    pts.push_back(gp_Pnt(p.x, p.y, p.z).Transformed(tr));
                for (auto t : mesh.triangles)
                    if (cutsScreen({pts[t.a], pts[t.b], pts[t.c]}, d["w"].toDouble(), d["d"].toDouble())) {
                        issue(name + " пересекает " + screen["name"].toString(), o, screen);
                        break;
                    }
            }
            continue;
        }
        if (opticsOnly && k == "plate") {
            bool near = false;
            auto d = o["dims"].toObject();
            double half = d["w"].toDouble() / 2;
            for (auto screen : screens) {
                auto tr = pose(o).Inverted() * pose(screen);
                auto dims = screen["dims"].toObject();
                auto w = dims["w"].toDouble() / 2, h = dims["d"].toDouble() / 2;
                Bnd_Box bounds;
                for (int x : {-1, 1})
                    for (int y : {-1, 1})
                        bounds.Add(gp_Pnt(x * w, y * h, 0).Transformed(tr));
                Bnd_Box slab;
                slab.Add(gp_Pnt(-half, -half, 0));
                slab.Add(
                    gp_Pnt(half, half,
                           d["h"].toDouble() + o["max_relief"].toDouble(o.contains("profile") ? 1e8 : 0)));
                near |= !bounds.IsOut(slab);
            }
            if (!near)
                continue;
        }
        auto shape =
            solid(k) ? build_body(resolved(scene, o), assets.toStdWString()) : displayShape(o, assets);
        if (!opticsOnly && !BRepCheck_Analyzer(shape).IsValid())
            issue(name + " · некорректное тело", o);
        if (!opticsOnly && solid(k) && !TopExp_Explorer(shape, TopAbs_SOLID).More())
            issue(name + " · незамкнутая оболочка", o);
        Body body{o, shape.Moved(TopLoc_Location(pose(o))), {}};
        BRepBndLib::Add(body.shape, body.bounds);
        for (auto &other : bodies) {
            if (body.bounds.IsOut(other.bounds) || (!solid(k) && !solid(other.object["kind"].toString())))
                continue;
            BRepAlgoAPI_Common common(body.shape, other.shape);
            if (!common.IsDone())
                fail("Не удалось проверить пересечение тел");
            if (TopExp_Explorer(common.Shape(), TopAbs_FACE).More())
                issue(name + " пересекает " + other.object["name"].toString(), o, other.object);
        }
        bodies << body;
    }
    return issues;
}
} // namespace raysculpt
