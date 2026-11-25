#include "geometry.hpp"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <optional>
#include <cmath>

namespace raysculpt {
static QStringList formats(QString argument) {
    QStringList result;
    for (auto name : argument.toLower().split(',')) {
        if (name == "stp")
            name = "step";
        if (!QStringList{"step", "brep", "stl", "obj", "3mf"}.contains(name))
            fail("Неизвестный формат " + name);
        if (!result.contains(name))
            result << name;
    }
    return result;
}
caustic::HeightField designProfile(const QString &source, QString &previous) {
    QString file = source;
    if (QFileInfo(file).isDir()) {
        QDir folder(file);
        file = folder.filePath(folder.exists("surface.txt") ? "surface.txt" : "report.json");
    }
    caustic::HeightField h;
    if (file.endsWith(".json", Qt::CaseInsensitive)) {
        QFile json(file);
        if (!json.open(QIODevice::ReadOnly))
            fail(json.errorString());
        auto meta = QJsonDocument::fromJson(json.readAll()).object();
        if (!meta.contains("spline_coefficients_mm") && QFileInfo(file).dir().exists("surface.txt")) {
            return designProfile(QFileInfo(file).dir().filePath("surface.txt"), previous);
        }
        if (meta["surface"] != "uniform_cubic_b_spline")
            fail("Неизвестный тип оптической поверхности");
        int n = meta["grid"].toInt();
        h.width_mm = h.height_mm = meta["plate_mm"].toDouble();
        h.thickness_mm = meta["thickness_mm"].toDouble();
        h.samples_x = h.samples_y = n;
        h.interpolation = caustic::HeightInterpolation::CubicSpline;
        auto coeff = meta["spline_coefficients_mm"].toArray();
        if (n < 3 || n > 1024 || coeff.size() != n * n || !(h.width_mm > 0 && h.thickness_mm > 0))
            fail("Некорректный отчёт");
        h.offsets_mm.clear();
        for (auto v : coeff) {
            if (!v.isDouble() || h.thickness_mm + v.toDouble() <= 0)
                fail("Некорректная высота");
            h.offsets_mm.push_back(v.toDouble());
        }
        previous = QFileInfo(file).dir().filePath("plate.stl");
    } else {
        h = profile(file).plate;
        if (QFileInfo(file).fileName() == "surface.txt")
            previous = QFileInfo(file).dir().filePath("02_plate.stl");
    }
    return h;
}
void writeModels(const QString &source, const QString &directory, const QStringList &requested, int cells) {
    QString previous;
    auto h = designProfile(source, previous);
    std::optional<caustic::Mesh> mesh;
    TopoDS_Shape shape;
    for (auto ext : requested) {
        auto out = QDir(directory).filePath("plate." + ext);
        if (ext == "step" || ext == "brep") {
            if (shape.IsNull())
                shape = splineSolid(h);
            write_solid(shape, out.toStdWString(), true);
        } else if (!cells && QFileInfo::exists(previous) && ext == "stl") {
            if (QFileInfo(out) != QFileInfo(previous) && !QFile::copy(previous, out))
                fail("Ошибка копирования STL");
        } else {
            if (!mesh)
                mesh = !cells && QFileInfo::exists(previous)
                           ? meshFile(previous)
                           : surfaceMesh(h, cells ? cells : std::max(256, int(h.samples_x) - 1));
            exportMesh(*mesh, out, gp_Trsf());
        }
    }
}
void exportPlate(const QStringList &args) {
    if (args.isEmpty())
        fail("Требуется путь к профилю");
    QString dest;
    QStringList names;
    int cells = 0;
    for (int i = 1; i < args.size(); i += 2) {
        if (i + 1 >= args.size())
            fail("Отсутствует значение параметра");
        auto key = args[i], value = args[i + 1];
        if (key == "--out")
            dest = value;
        else if (key == "--formats")
            names = formats(value);
        else if (key == "--cells") {
            bool ok;
            cells = value.toInt(&ok);
            if (!ok || cells < 2 || cells > 2048)
                fail("Число ячеек должно быть от 2 до 2048");
        } else
            fail("Неизвестный параметр " + key);
    }
    if (dest.isEmpty() || QFileInfo::exists(dest))
        fail("Требуется новый выходной файл или каталог");
    auto info = QFileInfo(dest);
    auto ext = info.suffix().toLower();
    if (ext == "stp")
        ext = "step";
    bool one = QStringList{"step", "brep", "stl", "3mf", "obj"}.contains(ext);
    if (one && !names.isEmpty() && names != QStringList{ext})
        fail("Для нескольких форматов укажите каталог");
    if (!one && !ext.isEmpty() && names.isEmpty())
        fail("Неизвестное расширение");
    if (names.isEmpty())
        names << (one ? ext : "step");
    auto parent = info.dir();
    if (!parent.mkpath("."))
        fail("Не удалось создать каталог");
    QTemporaryDir tmp(parent.filePath(".plate-XXXXXX"));
    if (!tmp.isValid() || !QDir(tmp.path()).mkdir("models"))
        fail("Не удалось создать временный каталог");
    auto folder = tmp.filePath("models");
    writeModels(args.front(), folder, names, cells);
    auto from = one ? QDir(folder).filePath("plate." + ext) : folder;
    if (!QDir().rename(from, info.absoluteFilePath()))
        fail("Ошибка записи экспорта");
}
} // namespace raysculpt
