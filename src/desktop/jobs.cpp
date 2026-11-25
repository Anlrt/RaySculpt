#include "jobs.hpp"
#include "geometry.hpp"
#include "photo_trace.hpp"
#include "target_renderer.hpp"
#include <opencv2/imgproc.hpp>
#include <QImageReader>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QThread>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <numeric>
#include <set>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QSaveFile>
#include "height_surface.hpp"
#include "surface_mesh.hpp"
#include <random>
namespace raysculpt {
using Filter = int (*)(const double *, const double *, double *, size_t, size_t, double);
extern "C" {
int raysculpt_inverse_transport_v1(const double *, size_t, double, double, double, size_t, double *, double *,
                                   Filter);
int raysculpt_forward_transport_v1(const double *, const double *, size_t, size_t, size_t, double, double,
                                   double, size_t, double *, double *, size_t);
int raysculpt_optical_profile_v1(const double *, const double *, size_t, double, double, double, double,
                                 double, double *, Filter);
int raysculpt_calibrate_v1(const double *, const double *, const double *, const double *, size_t, size_t,
                           double, double, double *, double *, Filter);
int raysculpt_spectral_filter_v1(const double *, const double *, double *, size_t, size_t, double) noexcept;
}
struct Setup {
    QJsonObject plate, lamp;
    QList<QJsonObject> screens;
};
static Setup setup(const QJsonObject &s) {
    Setup a;
    auto used = consumed(s);
    for (auto v : s["objects"].toArray()) {
        auto o = v.toObject();
        auto k = o["kind"].toString();
        if (solid(k)) {
            if (o["reference"].toBool() || used.contains(o["id"].toString()))
                continue;
            if (k != "plate" || !a.plate.isEmpty())
                fail("Для расчёта нужна одна оптическая пластина");
            a.plate = o;
        } else if (k == "screen")
            a.screens << o;
        else {
            if (!a.lamp.isEmpty())
                fail("Выберите один источник");
            a.lamp = o;
        }
    }
    if (a.plate.isEmpty() || a.lamp.isEmpty() || a.screens.isEmpty())
        fail("Добавьте пластину, источник и экран");
    return a;
}
static double dim(const QJsonObject &o, const char *k) { return o["dims"].toObject()[k].toDouble(); }
static QString path(const QString &d, const QString &file) { return QDir(d).filePath(file); }
static void save(const QString &name, QByteArray bytes) {
    QFile f(name);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
        fail(f.errorString());
}
static QJsonObject json(const QString &name) {
    QFile f(name);
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    auto d = QJsonDocument::fromJson(f.readAll());
    if (!d.isObject())
        fail("Некорректный отчёт расчёта");
    return d.object();
}
QSize pixels(const QJsonObject &s, int res) {
    auto w = dim(s, "w"), h = dim(s, "d");
    if (!(w > 0 && h > 0) || res < 1 || res > 2048)
        fail("Некорректный размер экрана");
    return {std::max(1, int(std::nearbyint(res * w / std::max(w, h)))),
            std::max(1, int(std::nearbyint(res * h / std::max(w, h))))};
}
static bool rectOverlap(const QJsonObject &a, const QJsonObject &b) {
    auto x = pose(a), y = pose(b);
    gp_Vec diff(x.TranslationPart() - y.TranslationPart());
    auto axes = [](const gp_Trsf &t) {
        return std::array{gp_Vec(1, 0, 0).Transformed(t), gp_Vec(0, 1, 0).Transformed(t),
                          gp_Vec(0, 0, 1).Transformed(t)};
    };
    auto ax = axes(x), bx = axes(y);
    auto separated = [&](gp_Vec n) {
        if (n.SquareMagnitude() < 1e-16)
            return false;
        n.Normalize();
        double r = (std::abs(n * ax[0]) * dim(a, "w") + std::abs(n * ax[1]) * dim(a, "d") +
                    std::abs(n * bx[0]) * dim(b, "w") + std::abs(n * bx[1]) * dim(b, "d")) *
                   .5;
        return r > 1e-8 ? std::abs(n * diff) >= r - 1e-8 : std::abs(n * diff) > 1e-8;
    };
    for (auto n : ax)
        if (separated(n))
            return false;
    for (auto n : bx)
        if (separated(n))
            return false;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            if (separated(ax[i] ^ bx[j]))
                return false;
    return true;
}
QStringList diagnostics(const QJsonObject &scene) {
    QStringList messages;
    try {
        auto a = setup(scene);
        auto base = pose(a.plate).Inverted();
        auto l = base * pose(a.lamp);
        double extent =
            a.lamp["kind"] == "area"
                ? (std::abs(l.Value(3, 1)) * dim(a.lamp, "w") + std::abs(l.Value(3, 2)) * dim(a.lamp, "d")) /
                      2
                : 0;
        if (a.lamp["kind"] == "sun" ? l.Value(3, 3) <= 0 : l.Value(3, 4) - extent >= 0)
            messages << "Направьте источник на входную грань пластины";
        for (int i = 0; i < a.screens.size(); ++i) {
            auto s = a.screens[i];
            for (int j = 0; j < i; ++j)
                if (rectOverlap(s, a.screens[j]))
                    messages << s["name"].toString() + " пересекает " + a.screens[j]["name"].toString();
            auto t = base * pose(s);
            double z = t.Value(3, 4), h = dim(a.plate, "h") + a.plate["min_relief"].toDouble();
            auto slab = a.plate;
            slab["dims"] = QJsonObject{{"w", dim(slab, "w")}, {"d", dim(slab, "w")}};
            QList<double> levels{0., std::clamp(z, 0., std::max(0., h)), h};
            for (int x : {-1, 1})
                for (int y : {-1, 1})
                    levels << std::clamp(
                        gp_Pnt(x * dim(s, "w") / 2, y * dim(s, "d") / 2, 0).Transformed(t).Z(), 0.,
                        std::max(0., h));
            if (a.lamp["kind"] == "point") {
                auto p = (pose(s).Inverted() * pose(a.lamp)).TranslationPart();
                if (std::abs(p.Z()) < 1e-7 && std::abs(p.X()) <= dim(s, "w") / 2 &&
                    std::abs(p.Y()) <= dim(s, "d") / 2)
                    messages << "Точечный источник лежит на экране";
            }
            for (auto level : levels) {
                gp_Trsf shift;
                shift.SetTranslation(gp_Vec(0, 0, level));
                assignPose(slab, pose(a.plate) * shift);
                if (rectOverlap(s, slab)) {
                    messages << s["name"].toString() + " пересекает пластину";
                    break;
                }
            }
            if (a.lamp["kind"] == "area" && rectOverlap(s, a.lamp))
                messages << "Источник пересекает экран";
        }
    } catch (const std::exception &e) {
        messages << QString::fromUtf8(e.what());
    }
    return messages;
}
QByteArray traceSettings(const QJsonObject &scene, int resolution, int rays) {
    auto a = setup(scene);
    auto base = pose(a.plate).Inverted();
    QByteArray bytes;
    QTextStream out(&bytes);
    out.setRealNumberPrecision(17);
    auto column = [&](gp_Trsf t, int col, int sign = 1) {
        for (int i = 1; i <= 3; ++i)
            out << ' ' << sign * t.Value(i, col);
    };
    out << dim(a.plate, "w") << ' ' << dim(a.plate, "h") << ' ' << indexOf(scene, a.plate) << ' ' << rays
        << ' '
        << (a.lamp["kind"] == "sun"    ? 2
            : a.lamp["kind"] == "area" ? 1
                                       : 0);
    auto t = base * pose(a.lamp);
    column(t, 4);
    column(t, 1);
    column(t, 2);
    out << ' ' << dim(a.lamp, "w") << ' ' << dim(a.lamp, "d") << ' ' << a.screens.size();
    for (auto s : a.screens) {
        t = base * pose(s);
        auto size = pixels(s, resolution);
        column(t, 4);
        column(t, 1);
        column(t, 2, -1);
        out << ' ' << dim(s, "w") << ' ' << dim(s, "d") << ' ' << size.width() << ' ' << size.height();
    }
    out << '\n';
    out.flush();
    return bytes;
}
static double linear(double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); }
static unsigned char encoded(double x) {
    x = std::clamp(x, 0., 1.);
    return uchar(std::lround(255 * (x <= .0031308 ? 12.92 * x : 1.055 * std::pow(x, 1. / 2.4) - .055)));
}
static cv::Mat photo(const QString &file, QSize size, bool crop) {
    QImageReader reader(file);
    reader.setAutoTransform(true);
    auto image = reader.read().convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
        fail(reader.errorString());
    int w = image.width(), h = image.height(), cw = size.width(), ch = size.height();
    cv::Rect box(0, 0, w, h);
    if (crop) {
        if (1LL * w * ch > 1LL * h * cw) {
            box.width = std::max(1, qRound(double(h) * cw / ch));
            box.x = (w - box.width) / 2;
        } else {
            box.height = std::max(1, qRound(double(w) * ch / cw));
            box.y = (h - box.height) / 2;
        }
    }
    double lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = linear(i / 255.);
    cv::Mat gray(box.height, box.width, CV_64F);
    for (int y = 0; y < box.height; ++y) {
        auto src = image.constScanLine(box.y + y) + box.x * 4;
        auto dst = gray.ptr<double>(y);
        for (int x = 0; x < box.width; ++x, src += 4)
            dst[x] = (lut[src[0]] * .2126 + lut[src[1]] * .7152 + lut[src[2]] * .0722) * src[3] / 255.;
    }
    double scale = std::min(double(cw) / box.width, double(ch) / box.height);
    int dw = crop ? cw : std::max(1, qRound(box.width * scale)),
        dh = crop ? ch : std::max(1, qRound(box.height * scale));
    cv::Mat result = cv::Mat::zeros(ch, cw, CV_64F);
    cv::resize(gray, result(cv::Rect((cw - dw) / 2, (ch - dh) / 2, dw, dh)), {dw, dh}, 0, 0,
               scale < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
    return result;
}
static QImage preview(const cv::Mat &img) {
    QImage q(img.cols, img.rows, QImage::Format_Grayscale8);
    for (int y = 0; y < img.rows; ++y)
        for (int x = 0; x < img.cols; ++x)
            q.scanLine(y)[x] = encoded(img.at<double>(y, x));
    return q;
}
QImage goalPreview(const QString &file, const QJsonObject &s, int res) {
    return preview(photo(file, pixels(s, res), s["image_fit"] == "crop"));
}
static void pgm(const QString &name, const cv::Mat &img) {
    QByteArray b = "P5\n" + QByteArray::number(img.cols) + " " + QByteArray::number(img.rows) + "\n65535\n";
    for (int y = 0; y < img.rows; ++y)
        for (int x = 0; x < img.cols; ++x) {
            unsigned n = unsigned(std::lround(65535 * std::clamp(img.at<double>(y, x), 0., 1.)));
            b.append(char(n >> 8));
            b.append(char(n));
        }
    save(name, b);
}
static cv::Mat goal(const QString &assets, const QJsonObject &s, int res, double angle,
                    const caustic::Mesh *mesh) {
    auto size = pixels(s, res);
    if (!mesh)
        return photo(assetFile(assets, s["image"].toString()), size, s["image_fit"] == "crop");
    caustic::RenderSettings settings;
    settings.width = size.width();
    settings.height = size.height();
    settings.azimuth_degrees = angle;
    settings.elevation_degrees = 0;
    auto img = caustic::render_target(*mesh, settings);
    cv::Mat result;
    cv::Mat(img.height, img.width, CV_8U, img.pixels.data()).convertTo(result, CV_64F, 1. / 255);
    return result;
}
static void check(int status) {
    if (status)
        fail(QString("Оптический расчёт отклонил параметры (код %1)").arg(status));
}
static caustic::Mesh transportMesh(const caustic::HeightField &h, const std::vector<double> &mx,
                                   const std::vector<double> &my, int map, int n) {
    caustic::Mesh mesh;
    auto &pts = mesh.vertices;
    auto &faces = mesh.triangles;
    pts.reserve(size_t(n + 1) * (n + 1) + 4 * n + 1);
    faces.reserve(2 * n * n + 12 * n);
    std::mt19937 rng(381901);
    std::uniform_real_distribution<double> jitter(-.23, .23);
    auto at = [&](int x, int y) { return uint32_t(y * (n + 1) + x); };
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            double u = (i + (i && i < n ? jitter(rng) : 0)) / n, v = (j + (j && j < n ? jitter(rng) : 0)) / n;
            auto sample = [&](const auto &m) {
                auto a = std::min(int(u * map), map - 1), b = std::min(int(v * map), map - 1),
                     idx = b * (map + 1) + a;
                double du = u * map - a, dv = v * map - b;
                return std::lerp(std::lerp(m[idx], m[idx + 1], du),
                                 std::lerp(m[idx + map + 1], m[idx + map + 2], du), dv);
            };
            double x = .97 * sample(mx) + .03 * (u - .5) * h.width_mm,
                   y = .97 * sample(my) + .03 * (v - .5) * h.width_mm;
            pts.push_back({x, y, h.thickness_mm + caustic::sample_spline(h, x, y).height});
        }
    auto facet = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (caustic::cross(pts[b] - pts[a], pts[c] - pts[a]).z <= 0)
            fail("Складка сетки переноса");
        faces.push_back({a, b, c});
    };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            auto a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
            auto len = [&](auto u, auto v) { return std::hypot(pts[u].x - pts[v].x, pts[u].y - pts[v].y); };
            if (len(a, c) <= len(b, d)) {
                facet(a, b, c);
                facet(a, c, d);
            } else {
                facet(a, b, d);
                facet(b, c, d);
            }
        }
    std::vector<uint32_t> rim;
    for (int i = 0; i < n; ++i)
        rim.push_back(at(i, 0));
    for (int j = 0; j < n; ++j)
        rim.push_back(at(n, j));
    for (int i = n; i > 0; --i)
        rim.push_back(at(i, n));
    for (int j = n; j > 0; --j)
        rim.push_back(at(0, j));
    uint32_t bottom = uint32_t(pts.size());
    for (auto i : rim)
        pts.push_back({pts[i].x, pts[i].y, 0});
    auto center = uint32_t(pts.size());
    pts.push_back({0, 0, 0});
    for (uint32_t i = 0; i < rim.size(); ++i) {
        auto j = uint32_t((i + 1) % rim.size());
        faces.push_back({bottom + i, bottom + j, rim[j]});
        faces.push_back({bottom + i, rim[j], rim[i]});
        faces.push_back({center, bottom + j, bottom + i});
    }
    return mesh;
}
static void photoDesign(const QString &dir, const cv::Mat &target, const QJsonObject &s, const Setup &a,
                        double source, double screen, int quality, const QJsonObject &options = {}) {
    int grid = std::array{96, 320, 512}.at(quality), map = std::array{96, 320, 640}.at(quality),
        rays = std::array{1000000, 8000000, 36000000}.at(quality);
    double aperture = dim(a.plate, "w"), thick = dim(a.plate, "h"), n = indexOf(s, a.plate),
           w = dim(a.screens[0], "w"), h = dim(a.screens[0], "d");
    grid = options["grid"].toInt(grid);
    map = options["map-size"].toInt(map);
    rays = options["rays"].toInt(rays);
    int steps = options["steps"].toInt(100), passes = options["corrections"].toInt(quality),
        cells = options["cells"].toInt(0);
    double toe = options["shadow-floor"].toDouble(.08),
           limit = options["max-relief"].toDouble(.9 * std::min(thick, screen - thick));
    bool transport = options["mesh-layout"] == "transport";
    if (grid < 32 || grid > 1024 || grid % 2 || map < 32 || map > 1024 || map % 2 || steps < 1 ||
        steps > 500 || passes < 0 || passes > 5 || toe < 0 || toe > .5 || rays < 1000 || rays > 200000000 ||
        !(limit > 0 && limit < thick && limit < screen - thick) ||
        (cells && (cells < grid - 1 || cells > (transport ? 2048 : 2047))))
        fail("Недопустимые параметры оптического расчёта");
    QJsonArray errors, scores;
    int selectedPass = 0;
    QString stop = "iteration budget";
    double mean = cv::mean(target)[0];
    if (mean <= 0)
        fail("Цель имеет нулевую яркость");
    cv::Mat wanted;
    cv::sqrt(target.mul(target) + std::pow(toe * mean, 2), wanted);
    wanted *= mean / cv::mean(wanted)[0];
    cv::Mat drive = wanted.clone();
    QString mapFile = path(dir, "goal.pgm");
    pgm(mapFile, target);
    double best = 1e100;
    QString bestFile;
    for (int pass = 0; pass <= passes; ++pass) {
        cv::Mat flip, field;
        cv::flip(drive, flip, 0);
        cv::resize(flip, field, {map, map}, 0, 0, cv::INTER_AREA);
        std::vector<double> mx((map + 1) * (map + 1)), my(mx.size()), tx(grid * grid), ty(tx.size()),
            coeff(tx.size());
        check(raysculpt_inverse_transport_v1(field.ptr<double>(), map, w, h, aperture, steps, mx.data(),
                                             my.data(), raysculpt_spectral_filter_v1));
        check(raysculpt_forward_transport_v1(mx.data(), my.data(), mx.size(), map, grid, aperture, w, h,
                                             grid <= 96 ? 1 : std::clamp(QThread::idealThreadCount(), 1, 12),
                                             tx.data(), ty.data(), tx.size()));
        check(raysculpt_optical_profile_v1(tx.data(), ty.data(), grid, aperture, thick, n, source, screen,
                                           coeff.data(), raysculpt_spectral_filter_v1));
        auto file = path(dir, QString("surface-%1.txt").arg(pass));
        std::ofstream out(std::filesystem::path(file.toStdWString()));
        out << std::setprecision(17) << grid << ' ' << aperture << ' ' << thick << ' ' << n << ' ' << source
            << ' ' << screen << ' ' << w << ' ' << h << '\n';
        bool valid = true;
        for (size_t i = 0; i < coeff.size(); ++i) {
            valid &= std::isfinite(coeff[i]) && std::abs(coeff[i]) <= limit;
            out << coeff[i] << (i % grid == size_t(grid - 1) ? '\n' : ' ');
        }
        out.close();
        if (!valid) {
            if (pass) {
                stop = "relief limit";
                break;
            }
            fail("Рельеф превышает доступную толщину");
        }
        auto cal = path(dir, QString("cal-%1").arg(pass));
        caustic::trace_photo(file.toStdWString(), mapFile.toStdWString(), cal.toStdWString(),
                             std::min(rays, 24000000), 0, 103433);
        auto report = json(path(cal, "report.json"));
        auto guard = report["guard_px"].toArray();
        int gx = guard.at(0).toInt(), gy = guard.at(1).toInt();
        cv::Mat full(target.rows + 2 * gy, target.cols + 2 * gx, CV_32F), actual;
        QFile f(path(cal, "irradiance.bin"));
        if (!f.open(QIODevice::ReadOnly) ||
            f.read(reinterpret_cast<char *>(full.data), full.total() * 4) != qint64(full.total() * 4))
            fail("Ошибка поля яркости");
        full(cv::Rect(gx, gy, target.cols, target.rows)).convertTo(actual, CV_64F);
        cv::Mat next(target.size(), CV_64F);
        double score = 0;
        check(raysculpt_calibrate_v1(actual.ptr<double>(), target.ptr<double>(), wanted.ptr<double>(),
                                     drive.ptr<double>(), target.rows, target.cols,
                                     report["picture_relative_rmse"].toDouble(), toe, &score,
                                     next.ptr<double>(), raysculpt_spectral_filter_v1));
        scores.append(score);
        errors.append(report["picture_relative_rmse"]);
        if (score >= best) {
            stop = "calibration score stopped improving";
            break;
        }
        best = score;
        selectedPass = pass + 1;
        bestFile = file;
        drive = next;
    }
    auto output = path(dir, "result");
    caustic::trace_photo(bestFile.toStdWString(), mapFile.toStdWString(), output.toStdWString(), rays,
                         transport ? 0 : cells, 0x70ca4e31);
    auto report = json(path(output, "report.json"));
    if (transport && cells) {
        cv::Mat flip, field;
        cv::flip(wanted, flip, 0);
        cv::resize(flip, field, {map, map}, 0, 0, cv::INTER_AREA);
        std::vector<double> x((map + 1) * (map + 1)), y(x.size());
        check(raysculpt_inverse_transport_v1(field.ptr<double>(), map, w, h, aperture, steps, x.data(),
                                             y.data(), raysculpt_spectral_filter_v1));
        auto mesh = transportMesh(profile(bestFile).plate, x, y, map, cells);
        std::string err;
        if (!caustic::save_binary_stl(path(output, "02_plate.stl").toStdWString(), mesh, err))
            fail(QString::fromStdString(err));
        report["mesh_cells"] = cells;
        report["triangles"] = qint64(mesh.triangles.size());
        report["mesh_layout"] = "screen_transport";
    }
    QFile::remove(path(output, "irradiance.bin"));
    report["selected_pass"] = selectedPass;
    report["calibration_scores"] = scores;
    report["calibration_relative_rmse"] = errors;
    report["calibration_stop"] = stop;
    report["map_size"] = map;
    report["transport_max_steps"] = steps;
    report["shadow_floor"] = toe;
    save(path(output, "report.json"), QJsonDocument(report).toJson());
}
void designPhotograph(const QStringList &args) {
    if (args.isEmpty())
        fail("Укажите изображение");
    QJsonObject options;
    QSet<QString> strings{"out", "formats", "image-fit", "mesh-layout"};
    QSet<QString> numbers{"grid",        "map-size",     "rays",   "cells", "steps",  "resolution",
                          "corrections", "shadow-floor", "plate",  "width", "height", "thickness",
                          "max-relief",  "index",        "source", "screen"};
    for (int i = 1; i < args.size(); i += 2) {
        if (!args[i].startsWith("--") || i + 1 >= args.size())
            fail("Некорректный параметр");
        auto k = args[i].mid(2), value = args[i + 1];
        if (strings.contains(k))
            options[k] = value;
        else if (numbers.contains(k)) {
            bool ok;
            double v = value.toDouble(&ok);
            if (!ok || !std::isfinite(v))
                fail("Некорректное число");
            options[k] = v;
        } else
            fail("Неизвестный параметр " + k);
    }
    QString out = options["out"].toString();
    if (out.isEmpty() || QFileInfo::exists(out))
        fail("Требуется новый выходной каталог");
    auto parent = QFileInfo(out).absoluteDir();
    if (!parent.mkpath("."))
        fail("Невозможно создать каталог");
    auto scene = newDocument();
    auto a = setup(scene);
    QImageReader reader(args.front());
    reader.setAutoTransform(true);
    auto image = reader.read();
    if (image.isNull())
        fail(reader.errorString());
    double aperture = options["plate"].toDouble(40), thick = options["thickness"].toDouble(8),
           width = options["width"].toDouble(50),
           height = options["height"].toDouble(width * image.height() / image.width()),
           index = options["index"].toDouble(1.49), source = options["source"].toDouble(600),
           screen = options["screen"].toDouble(260);
    if (!(aperture > 0 && width > 0 && height > 0 && thick > 0 && index > 1 && index <= 6 && source > 0 &&
          screen > thick))
        fail("Некорректная оптическая схема");
    a.plate["dims"] = QJsonObject{{"w", aperture}, {"h", thick}};
    a.screens[0]["dims"] = QJsonObject{{"w", width}, {"d", height}};
    scene["materials"] = QJsonObject{{"pmma", QJsonObject{{"indices", QJsonObject{{"589.3", index}}}}}};
    auto fit = options["image-fit"].toString("fit"), layout = options["mesh-layout"].toString("uniform");
    if ((fit != "fit" && fit != "crop") || (layout != "transport" && layout != "uniform"))
        fail("Неизвестный режим размещения");
    auto formats = options["formats"].toString("stl").toLower().split(',');
    formats.removeDuplicates();
    bool mesh = false;
    for (auto f : formats) {
        if (!QStringList{"stl", "step", "brep", "obj", "3mf"}.contains(f))
            fail("Неизвестный формат " + f);
        mesh |= f != "step" && f != "brep";
    }
    if (!options.contains("max-relief"))
        options["max-relief"] = 5;
    if (!options.contains("cells"))
        options["cells"] = 1024;
    if (!mesh)
        options["cells"] = 0;
    int res = options["resolution"].toInt(448);
    if (res < 16 || res > 1024)
        fail("Недопустимое разрешение");
    QTemporaryDir work(parent.filePath(".photo-XXXXXX"));
    if (!work.isValid())
        fail("Ошибка рабочей папки");
    auto target = photo(args.front(), pixels(a.screens[0], res), fit == "crop");
    photoDesign(work.path(), target, scene, a, source, screen, 2, options);
    auto result = work.filePath("result");
    auto others = formats;
    others.removeAll("stl");
    if (!others.isEmpty()) {
        QDir d(result);
        if (!d.mkdir("formats"))
            fail("Ошибка каталога экспорта");
        auto staged = d.filePath("formats");
        writeModels(result, staged, others, 0);
        for (auto f : others)
            if (!QFile::rename(path(staged, "plate." + f), path(result, "02_plate." + f)))
                fail("Ошибка публикации модели");
        d.rmdir("formats");
    }
    if (!formats.contains("stl"))
        QFile::remove(path(result, "02_plate.stl"));
    auto report = json(path(result, "report.json"));
    QJsonArray files;
    for (auto f : formats)
        files.append("02_plate." + f);
    report["model_files"] = files;
    save(path(result, "report.json"), QJsonDocument(report).toJson());
    if (!QDir().rename(result, QFileInfo(out).absoluteFilePath()))
        fail("Не удалось сохранить расчёт");
}
struct BrightnessSums {
    double dot = 0, energy = 0, targetEnergy = 0;
};
static BrightnessSums brightnessSums(const QString &image, const cv::Mat &target, double peak) {
    QImage actual = QImage(image).convertToFormat(QImage::Format_Grayscale8);
    if (actual.size() != QSize(target.cols, target.rows))
        fail("Разрешение экрана и цели различается");
    BrightnessSums sums;
    double lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = linear(i / 255.) * peak;
    for (int y = 0; y < target.rows; ++y) {
        auto src = actual.constScanLine(y);
        auto wanted = target.ptr<double>(y);
        for (int x = 0; x < target.cols; ++x) {
            double a = lut[src[x]], b = wanted[x];
            sums.dot += a * b;
            sums.energy += a * a;
            sums.targetEnergy += b * b;
        }
    }
    return sums;
}
QJsonObject placementOptions(const QJsonObject &req) {
    auto scene = req["scene"].toObject(), recv = object(scene, req["screen"].toString());
    auto a = setup(scene);
    auto assets = req["assets"].toString(), dir = req["out"].toString();
    double lo = req["min_gap"].toDouble(), hi = req["max_gap"].toDouble(),
           maximum = req["max_lamp"].toDouble(), size = req["size"].toDouble();
    bool sun = req["mode"] == "sun";
    if (!(lo > 0 && hi >= lo && maximum >= 10 && size > 0) || recv["kind"] != "screen")
        fail("Проверьте диапазон установки и экран");
    for (auto o : {recv, a.lamp}) {
        if (o.contains("parent") || o["locked"].toBool())
            fail("Для подбора источник и экран должны быть подвижными");
        for (auto p = a.plate; !p.isEmpty(); p = object(scene, p["parent"].toString()))
            if (p["id"] == o["id"])
                fail("Пластина закреплена на подвижном объекте");
    }
    auto base = pose(a.plate), local = base.Inverted() * pose(recv);
    auto h = dim(a.plate, "h"), w = dim(a.plate, "w"), pw = dim(recv, "w"), ph = dim(recv, "d"),
         n = indexOf(scene, a.plate);
    std::set<double> gaps{std::clamp(local.Value(3, 4) - h, lo, hi)}, lamps;
    for (int i = 0; i <= 6; ++i)
        gaps.insert(lo + (hi - lo) * i / 6);
    if (sun)
        lamps.insert(75);
    else
        for (auto d : {maximum * .45, maximum * .7, maximum, -(base.Inverted() * pose(a.lamp)).Value(3, 4)})
            lamps.insert(std::clamp(d, 10., maximum));
    QList<QJsonObject> rows;
    int counter = 0;
    auto cfg = path(dir, "layout.txt");
    int screenIndex = a.screens.indexOf(recv);
    cv::Mat target;
    if (recv.contains("image"))
        target = goal(assets, recv, 64, 0, nullptr);
    for (double g : gaps)
        for (double d : lamps) {
            auto s = scene, source = a.lamp;
            source["kind"] = sun ? "sun" : "area";
            if (source["name"] == label(a.lamp["kind"].toString()))
                source["name"] = label(source["kind"].toString());
            source["dims"] = sun ? QJsonObject{} : QJsonObject{{"w", size}, {"d", size}};
            put(s, source);
            gp_Trsf shift;
            shift.SetTranslation(gp_Vec(0, 0, -d));
            moveBody(s, source["id"].toString(), base * shift);
            auto tr = local;
            auto p = tr.TranslationPart();
            p.SetZ(h + g);
            tr.SetTranslationPart(p);
            moveBody(s, recv["id"].toString(), base * tr);
            if (!diagnostics(s).isEmpty() || !inspectScene(s, assets, true).isEmpty())
                continue;
            double blur = g * (sun ? 2 * std::tan(.004652) : size / d), slope = 0;
            for (int x : {-1, 1})
                for (int y : {-1, 1}) {
                    gp_Vec out(gp_Pnt(x * w / 2, y * w / 2, h),
                               gp_Pnt(x * pw / 2, y * ph / 2, 0).Transformed(tr));
                    if (out.SquareMagnitude() < 1e-16) {
                        slope = 1e99;
                        continue;
                    }
                    out.Normalize();
                    auto in = sun ? gp_Vec(0, 0, 1) : gp_Vec(x * w / 2, y * w / 2, d).Normalized();
                    double ix = in.X() / n, iy = in.Y() / n;
                    auto normal = n * gp_Vec(ix, iy, std::sqrt(1 - ix * ix - iy * iy)) - out;
                    slope = std::max(slope, std::hypot(normal.X(), normal.Y()) / std::max(normal.Z(), 1e-9));
                }
            double score = blur * 64 / std::max(pw, ph) + .9 * slope + .12 * g / hi +
                           (sun ? 0 : .2 * w / d + .06 * d / maximum);
            QJsonObject row{{"scene", s}, {"gap", g}, {"lamp", d}, {"blur", blur}};
            if (a.plate.contains("profile")) {
                auto out = path(dir, QString("layout-%1").arg(counter++));
                save(cfg, traceSettings(s, 64, 120000));
                caustic::trace_scene(cfg.toStdWString(),
                                     assetFile(assets, a.plate["profile"].toString()).toStdWString(),
                                     out.toStdWString());
                auto report = json(path(out, "report.json"));
                auto screen = report["screens"].toArray().at(screenIndex).toObject();
                if (screen["flux"].toDouble() <= 0)
                    continue;
                double capture = report["hit"].toDouble() / report["rays"].toDouble();
                row["capture"] = capture;
                score = .12 * (1 - capture) + .025 * score;
                if (!target.empty()) {
                    auto sums = brightnessSums(path(out, screen["image"].toString()), target, 1);
                    auto error = std::sqrt(
                        std::max(0., sums.targetEnergy - sums.dot * sums.dot / std::max(1e-30, sums.energy)) /
                        std::max(1e-16, sums.targetEnergy));
                    row["error"] = error;
                    score += error;
                }
            }
            row["score"] = score;
            rows << row;
        }
    if (rows.empty())
        fail("Подходящих положений в заданном диапазоне нет");
    std::stable_sort(rows.begin(), rows.end(),
                     [](auto a, auto b) { return a["score"].toDouble() < b["score"].toDouble(); });
    QJsonArray choices;
    for (int i = 0; i < std::min(3, int(rows.size())); ++i)
        choices.append(rows[i]);
    return {{"choices", choices}};
}
QJsonObject solve(const QJsonObject &req) {
    auto scene = req["scene"].toObject();
    validateDocument(scene);
    if (req["task"] == "export") {
        auto folder = req["assets"].toString(), dest = req["destination"].toString();
        auto o = object(scene, req["id"].toString()), data = resolved(scene, o);
        auto k = o["kind"].toString(), ext = QFileInfo(dest).suffix().toLower();
        if (!solid(k) || dest.isEmpty())
            fail("Выберите тело и формат");
        auto tr = pose(o);
        if (ext == "step" || ext == "stp" || ext == "brep") {
            auto tmp = path(req["out"].toString(), "body." + ext);
            write_solid(build_body(data, folder.toStdWString()).Moved(TopLoc_Location(tr)),
                        tmp.toStdWString(), k == "plate");
            QFile f(tmp);
            QSaveFile out(dest);
            if (!f.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly))
                fail("Ошибка экспорта");
            while (!f.atEnd()) {
                auto b = f.read(262144);
                if (b.isEmpty() || out.write(b) != b.size())
                    fail("Ошибка записи");
            }
            if (!out.commit())
                fail(out.errorString());
        } else {
            caustic::Mesh mesh;
            if (o.contains("profile"))
                mesh = surfaceMesh(profile(assetFile(folder, o["profile"].toString())).plate,
                                   req["cells"].toInt());
            else if (k == "mesh")
                mesh = meshFile(assetFile(folder, o["asset"].toString()));
            else
                mesh = shapeMesh(build_body(data, folder.toStdWString()));
            exportMesh(mesh, dest, tr);
        }
        return {{"exported", dest}};
    }
    if (req["task"] == "inspect")
        return {{"issues", inspectScene(scene, req["assets"].toString())}};
    if (req["task"] == "placement")
        return placementOptions(req);
    if (req["task"] != "trace" && req["task"] != "design")
        fail("Неизвестная задача");
    auto a = setup(scene);
    auto issues = diagnostics(scene);
    if (!issues.isEmpty())
        fail(issues.first());
    auto assets = req["assets"].toString(), dir = req["out"].toString();
    int quality = std::clamp(req["quality"].toInt(1), 0, 2), res = req["resolution"].toInt(256),
        rays = req["rays"].toInt(2000000);
    auto precise = inspectScene(scene, assets, true);
    if (!precise.isEmpty())
        fail(precise.at(0).toObject()["message"].toString());
    auto cfg = path(dir, "scene.txt"), result = path(dir, "result"),
         surface =
             a.plate.contains("profile") ? assetFile(assets, a.plate["profile"].toString()) : QString("-");
    auto turn = scene["turntable"].toObject();
    std::optional<caustic::Mesh> mesh;
    if (!turn.isEmpty())
        mesh = meshFile(assetFile(assets, turn["asset"].toString()));
    bool design = req["task"] == "design";
    std::set<double> angles{0};
    if (design && mesh) {
        angles.clear();
        for (auto v : turn["angles"].toArray())
            angles.insert(v.toDouble());
    }
    if (design && !mesh)
        for (auto s : a.screens)
            if (!s.contains("image"))
                fail("Назначьте изображения всем экранам");
    if (design && !mesh && a.screens.size() == 1 && a.lamp["kind"] == "point") {
        auto p = pose(a.plate).Inverted();
        auto l = p * pose(a.lamp), t = p * pose(a.screens[0]);
        bool aligned = true;
        for (int i = 1; i <= 3; ++i)
            for (int j = 1; j <= 3; ++j)
                aligned &= std::abs(t.Value(i, j) - (i == j)) < 1e-7;
        if (aligned && std::hypot(l.Value(1, 4), l.Value(2, 4)) < 1e-6 &&
            std::hypot(t.Value(1, 4), t.Value(2, 4)) < 1e-6 && t.Value(3, 4) > dim(a.plate, "h")) {
            auto target = goal(assets, a.screens[0], std::array{96, 256, 448}.at(quality), 0, nullptr);
            photoDesign(dir, target, scene, a, -l.Value(3, 4), t.Value(3, 4), quality);
            return {{"profile", path(result, "surface.txt")},
                    {"screens", QJsonArray{QJsonObject{{"name", a.screens[0]["name"]},
                                                       {"image", path(result, "03_projection.png")},
                                                       {"target", path(result, "01_monochrome.png")},
                                                       {"gain", 1.}}}}};
        }
    }
    if (design) {
        if (1024LL * angles.size() * a.screens.size() > 1000000)
            fail("Суммарное разрешение целей превышает бюджет оптимизации");
        save(cfg, traceSettings(scene, 32, 16000));
        QByteArray list = "7 " + QByteArray::number(int(angles.size()));
        int j = 0;
        for (auto angle : angles) {
            list += " " + QByteArray::number(angle, 'g', 17);
            int k = 0;
            for (auto s : a.screens) {
                pgm(path(dir, QString("%1_%2.pgm").arg(j).arg(k++)),
                    goal(assets, s, 32, angle, mesh ? &*mesh : nullptr));
            }
            ++j;
        }
        save(path(dir, "angles.txt"), list);
        caustic::fit_scene(dir.toStdWString(), result.toStdWString());
        auto fit = json(path(result, "fit.json"));
        if (!(fit["final"].toDouble() < fit["initial"].toDouble()))
            fail("Подбор не улучшил плоскую заготовку");
        surface = path(result, "surface.txt");
        res = 96;
        rays = 240000;
    }
    auto checkAngles = angles;
    if (angles.size() > 1)
        for (auto i = std::next(angles.begin()); i != angles.end(); ++i)
            checkAngles.insert((*i + *std::prev(i)) / 2);
    save(cfg, traceSettings(scene, res, rays));
    QJsonArray entries;
    double totalDot = 0, totalEnergy = 0;
    int j = 0;
    for (auto angle : checkAngles) {
        auto out = path(dir, QString("trace-%1").arg(j++));
        caustic::trace_scene(cfg.toStdWString(), surface.toStdWString(), out.toStdWString(), angle);
        auto report = json(path(out, "report.json"));
        int k = 0;
        for (auto s : a.screens) {
            auto r = report["screens"].toArray().at(k).toObject();
            QJsonObject row{{"name", s["name"]},
                            {"angle", angle},
                            {"intermediate", !angles.contains(angle)},
                            {"image", path(out, r["image"].toString())},
                            {"flux", r["flux"]},
                            {"peak", report["peak"]}};
            if (mesh || s.contains("image")) {
                auto img = goal(assets, s, res, angle, mesh ? &*mesh : nullptr);
                auto file = path(out, QString("goal-%1.png").arg(k));
                if (!preview(img).save(file))
                    fail("Ошибка сохранения цели");
                row["target"] = file;
                if (design) {
                    auto stats = brightnessSums(row["image"].toString(), img, report["peak"].toDouble());
                    row["dot"] = stats.dot;
                    row["energy"] = stats.energy;
                    row["target_energy"] = stats.targetEnergy;
                    if (angles.contains(angle)) {
                        totalDot += stats.dot;
                        totalEnergy += stats.energy;
                    }
                }
            }
            entries.append(row);
            ++k;
        }
    }
    if (design) {
        double gain = totalEnergy > 0 ? totalDot / totalEnergy : 0;
        bool reached = false;
        for (int i = 0; i < entries.size(); ++i) {
            auto row = entries.at(i).toObject();
            row["gain"] = gain * row["peak"].toDouble();
            reached |= row["flux"].toDouble() > 0;
            double targetEnergy = row.take("target_energy").toDouble();
            row["error"] = std::sqrt(std::max(0., gain * gain * row.take("energy").toDouble() -
                                                      2 * gain * row.take("dot").toDouble() + targetEnergy) /
                                     std::max(1e-16, targetEnergy));
            entries[i] = row;
        }
        if (!reached)
            fail("Лучи не достигли экранов");
    }
    QJsonObject answer{{"screens", entries}};
    if (design)
        answer["profile"] = surface;
    return answer;
}
} // namespace raysculpt
