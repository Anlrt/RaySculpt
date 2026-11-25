#include "document.hpp"
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QUuid>
#include <QTextStream>
#include <archive.h>
#include <archive_entry.h>
#include <gp_Quaternion.hxx>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <memory>

namespace raysculpt {
void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
QJsonArray vec(double x, double y, double z) { return {x, y, z}; }
static const QMap<QString, QString> labels = {{"plate", "Оптическая пластина"},
                                              {"box", "Блок"},
                                              {"sphere", "Сфера"},
                                              {"cylinder", "Цилиндр"},
                                              {"prism", "Выдавливание"},
                                              {"cad", "Тело CAD"},
                                              {"mesh", "Полигональная модель"},
                                              {"screen", "Экран"},
                                              {"sun", "Солнечный свет"},
                                              {"point", "Точечный источник"},
                                              {"area", "Площадной источник"}};
QString label(const QString &kind) { return labels.value(kind, kind); }
bool solid(const QString &k) {
    return QStringList{"plate", "box", "sphere", "cylinder", "prism", "cad", "mesh"}.contains(k);
}
QJsonObject newBody(const QString &kind) {
    QJsonObject d;
    if (kind == "plate")
        d = {{"w", 40}, {"h", 8}};
    if (kind == "box")
        d = {{"w", 40}, {"d", 32}, {"h", 24}};
    if (kind == "sphere")
        d = {{"r", 20}};
    if (kind == "cylinder")
        d = {{"r", 18}, {"h", 32}};
    if (kind == "screen" || kind == "area") {
        int n = kind == "screen" ? 50 : 10;
        d = {{"w", n}, {"d", n}};
    }
    if (kind == "prism")
        d = {{"h", 15}};
    QJsonObject obj{{"id", QUuid::createUuid().toString(QUuid::Id128)},
                    {"kind", kind},
                    {"name", label(kind)},
                    {"pos", vec(0, 0, 0)},
                    {"rot", vec(0, 0, 0)},
                    {"dims", d},
                    {"locked", false}};
    if (solid(kind)) {
        obj["material"] = "pmma";
        obj["line"] = "589.3";
        if (kind != "plate")
            obj["reference"] = true;
    }
    if (kind == "prism")
        obj["outline"] =
            QJsonArray{QJsonArray{-20, -15}, QJsonArray{20, -15}, QJsonArray{20, 15}, QJsonArray{-20, 15}};
    return obj;
}
static QString catalogPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath("CausticCAD/materials.json");
}
static QJsonObject readCatalog() {
    QFile f(catalogPath());
    if (!f.exists())
        return {};
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    auto json = QJsonDocument::fromJson(f.readAll());
    if (!json.isObject())
        fail("Повреждён справочник материалов");
    return json.object();
}
void storeMaterial(const QString &key, const QJsonObject &card) {
    auto path = catalogPath();
    if (!QFileInfo(path).dir().mkpath("."))
        fail("Ошибка создания каталога материалов");
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(2000))
        fail("Справочник открыт для записи");
    auto s = readCatalog();
    s[key] = card;
    auto bytes = QJsonDocument(s).toJson();
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        fail(f.errorString());
}
QJsonObject newDocument() {
    QFile f(":/materials.json");
    if (!f.open(QIODevice::ReadOnly))
        fail("Справочник материалов недоступен");
    QJsonArray objects;
    for (auto kind : {"plate", "point", "screen"}) {
        auto o = newBody(kind);
        o["rot"] = vec(90, 0, 90);
        o["pos"] = vec(QString(kind) == "point" ? -240 : QString(kind) == "screen" ? 125 : 0, 0, 25);
        objects.append(o);
    }
    auto materials = QJsonDocument::fromJson(f.readAll()).object();
    auto catalog = readCatalog();
    for (auto i = catalog.begin(); i != catalog.end(); ++i)
        materials[i.key()] = i.value();
    return {{"version", 2}, {"materials", materials}, {"objects", objects}};
}
QJsonObject object(const QJsonObject &s, const QString &id) {
    for (auto v : s["objects"].toArray())
        if (v.toObject()["id"].toString() == id)
            return v.toObject();
    return {};
}
void put(QJsonObject &s, const QJsonObject &o) {
    auto a = s["objects"].toArray();
    for (int i = 0; i < a.size(); ++i)
        if (a[i].toObject()["id"] == o["id"]) {
            a[i] = o;
            s["objects"] = a;
            return;
        }
    a.append(o);
    s["objects"] = a;
}
QSet<QString> consumed(const QJsonObject &s) {
    QSet<QString> ids;
    for (auto v : s["objects"].toArray())
        for (auto op : v.toObject()["operations"].toArray()) {
            auto o = op.toObject();
            if (o.contains("tool") && !o["suppressed"].toBool())
                ids.insert(o["tool"].toString());
        }
    return ids;
}
static constexpr double rad = std::numbers::pi / 180.;
gp_Trsf pose(const QJsonObject &o) {
    auto a = o["rot"].toArray(), p = o["pos"].toArray();
    gp_Quaternion q;
    q.SetEulerAngles(gp_Extrinsic_XYZ, a.at(0).toDouble() * rad, a.at(1).toDouble() * rad,
                     a.at(2).toDouble() * rad);
    gp_Trsf t;
    t.SetRotation(q);
    t.SetTranslationPart(gp_Vec(p.at(0).toDouble(), p.at(1).toDouble(), p.at(2).toDouble()));
    return t;
}
void assignPose(QJsonObject &o, const gp_Trsf &t) {
    double x, y, z;
    t.GetRotation().GetEulerAngles(gp_Extrinsic_XYZ, x, y, z);
    o["rot"] = vec(x / rad, y / rad, z / rad);
    auto p = t.TranslationPart();
    o["pos"] = vec(p.X(), p.Y(), p.Z());
}
void moveBody(QJsonObject &s, const QString &id, const gp_Trsf &t) {
    auto root = object(s, id);
    if (root["locked"].toBool() || root.contains("parent"))
        fail("Положение зафиксировано");
    auto delta = t * pose(root).Inverted();
    QSet<QString> moved{id};
    QStringList pending{id};
    while (!pending.isEmpty()) {
        auto parent = pending.takeLast();
        for (auto v : s["objects"].toArray()) {
            auto child = v.toObject();
            auto key = child["id"].toString();
            if (child["parent"].toString() != parent)
                continue;
            if (moved.contains(key))
                fail("Цикл связей");
            moved.insert(key);
            pending.append(key);
        }
    }
    for (auto key : moved) {
        auto child = object(s, key);
        assignPose(child, delta * pose(child));
        put(s, child);
    }
}
void arrangeScreens(QJsonObject &scene) {
    QJsonObject plate;
    QList<QJsonObject> screens;
    double width = 0;
    for (auto v : scene["objects"].toArray()) {
        auto obj = v.toObject();
        if (obj["kind"] == "plate" && !obj["reference"].toBool()) {
            if (!plate.isEmpty())
                fail("Выберите сцену с одной оптической пластиной");
            plate = obj;
        }
        if (obj["kind"] == "screen") {
            if (obj.contains("parent") || obj["locked"].toBool())
                fail("Для расстановки снимите фиксацию или связь экрана «" + obj["name"].toString() + "»");
            width += obj["dims"].toObject()["w"].toDouble();
            screens << obj;
        }
    }
    if (plate.isEmpty() || screens.size() < 2)
        fail("Добавьте оптическую пластину и два экрана");
    const double gap = 1.;
    double z = (pose(plate).Inverted() * pose(screens.front())).Value(3, 4);
    if (z <= plate["dims"].toObject()["h"].toDouble() + plate["max_relief"].toDouble())
        fail("Первый экран должен находиться за пластиной");
    double edge = -(width + (screens.size() - 1) * gap) * .5;
    auto next = scene;
    for (const auto &screen : screens) {
        const double w = screen["dims"].toObject()["w"].toDouble();
        gp_Trsf t;
        t.SetTranslation(gp_Vec(edge + w / 2, 0, z));
        moveBody(next, screen["id"].toString(), pose(plate) * t);
        edge += w + gap;
    }
    scene = next;
}
QJsonObject resolved(const QJsonObject &s, const QJsonObject &obj, QSet<QString> visiting) {
    auto key = obj["id"].toString();
    if (visiting.contains(key))
        fail("Цикл операций CAD");
    visiting.insert(key);
    auto a = obj["operations"].toArray();
    for (int i = 0; i < a.size(); ++i) {
        auto op = a[i].toObject();
        if (!op.contains("tool") || op["suppressed"].toBool())
            continue;
        auto operand = object(s, op["tool"].toString());
        if (operand.isEmpty())
            fail("Потеряно тело операции");
        auto tr = pose(obj).Inverted() * pose(operand);
        QJsonArray basis;
        for (int j = 1; j <= 3; ++j)
            basis.append(vec(tr.Value(j, 1), tr.Value(j, 2), tr.Value(j, 3)));
        op["basis"] = basis;
        op["offset"] = vec(tr.Value(1, 4), tr.Value(2, 4), tr.Value(3, 4));
        op["operand"] = resolved(s, operand, visiting);
        a[i] = op;
    }
    auto result = obj;
    result["operations"] = a;
    return result;
}
double indexOf(const QJsonObject &s, const QJsonObject &p) {
    return s["materials"]
        .toObject()[p["material"].toString()]
        .toObject()["indices"]
        .toObject()[p["line"].toString()]
        .toDouble();
}
static const QRegularExpression assetPattern("^[a-f0-9]{64}\\.[a-z0-9]{1,8}$");
QString assetFile(const QString &folder, const QString &key) {
    if (!assetPattern.match(key).hasMatch())
        fail("Некорректное имя вложения");
    return QDir(folder).filePath(key);
}
static QSet<QString> keys(const QJsonObject &s) {
    QSet<QString> result;
    for (auto v : s["objects"].toArray())
        for (auto k : {"asset", "image", "profile"})
            if (v.toObject().contains(k))
                result.insert(v.toObject()[k].toString());
    if (s.contains("turntable"))
        result.insert(s["turntable"].toObject()["asset"].toString());
    return result;
}
QString storeAsset(const QString &path, const QString &folder) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f))
        fail(f.errorString());
    auto key = QString::fromLatin1(hash.result().toHex()) + "." + QFileInfo(path).suffix().toLower();
    auto dest = assetFile(folder, key);
    if (!QFile::exists(dest) && !f.copy(dest))
        fail(f.errorString());
    return key;
}
void validateDocument(const QJsonObject &scene) {
    if (!scene["objects"].isArray() || !scene["materials"].isObject() ||
        (scene["version"].toInt() != 1 && scene["version"].toInt() != 2))
        fail("Некорректный формат проекта");
    auto materials = scene["materials"].toObject();
    for (auto v : materials) {
        auto m = v.toObject(), indices = m["indices"].toObject();
        if (m["name"].toString().trimmed().isEmpty() || !indices.contains(m["default"].toString()) ||
            (!m["density"].isNull() && !(m["density"].toDouble() > 0 && m["density"].toDouble() < 100)))
            fail("Некорректная карточка материала");
        for (auto i = indices.begin(); i != indices.end(); ++i)
            if (!(i.value().toDouble() > 1 && i.value().toDouble() <= 6) ||
                (i.key() != "nominal" && !(i.key().toDouble() > 0 && i.key().toDouble() < 100000)))
                fail("Проверьте оптические характеристики материала");
    }
    QSet<QString> ids;
    QJsonArray screens;
    for (auto v : scene["objects"].toArray()) {
        auto o = v.toObject();
        QString k = o["kind"].toString(), id = o["id"].toString();
        if (!labels.contains(k) || id.isEmpty() || ids.contains(id) ||
            o["name"].toString().trimmed().isEmpty())
            fail("Некорректный объект проекта");
        ids.insert(id);
        for (auto key : {"pos", "rot"}) {
            auto a = o[key].toArray();
            if (a.size() != 3)
                fail("Некорректная система координат");
            for (auto x : a)
                if (!x.isDouble() || !std::isfinite(x.toDouble()))
                    fail("Некорректная координата");
        }
        auto dims = newBody(k)["dims"].toObject();
        for (auto dim = dims.begin(); dim != dims.end(); ++dim)
            if (!(o["dims"].toObject()[dim.key()].toDouble() > 0))
                fail("Размер должен быть положительным");
        if (solid(k) && !(indexOf(scene, o) > 1))
            fail("Материал тела отсутствует");
        if ((k == "mesh" || k == "cad") && !o.contains("asset"))
            fail("Отсутствует файл тела");
        if (k == "screen") {
            screens.append(o);
            if (o.contains("image_fit") && o["image_fit"] != "fit" && o["image_fit"] != "crop")
                fail("Некорректное размещение изображения");
        }
    }
    for (auto v : scene["objects"].toArray()) {
        auto o = v.toObject();
        QSet<QString> trail;
        for (auto parent = o; parent.contains("parent");) {
            auto p = parent["parent"].toString();
            if (!ids.contains(p) || trail.contains(p) || parent["locked"].toBool())
                fail("Некорректная связь тел");
            trail.insert(p);
            parent = object(scene, p);
        }
        if (o.contains("operations"))
            resolved(scene, o);
    }
    for (auto key : keys(scene))
        if (!assetPattern.match(key).hasMatch())
            fail("Некорректный путь вложения");
    if (scene.contains("turntable")) {
        auto turn = scene["turntable"].toObject();
        if (screens.size() != 1 || screens.at(0).toObject()["id"] != turn["screen"] ||
            screens.at(0).toObject().contains("image"))
            fail("Серия видов должна быть назначена одному экрану");
        QSet<double> seen;
        auto angles = turn["angles"].toArray();
        if (angles.isEmpty() || angles.size() > 32)
            fail("Требуется от 1 до 32 углов");
        for (auto v : angles) {
            auto a = std::fmod(std::fmod(v.toDouble(), 360.) + 360., 360.);
            if (!v.isDouble() || seen.contains(a))
                fail("Некорректные углы поворота");
            seen.insert(a);
        }
    }
}
QJsonObject loadProject(const QString &path, const QString &folder) {
    using Reader = std::unique_ptr<archive, decltype(&archive_read_free)>;
    Reader ar(archive_read_new(), archive_read_free);
    archive_read_support_format_zip(ar.get());
    if (archive_read_open_filename_w(ar.get(), path.toStdWString().c_str(), 65536) != ARCHIVE_OK)
        fail("Ошибка открытия CCAD");
    QJsonObject scene;
    QSet<QString> found;
    archive_entry *e = nullptr;
    qsizetype total = 0;
    int status;
    while ((status = archive_read_next_header(ar.get(), &e)) == ARCHIVE_OK) {
        auto name = QString::fromUtf8(archive_entry_pathname(e));
        bool manifest = name == "scene.json";
        QString key = name.mid(7);
        if (!manifest && (!name.startsWith("assets/") || !assetPattern.match(key).hasMatch())) {
            archive_read_data_skip(ar.get());
            continue;
        }
        if (found.contains(name))
            fail("Повторяющееся вложение");
        found.insert(name);
        QByteArray json;
        QSaveFile asset(manifest ? QString() : assetFile(folder, key));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!manifest && !asset.open(QIODevice::WriteOnly))
            fail(asset.errorString());
        char block[65536];
        la_ssize_t n;
        while ((n = archive_read_data(ar.get(), block, sizeof(block))) > 0) {
            total += n;
            if (total > 1024LL * 1024 * 1024)
                fail("Проект превышает 1 ГиБ");
            if (manifest) {
                if (json.size() + n > 8 * 1024 * 1024)
                    fail("Слишком большое описание сцены");
                json.append(block, n);
            } else {
                if (asset.write(block, n) != n)
                    fail(asset.errorString());
                hash.addData(QByteArrayView(block, n));
            }
        }
        if (n < 0)
            fail("Ошибка чтения архива");
        if (manifest)
            scene = QJsonDocument::fromJson(json).object();
        else if (hash.result().toHex() != key.left(64).toLatin1() || !asset.commit())
            fail("Ошибка контрольной суммы вложения");
    }
    if (status != ARCHIVE_EOF || !scene["objects"].isArray() ||
        (scene["version"].toInt() != 1 && scene["version"].toInt() != 2))
        fail("Некорректный проект");
    for (auto key : keys(scene))
        if (!found.contains("assets/" + key))
            fail("Отсутствует вложение");
    QSet<QString> ids;
    for (auto v : scene["objects"].toArray()) {
        auto o = v.toObject();
        auto id = o["id"].toString();
        if (id.isEmpty() || ids.contains(id) || !labels.contains(o["kind"].toString()) ||
            o["pos"].toArray().size() != 3 || o["rot"].toArray().size() != 3)
            fail("Некорректный объект сцены");
        ids.insert(id);
    }
    validateDocument(scene);
    return scene;
}
void saveProject(const QJsonObject &s, const QString &folder, const QString &path) {
    validateDocument(s);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        fail(out.errorString());
    std::unique_ptr<archive, decltype(&archive_write_free)> ar(archive_write_new(), archive_write_free);
    archive_write_set_format_zip(ar.get());
    auto sink = [](archive *, void *client, const void *data, size_t n) -> la_ssize_t {
        return static_cast<QIODevice *>(client)->write(static_cast<const char *>(data), n);
    };
    if (archive_write_open(ar.get(), &out, nullptr, sink, nullptr) != ARCHIVE_OK)
        fail("Ошибка создания архива");
    auto header = [&](const QByteArray &name, qint64 size) {
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> e(archive_entry_new(),
                                                                        archive_entry_free);
        archive_entry_set_pathname(e.get(), name.constData());
        archive_entry_set_size(e.get(), size);
        archive_entry_set_filetype(e.get(), AE_IFREG);
        archive_entry_set_perm(e.get(), 0644);
        if (archive_write_header(ar.get(), e.get()) != ARCHIVE_OK)
            fail("Ошибка записи архива");
    };
    auto data = [&](const QByteArray &bytes) {
        if (archive_write_data(ar.get(), bytes.constData(), bytes.size()) != bytes.size())
            fail("Ошибка записи проекта");
    };
    auto json = QJsonDocument(s).toJson(QJsonDocument::Compact);
    header("scene.json", json.size());
    data(json);
    for (auto key : keys(s)) {
        QFile f(assetFile(folder, key));
        if (!f.open(QIODevice::ReadOnly))
            fail(f.errorString());
        header(("assets/" + key).toUtf8(), f.size());
        while (!f.atEnd()) {
            auto chunk = f.read(65536);
            if (chunk.isEmpty())
                fail(f.errorString());
            data(chunk);
        }
    }
    if (archive_write_close(ar.get()) != ARCHIVE_OK || !out.commit())
        fail("Не удалось сохранить проект");
}
} // namespace raysculpt
