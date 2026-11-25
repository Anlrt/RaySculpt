#include "verify.hpp"
#include "geometry.hpp"
#include "image_file.hpp"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <numeric>
#include <optional>
#include <random>
#include <thread>

namespace raysculpt {
using caustic::Vec3;
struct Facet {
    Vec3 a, b, c;
};
static Facet readFacet(std::istream &file) {
    std::array<char, 50> bytes{};
    if (!file.read(bytes.data(), 50))
        fail("Оборванная запись бинарного STL");
    float p[9];
    std::memcpy(p, bytes.data() + 12, sizeof p);
    return {{p[0], p[1], p[2]}, {p[3], p[4], p[5]}, {p[6], p[7], p[8]}};
}
static double fresnel(double ci, double ct, double from, double to) {
    auto s = (from * ci - to * ct) / (from * ci + to * ct), p = (from * ct - to * ci) / (from * ct + to * ci);
    return 1 - (s * s + p * p) / 2;
}
struct Deposit {
    std::vector<double> bins;
    uint64_t side = 0, tir = 0, miss = 0;
    double outside = 0, area = 0;
};
struct UniformTop {
    int n;
    double width;
    std::vector<float> x, y, z;
    double bound = 0;
    UniformTop(const std::filesystem::path &path, int cells, double w)
        : n(cells), width(w), x(n + 1), y(n + 1),
          z(size_t(n + 1) * (n + 1), std::numeric_limits<float>::quiet_NaN()) {
        std::ifstream in(path, std::ios::binary);
        in.seekg(84);
        auto store = [&](int i, int j, Vec3 v) {
            auto &a = z[size_t(j) * (n + 1) + i];
            if (std::isfinite(a) && a != float(v.z))
                fail("STL содержит разрыв рельефа");
            double tolerance = std::max(1e-6, width * 1e-7);
            if (!(v.z > 0) || std::abs(v.x - (double(i) / n - .5) * width) > tolerance ||
                std::abs(v.y - (double(j) / n - .5) * width) > tolerance)
                fail("Для нерегулярной сетки используйте trace-facets");
            a = float(v.z);
            x[i] = float(v.x);
            y[j] = float(v.y);
        };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                auto p = readFacet(in), q = readFacet(in);
                store(i, j, p.a);
                store(i + 1, j, p.b);
                store(i + 1, j + 1, p.c);
                store(i, j, q.a);
                store(i + 1, j + 1, q.b);
                store(i, j + 1, q.c);
                for (auto face : {p, q}) {
                    auto normal = caustic::cross(face.b - face.a, face.c - face.a);
                    if (normal.z <= 0)
                        fail("Вырожденная грань");
                    bound = std::max(bound, (std::abs(normal.x) + std::abs(normal.y)) / normal.z);
                }
            }
    }
    std::array<double, 3> sample(double px, double py) const {
        px = std::clamp(px, double(x.front()), double(x.back()));
        py = std::clamp(py, double(y.front()), double(y.back()));
        int i = std::clamp(int((px / width + .5) * n), 0, n - 1),
            j = std::clamp(int((py / width + .5) * n), 0, n - 1);
        i = std::clamp(i + (px > x[i + 1]) - (px < x[i]), 0, n - 1);
        j = std::clamp(j + (py > y[j + 1]) - (py < y[j]), 0, n - 1);
        auto a = size_t(j) * (n + 1) + i, b = a + 1, d = a + n + 1, c = d + 1;
        double dx = x[i + 1] - x[i], dy = y[j + 1] - y[j], ux = px - x[i], uy = py - y[j];
        bool lower = ux * dy >= uy * dx;
        double gx = (lower ? double(z[b]) - z[a] : double(z[c]) - z[d]) / dx,
               gy = (lower ? double(z[c]) - z[b] : double(z[d]) - z[a]) / dy;
        return {z[a] + gx * ux + gy * uy, gx, gy};
    }
};
QJsonObject verifyFacets(const QStringList &args, bool exitSampling) {
    if (args.isEmpty())
        fail("Укажите каталог расчёта");
    int strata = exitSampling ? 4 : 4000;
    double emitter = 0;
    for (int i = 1; i < args.size(); i += 2) {
        if (i + 1 >= args.size())
            fail("Отсутствует значение");
        bool ok;
        if (args[i] == "--source-size") {
            emitter = args[i + 1].toDouble(&ok);
        } else if (args[i] == (exitSampling ? "--strata" : "--side"))
            strata = args[i + 1].toInt(&ok);
        else
            fail("Неизвестный параметр");
        if (!ok)
            fail("Некорректное число");
    }
    if (!(emitter >= 0 && emitter <= 100) || strata < (exitSampling ? 1 : 100) ||
        strata > (exitSampling ? 16 : 12000))
        fail("Недопустимый размер выборки");
    QDir dir(args[0]);
    auto design = profile(dir.filePath("surface.txt"));
    auto &plate = design.plate;
    double n = plate.refractive_index, width = plate.width_mm, source = design.source;
    QFile f(dir.filePath("report.json"));
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    auto info = QJsonDocument::fromJson(f.readAll()).object();
    int cells = info["mesh_cells"].toInt();
    if (cells < 2 || cells > 2048)
        fail("Некорректная сетка STL");
    auto pix = info["picture_px"].toArray(), guard = info["guard_px"].toArray();
    int w = pix.at(0).toInt(), h = pix.at(1).toInt(), gx = guard.at(0).toInt(), gy = guard.at(1).toInt();
    if (w < 1 || h < 1 || w > 2048 || h > 2048 || gx < 0 || gy < 0 || gx > 2048 || gy > 2048)
        fail("Некорректный экран");
    int sw = w + 2 * gx, sh = h + 2 * gy;
    auto goal = caustic::load_target_image(dir.filePath("01_monochrome.png").toStdWString());
    if (goal.width != sw || goal.height != sh)
        fail("Размер монохромной цели не совпадает с отчётом");
    std::filesystem::path path = dir.filePath("02_plate.stl").toStdWString();
    int64_t top = 2LL * cells * cells;
    std::ifstream file(path, std::ios::binary);
    uint32_t count = 0;
    file.seekg(80);
    file.read(reinterpret_cast<char *>(&count), 4);
    if (!file || count != top + 12 * cells || std::filesystem::file_size(path) != 84 + 50ULL * count)
        fail("Неверный размер или топология STL");
    std::optional<UniformTop> uniform;
    if (!exitSampling) {
        uniform.emplace(path, cells, width);
        if (uniform->bound * (width + emitter) / (2 * n * source) >= .9)
            fail("Крутизна выходит за предел проверки");
    }
    int64_t jobs = exitSampling ? top : strata;
    int workers = std::max(
        1, std::min({12, int(std::thread::hardware_concurrency()), int(jobs), int(16000000LL / (sw * sh))}));
    auto work = [&](int worker) {
        Deposit result;
        result.bins.resize(size_t(sw) * sh);
        std::mt19937_64 rng(975319 + worker);
        std::uniform_real_distribution<double> rand(0, 1);
        auto deposit = [&](Vec3 p, Vec3 inside, double sx, double sy, double weight) {
            double l = std::sqrt(1 + sx * sx + sy * sy), ci = (inside.z - inside.x * sx - inside.y * sy) / l,
                   k = 1 - n * n * (1 - ci * ci);
            if (k < 0 || ci <= 0) {
                ++result.tir;
                return;
            }
            double ct = std::sqrt(k), bend = (n * ci - ct) / l, oz = n * inside.z - bend;
            if (oz <= 0) {
                ++result.miss;
                return;
            }
            double t = (design.screen - p.z) / oz;
            double x = (p.x + (n * inside.x + bend * sx) * t) * w / design.width + sw * .5,
                   y = -(p.y + (n * inside.y + bend * sy) * t) * h / design.height + sh * .5;
            if (!(x >= 0 && x < sw && y >= 0 && y < sh)) {
                ++result.miss;
                return;
            }
            result.bins[size_t(int(y)) * sw + int(x)] += weight * fresnel(ci, ct, n, 1);
        };
        auto begin = jobs * worker / workers, end = jobs * (worker + 1) / workers;
        std::ifstream in;
        if (exitSampling) {
            in.open(path, std::ios::binary);
            in.seekg(84 + 50 * begin);
        }
        for (int64_t j = begin; j < end; ++j) {
            if (exitSampling) {
                auto tri = readFacet(in);
                auto ex = tri.b - tri.a, ey = tri.c - tri.a, norm = caustic::cross(ex, ey);
                double area = norm.z / 2;
                if (!(area > 0))
                    fail("Перевёрнутая грань STL");
                result.area += area;
                double sx = -norm.x / norm.z, sy = -norm.y / norm.z;
                if ((std::abs(sx) + std::abs(sy)) * (width + emitter) / (2 * n * source) >= .9)
                    fail("Слишком крутая грань");
                for (int a = 0; a < strata; ++a)
                    for (int b = 0; b < strata; ++b) {
                        double u = (a + rand(rng)) / strata, v = (b + rand(rng)) / strata;
                        if (u + v > 1) {
                            u = 1 - u;
                            v = 1 - v;
                        }
                        auto p = tri.a + ex * u + ey * v;
                        double lx = emitter * (rand(rng) - .5), ly = emitter * (rand(rng) - .5),
                               rx = p.x - lx, ry = p.y - ly, fac = 1 + p.z / (n * source), q = 0;
                        bool converged = false;
                        for (int k = 0; k < 16; ++k) {
                            q = std::sqrt(n * n * source * source +
                                          (n * n - 1) * (rx * rx + ry * ry) / (fac * fac));
                            auto next = 1 + p.z / q;
                            if (std::abs(next - fac) < 2e-13) {
                                converged = true;
                                break;
                            }
                            fac = next;
                        }
                        if (!converged)
                            fail("Не сошёлся расчёт входной точки");
                        auto x = rx / fac, y = ry / fac;
                        double distance = std::hypot(x, y, source), cx = x / (distance * n),
                               cy = y / (distance * n), cz = std::sqrt(1 - cx * cx - cy * cy),
                               c0 = source / distance;
                        auto jac = (1 - (x * sx + y * sy) / q) /
                                   ((1 + p.z / q) * (1 + p.z * n * n * source * source / (q * q * q)));
                        double weight = area / (strata * strata) * jac * c0 / (distance * distance) *
                                        fresnel(c0, cz, 1, n) * (emitter ? c0 : 1);
                        if (std::abs(x + lx) > width / 2 || std::abs(y + ly) > width / 2) {
                            result.outside += weight;
                            continue;
                        }
                        deposit(p, {cx, cy, cz}, sx, sy, weight);
                    }
            } else
                for (int i = 0; i < strata; ++i) {
                    double x = ((i + rand(rng)) / strata - .5) * width,
                           y = ((j + rand(rng)) / strata - .5) * width, dx = x - emitter * (rand(rng) - .5),
                           dy = y - emitter * (rand(rng) - .5), dist = std::hypot(dx, dy, source),
                           ix = dx / (n * dist), iy = dy / (n * dist), iz = std::sqrt(1 - ix * ix - iy * iy),
                           level = plate.thickness_mm;
                    double px = 0, py = 0, sx = 0, sy = 0;
                    bool converged = false;
                    for (int k = 0; k < 40; ++k) {
                        px = x + level * ix / iz;
                        py = y + level * iy / iz;
                        auto z = uniform->sample(px, py);
                        sx = z[1];
                        sy = z[2];
                        if (std::abs(z[0] - level) < 1e-8) {
                            converged = true;
                            break;
                        }
                        level = z[0];
                    }
                    if (!converged)
                        fail("Не сошлось пересечение луча с STL");
                    if (std::abs(px) > width / 2 || std::abs(py) > width / 2) {
                        ++result.side;
                        continue;
                    }
                    double c0 = source / dist;
                    deposit({px, py, level}, {ix, iy, iz}, sx, sy,
                            c0 / (dist * dist) * fresnel(c0, iz, 1, n) * (emitter ? c0 : 1));
                }
        }
        return result;
    };
    std::vector<std::future<Deposit>> pending;
    for (int i = 0; i < workers; ++i)
        pending.push_back(std::async(std::launch::async, work, i));
    Deposit total;
    total.bins.resize(size_t(sw) * sh);
    for (auto &future : pending) {
        auto r = future.get();
        for (size_t i = 0; i < r.bins.size(); ++i)
            total.bins[i] += r.bins[i];
        total.area += r.area;
        total.outside += r.outside;
        total.tir += r.tir;
        total.side += r.side;
        total.miss += r.miss;
    }
    double flux = std::accumulate(total.bins.begin(), total.bins.end(), 0.),
           energy = std::accumulate(goal.pixels.begin(), goal.pixels.end(), 0.);
    if (!(flux > 0 && energy > 0))
        fail("Поток на экране равен нулю");
    double gain = energy / flux, err = 0, norm = 0, inside = 0;
    for (int y = gy; y < gy + h; ++y)
        for (int x = gx; x < gx + w; ++x) {
            auto i = size_t(y) * sw + x;
            inside += total.bins[i];
            err += std::pow(gain * total.bins[i] - goal.pixels[i], 2);
            norm += std::pow(goal.pixels[i], 2);
        }
    if (exitSampling && std::abs(total.area - width * width) > 1e-5 * width * width)
        fail("Фасеты не покрывают апертуру");
    QString suffix = emitter ? QString("_source_%1mm").arg(emitter) : QString();
    QJsonObject report{{"rays", qint64(exitSampling ? top * strata * strata : int64_t(strata) * strata)},
                       {"source_size_mm", emitter},
                       {"tir", qint64(total.tir)},
                       {"miss", qint64(total.miss)},
                       {"picture_relative_rmse", std::sqrt(err / std::max(1e-16, norm))},
                       {"captured_flux_guard_pct", 100 * (1 - inside / flux)}};
    if (exitSampling) {
        report["exit_area_mm2"] = total.area;
        report["captured_flux"] = flux;
        report["unilluminated_exit_weight"] = total.outside;
    } else
        report["side_escape"] = qint64(total.side);
    caustic::save_projection_image(
        dir.filePath(QString(exitSampling ? "04_facet_projection" : "04_stl_projection") + suffix + ".png")
            .toStdWString(),
        sw, sh, total.bins, gain);
    QSaveFile out(
        dir.filePath(QString(exitSampling ? "facet_trace" : "stl_verification") + suffix + ".json"));
    auto bytes = QJsonDocument(report).toJson();
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
        fail(out.errorString());
    return report;
}
} // namespace raysculpt
