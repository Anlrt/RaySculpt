#include "../src/desktop/topology.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <BRepGProp.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <iostream>
using namespace raysculpt;
static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        auto s = newDocument();
        validateDocument(s);
        auto a = s["objects"].toArray();
        auto base = a[0].toObject(), screen = a[2].toObject();
        require(std::abs(pose(base).Value(1, 3) - 1) < 1e-10, "horizontal axis");
        auto box = newBody("box"), second = newBody("box");
        put(s, box);
        put(s, second);
        auto exact = build_body(box, dir.path().toStdWString());
        auto top = pickReference(box, dir.path(), exact, TopAbs_FACE, gp_Pnt(0, 0, 24));
        auto support = localFrame(s, top, dir.path());
        require(std::abs(support.Value(3, 4) - 24) < 1e-8, "top face");
        auto bottom = pickReference(second, dir.path(), exact, TopAbs_FACE, gp_Pnt(0, 0, 0));
        second["parent"] = box["id"];
        second["constraint"] = QJsonObject{{"base", top}, {"own", bottom}, {"distance", 3}, {"angle", 90}};
        put(s, second);
        rebuildRelations(s, dir.path());
        auto checkMate = [&](double height) {
            auto joined = pose(object(s, second["id"].toString())) * localFrame(s, bottom, dir.path());
            require(std::abs(joined.Value(3, 4) - (height + 3)) < 1e-7 && std::abs(joined.Value(3, 3)) < 1e-7,
                    "associative mate");
        };
        checkMate(24);
        auto dims = box["dims"].toObject();
        dims["h"] = 32;
        box["dims"] = dims;
        put(s, box);
        rebuildRelations(s, dir.path());
        checkMate(32);
        auto op = newBody("sphere");
        op["dims"] = QJsonObject{{"r", 5}};
        put(s, op);
        box["operations"] = QJsonArray{QJsonObject{{"id", "cut"}, {"kind", "subtract"}, {"tool", op["id"]}}};
        put(s, box);
        auto result = build_body(resolved(s, box), dir.path().toStdWString());
        GProp_GProps g;
        BRepGProp::VolumeProperties(result, g);
        require(BRepCheck_Analyzer(result).IsValid() && g.Mass() < 40 * 32 * 32, "boolean cut");
        auto prism = newBody("prism");
        prism["arcs"] = QJsonObject{{"0", -4}};
        GProp_GProps prismProps;
        BRepGProp::VolumeProperties(build_body(prism, dir.path().toStdWString()), prismProps);
        require(prismProps.Mass() > 40 * 30 * 15, "curved extrusion");
        auto source = dir.filePath("triangle.obj");
        QFile f(source);
        require(f.open(QIODevice::WriteOnly), "fixture");
        f.write("v 0 0 0\nv 3 0 0\nv 0 3 0\nf 1 2 3\n");
        f.close();
        auto m = newBody("mesh");
        m["asset"] = storeAsset(source, dir.path());
        auto scene = newDocument();
        put(scene, m);
        bool open = false;
        for (auto v : inspectScene(scene, dir.path()))
            open |= v.toObject()["message"].toString().contains("открытые кромки: 3");
        require(open, "mesh boundary");
        scene["version"] = 1;
        auto path = dir.filePath("roundtrip.ccad");
        saveProject(scene, dir.path(), path);
        QTemporaryDir read;
        require(loadProject(path, read.path()) == scene, "v1 round trip");
        auto invalid = scene;
        auto bad = m;
        bad["parent"] = "missing";
        put(invalid, bad);
        bool rejected = false;
        try {
            validateDocument(invalid);
        } catch (const std::exception &) {
            rejected = true;
        }
        require(rejected, "invalid attachment");
        return 0;
    } catch (const Standard_Failure &e) {
        std::cerr << e.GetMessageString();
    } catch (const std::exception &e) {
        std::cerr << e.what();
    }
    return 1;
}
