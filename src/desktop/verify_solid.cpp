#include "verify.hpp"
#include "geometry.hpp"
#include "height_surface.hpp"
#include "photo_trace.hpp"
#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Geom_BSplineSurface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <fstream>
#include <random>
#include <numbers>
namespace raysculpt {
static occ::handle<Geom_BSplineSurface> opticalFace(const TopoDS_Shape &solid) {
    int solids = 0, faces = 0;
    for (TopExp_Explorer it(solid, TopAbs_SOLID); it.More(); it.Next())
        ++solids;
    occ::handle<Geom_BSplineSurface> optical;
    for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
        ++faces;
        BRepAdaptor_Surface surface(TopoDS::Face(it.Current()));
        if (surface.GetType() == GeomAbs_BSplineSurface) {
            if (!optical.IsNull())
                fail("Несколько оптических граней");
            optical = surface.BSpline();
        }
    }
    if (solids != 1 || faces != 6 || optical.IsNull() || !BRepCheck_Analyzer(solid).IsValid())
        fail("Ожидается одно корректное тело с шестью гранями");
    return optical;
}
QJsonObject verifySolid(const QStringList &args) {
    if (args.size() < 2)
        fail("Укажите профиль и CAD-файл");
    QString target, out;
    int rays = 36000000;
    for (int i = 2; i < args.size(); i += 2) {
        if (i + 1 >= args.size())
            fail("Отсутствует значение");
        if (args[i] == "--target")
            target = args[i + 1];
        else if (args[i] == "--out")
            out = args[i + 1];
        else if (args[i] == "--rays")
            rays = args[i + 1].toInt();
        else
            fail("Неизвестный параметр");
    }
    if (target.isEmpty() != out.isEmpty())
        fail("Для трассировки нужны --target и --out");
    QString previous;
    auto h = designProfile(args[0], previous);
    int n = h.samples_x;
    auto o = newBody("cad");
    QFileInfo cad(args[1]);
    o["asset"] = cad.fileName();
    auto surface = opticalFace(build_body(o, cad.absolutePath().toStdWString())),
         expected = opticalFace(splineSolid(h));
    if (surface->UDegree() != 3 || surface->VDegree() != 3 || surface->NbUPoles() != n + 2 ||
        surface->NbVPoles() != n + 2 || surface->NbUKnots() != n || surface->NbVKnots() != n ||
        surface->IsURational() || surface->IsVRational() || surface->IsUPeriodic() || surface->IsVPeriodic())
        fail("Структура сплайна изменилась");
    for (int i = 1; i <= n; ++i)
        if (std::abs(surface->UKnot(i) - expected->UKnot(i)) > 1e-8 ||
            std::abs(surface->VKnot(i) - expected->VKnot(i)) > 1e-8 ||
            surface->UMultiplicity(i) != expected->UMultiplicity(i) ||
            surface->VMultiplicity(i) != expected->VMultiplicity(i))
            fail("Узлы сплайна отличаются");
    double pole = 0, height = 0, angle = 0;
    for (int i = 1; i <= n + 2; ++i)
        for (int j = 1; j <= n + 2; ++j) {
            auto a = surface->Pole(i, j), b = expected->Pole(i, j);
            for (int k = 1; k <= 3; ++k)
                pole = std::max(pole, std::abs(a.Coord(k) - b.Coord(k)));
        }
    auto sample = [&](double x, double y) {
        gp_Pnt p;
        gp_Vec u, v;
        surface->D1(x, y, p, u, v);
        auto truth = caustic::sample_spline(h, x, y);
        height = std::max(height, std::abs(p.Z() - h.thickness_mm - truth.height));
        angle = std::max(angle, (u ^ v).Angle(gp_Vec(-truth.dx, -truth.dy, 1)) * 180 / std::numbers::pi);
    };
    std::mt19937_64 rng(81621);
    double half = h.width_mm / 2;
    std::uniform_real_distribution<double> spread(-half, half);
    for (int i = 0; i < 8192; ++i)
        sample(spread(rng), spread(rng));
    for (int i = 0; i < n; ++i) {
        double t = h.width_mm * i / (n - 1) - half;
        for (double edge : {-half, half}) {
            sample(t, edge);
            sample(edge, t);
        }
    }
    if (pole > 1e-7 || height > 1e-7 || angle > 1e-5)
        fail("Поверхность CAD отличается от расчётной");
    QJsonObject report{{"solids", 1},
                       {"faces", 6},
                       {"optical_faces", 1},
                       {"poles", (n + 2) * (n + 2)},
                       {"samples", 8192 + 4 * n},
                       {"max_pole_component_error_mm", pole},
                       {"max_height_error_mm", height},
                       {"max_normal_error_deg", angle}};
    if (!out.isEmpty()) {
        if (h.interpolation != caustic::HeightInterpolation::CubicLinearEnds)
            fail("Для повтора нужен фотографический профиль");
        QFileInfo info(args[0]);
        QString original = info.isDir()                        ? QDir(args[0]).filePath("surface.txt")
                           : info.suffix().toLower() == "json" ? info.dir().filePath("surface.txt")
                                                               : args[0];
        QTemporaryDir dir;
        QFile input(original);
        if (!input.open(QIODevice::ReadOnly))
            fail(input.errorString());
        auto file = dir.filePath("surface.txt");
        std::ofstream f(std::filesystem::path(file.toStdWString()));
        f << input.readLine().toStdString() << std::setprecision(17);
        auto restore = [&](int i, auto get) {
            return i == 0       ? (3 * get(2) - get(3)) / 2
                   : i == n - 1 ? (3 * get(n + 1) - get(n)) / 2
                                : get(i + 2);
        };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                double z = restore(
                    j, [&](int v) { return restore(i, [&](int u) { return surface->Pole(u, v).Z(); }); });
                f << z - h.thickness_mm << (i == n - 1 ? '\n' : ' ');
            }
        f.close();
        caustic::trace_photo(file.toStdWString(), target.toStdWString(), out.toStdWString(), rays, 0,
                             0x70ca4e31);
        QFile json(QDir(out).filePath("cad_verification.json"));
        auto bytes = QJsonDocument(report).toJson();
        if (!json.open(QIODevice::WriteOnly) || json.write(bytes) != bytes.size())
            fail(json.errorString());
    }
    return report;
}
} // namespace raysculpt
