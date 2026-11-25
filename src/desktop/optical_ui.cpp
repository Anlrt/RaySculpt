#include "window.hpp"
#include "jobs.hpp"
#include <QtWidgets>
#include <QImageReader>
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>

namespace raysculpt {
QByteArray Window::opticalSignature() const {
    QJsonArray objects;
    auto used = consumed(scene);
    for (auto v : scene["objects"].toArray()) {
        auto o = v.toObject();
        if (used.contains(o["id"].toString()) || o["reference"].toBool())
            continue;
        QJsonObject row;
        for (auto key :
             {"id", "kind", "pos", "rot", "dims", "profile", "image", "image_fit", "cut", "operations"})
            if (o.contains(key))
                row[key] = o[key];
        if (solid(o["kind"].toString()))
            row["n"] = indexOf(scene, o);
        objects.append(row);
    }
    return QCryptographicHash::hash(
        QJsonDocument(QJsonArray{objects, scene.value("turntable")}).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
}
void Window::loadProfile() {
    auto file = QFileDialog::getOpenFileName(this, "Расчётная поверхность", {}, "Профиль (*.txt)");
    if (file.isEmpty() || !readyToClose())
        return;
    auto p = profile(file);
    auto s = newDocument();
    auto a = s["objects"].toArray();
    auto plate = a.at(0).toObject(), src = a.at(1).toObject(), screen = a.at(2).toObject();
    plate["profile"] = storeAsset(file, assets.path());
    auto &h = p.plate;
    auto mm = std::minmax_element(h.offsets_mm.begin(), h.offsets_mm.end());
    plate["min_relief"] = *mm.first;
    plate["max_relief"] = *mm.second;
    plate["design_n"] = h.refractive_index;
    plate["dims"] = QJsonObject{{"w", h.width_mm}, {"h", h.thickness_mm}};
    auto catalog = s["materials"].toObject();
    bool found = false;
    for (auto it = catalog.begin(); it != catalog.end() && !found; ++it) {
        auto indices = it.value().toObject()["indices"].toObject();
        for (auto j = indices.begin(); j != indices.end(); ++j)
            if (std::abs(j.value().toDouble() - h.refractive_index) < 1e-8) {
                plate["material"] = it.key();
                plate["line"] = j.key();
                found = true;
                break;
            }
    }
    if (!found) {
        QString key = QUuid::createUuid().toString(QUuid::Id128);
        plate["material"] = key;
        plate["line"] = "nominal";
        catalog[key] = QJsonObject{{"name", QString("Из профиля · n %1").arg(h.refractive_index)},
                                   {"density", QJsonValue::Null},
                                   {"default", "nominal"},
                                   {"indices", QJsonObject{{"nominal", h.refractive_index}}},
                                   {"source", ""}};
        s["materials"] = catalog;
    }
    gp_Trsf t;
    t.SetTranslation(gp_Vec(0, 0, -p.source));
    assignPose(src, pose(plate) * t);
    t.SetTranslation(gp_Vec(0, 0, p.screen));
    assignPose(screen, pose(plate) * t);
    screen["dims"] = QJsonObject{{"w", p.width}, {"d", p.height}};
    for (auto o : {plate, src, screen})
        put(s, o);
    scene = s;
    filename.clear();
    saved = {};
    frames = {};
    anchorReference = {};
    selectedReference = {};
    undoStack.clear();
    redoStack.clear();
    selected = plate["id"].toString();
    refresh();
    view->fit();
}
void Window::chooseImage() {
    if (scene.contains("turntable"))
        fail("Сначала уберите 3D-цель");
    auto screen = object(scene, selected);
    if (screen["kind"] != "screen") {
        QList<QJsonObject> choices;
        for (auto v : scene["objects"].toArray())
            if (v.toObject()["kind"] == "screen")
                choices << v.toObject();
        if (choices.isEmpty())
            fail("Добавьте экран");
        if (choices.size() == 1)
            screen = choices.front();
        else {
            QStringList names;
            int firstFree = -1;
            for (const auto &o : choices) {
                if (!o.contains("image") && firstFree < 0)
                    firstFree = names.size();
                names << QString("%1. %2%3")
                             .arg(names.size() + 1)
                             .arg(o["name"].toString(), o.contains("image") ? " · цель назначена" : "");
            }
            bool ok = false;
            auto choice = QInputDialog::getItem(this, "Назначение изображения", "Экран", names,
                                                std::max(0, firstFree), false, &ok);
            if (!ok)
                return;
            screen = choices.at(names.indexOf(choice));
        }
    }
    auto file = QFileDialog::getOpenFileName(
        this, "Изображение экрана", {}, "Изображения (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.pgm)");
    if (file.isEmpty())
        return;
    QImageReader reader(file);
    reader.setAutoTransform(true);
    auto img = reader.read();
    if (img.isNull())
        fail(reader.errorString());
    QDialog dlg(this);
    dlg.setWindowTitle("Размещение изображения");
    QVBoxLayout f(&dlg);
    QComboBox mode;
    mode.addItems({"Вписать целиком", "Заполнить с обрезкой", "Изменить высоту экрана"});
    mode.setCurrentIndex(screen["image_fit"] == "crop" ? 1 : 0);
    QLabel preview, dimensions;
    preview.setAlignment(Qt::AlignCenter);
    f.addWidget(&mode);
    f.addWidget(&preview);
    f.addWidget(&dimensions);
    QJsonObject desired;
    auto update = [&] {
        desired = screen;
        auto dims = screen["dims"].toObject();
        if (mode.currentIndex() == 2)
            dims["d"] = dims["w"].toDouble() * img.height() / img.width();
        desired["dims"] = dims;
        desired["image_fit"] = mode.currentIndex() == 1 ? "crop" : "fit";
        preview.setPixmap(QPixmap::fromImage(goalPreview(file, desired, 280)));
        dimensions.setText(QString("%1 × %2 мм").arg(dims["w"].toDouble()).arg(dims["d"].toDouble()));
    };
    update();
    connect(&mode, &QComboBox::currentIndexChanged, &dlg, update);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f.addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    desired["image"] = storeAsset(file, assets.path());
    auto s = scene;
    put(s, desired);
    selected = desired["id"].toString();
    commit(s);
}
void Window::turntable() {
    QList<QJsonObject> screens;
    for (auto v : scene["objects"].toArray())
        if (v.toObject()["kind"] == "screen")
            screens << v.toObject();
    if (screens.size() != 1)
        fail("Для серии видов нужен один экран");
    auto s = scene;
    auto turn = s["turntable"].toObject();
    if (turn.isEmpty()) {
        auto file = QFileDialog::getOpenFileName(this, "Модель для серии видов", {},
                                                 "Модель (*.obj *.stl *.brep *.step *.stp)");
        if (file.isEmpty())
            return;
        turn["asset"] = storeAsset(file, assets.path());
        turn["name"] = QFileInfo(file).fileName();
        turn["screen"] = screens[0]["id"];
    }
    QStringList previous;
    for (auto v : turn["angles"].toArray())
        previous << QString::number(v.toDouble());
    bool ok;
    auto text =
        QInputDialog::getText(this, "Серия видов", "Углы вращения вокруг Y, ° через пробел",
                              QLineEdit::Normal, previous.isEmpty() ? "-12 0 12" : previous.join(" "), &ok);
    if (!ok)
        return;
    QJsonArray angles;
    QSet<double> seen;
    for (auto val : text.simplified().split(' ', Qt::SkipEmptyParts)) {
        double angle = val.replace(',', '.').toDouble(&ok);
        double unique = std::fmod(std::fmod(angle, 360.) + 360., 360.);
        if (!ok || !std::isfinite(angle) || seen.contains(unique))
            fail("Требуются различные конечные углы");
        seen.insert(unique);
        angles.append(angle);
    }
    if (angles.isEmpty() || angles.size() > 32)
        fail("Укажите от 1 до 32 углов");
    turn["angles"] = angles;
    s["turntable"] = turn;
    auto o = screens[0];
    o.remove("image");
    o.remove("image_fit");
    put(s, o);
    commit(s);
}
void Window::run(bool design) {
    if (job.state() != QProcess::NotRunning)
        return;
    int quality = 1;
    if (design) {
        bool ok;
        QStringList labels{"Эскиз · 96 пикселей", "Стандарт · 256 пикселей", "Детально · 448 пикселей"};
        auto name = QInputDialog::getItem(this, "Построение рельефа", "Точность", labels, 1, false, &ok);
        if (!ok)
            return;
        quality = labels.indexOf(name);
    }
    launch(QJsonObject{{"task", design ? "design" : "trace"},
                       {"quality", quality},
                       {"rays", traceRays},
                       {"resolution", traceResolution}});
}
void Window::launch(QJsonObject req) {
    if (job.state() != QProcess::NotRunning)
        fail("Дождитесь завершения или остановите расчёт");
    pendingDir = std::make_unique<QTemporaryDir>();
    if (!pendingDir->isValid())
        fail("Не удалось создать рабочую папку");
    pendingSignature = opticalSignature();
    req["scene"] = scene;
    req["assets"] = assets.path();
    req["out"] = pendingDir->path();
    if (req["task"] == "inspect" || req["task"] == "placement")
        pendingSignature = QJsonDocument(scene).toJson(QJsonDocument::Compact);
    QFile f(pendingDir->filePath("request.json"));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(req).toJson()) < 0)
        fail(f.errorString());
    f.close();
    job.start(QDir(QCoreApplication::applicationDirPath()).filePath("RaySculpt.exe"),
              {"--job", f.fileName(), pendingDir->filePath("answer.json")});
    refresh();
}
void Window::finish(int code) {
    if (!pendingDir)
        return;
    auto dir = std::move(pendingDir);
    refresh();
    QFile f(dir->filePath("answer.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        log->appendPlainText(code ? "Расчёт остановлен или прерван" : "Ответ расчёта отсутствует");
        return;
    }
    auto answer = QJsonDocument::fromJson(f.readAll()).object();
    if (code || answer.contains("error"))
        fail(answer["error"].toString("Ошибка расчёта"));
    if (answer.contains("exported")) {
        log->appendPlainText("Сохранена модель " + answer["exported"].toString());
        return;
    }
    if (answer.contains("issues")) {
        if (pendingSignature != QJsonDocument(scene).toJson(QJsonDocument::Compact))
            return;
        QStringList issues;
        auto rows = answer["issues"].toArray();
        inspectedScene = scene;
        inspectionRows = rows;
        refresh();
        for (auto v : rows)
            issues << v.toObject()["message"].toString();
        if (!rows.isEmpty())
            select(rows.at(0).toObject()["objects"].toArray().at(0).toString());
        QString message =
            issues.isEmpty() ? "Проверка геометрии завершена. Пересечений не обнаружено." : issues.join('\n');
        log->appendPlainText(message);
        QMessageBox::information(this, "Геометрия", message);
        return;
    }
    if (answer.contains("choices")) {
        if (pendingSignature != QJsonDocument(scene).toJson(QJsonDocument::Compact))
            return;
        auto rows = answer["choices"].toArray();
        QStringList labels;
        for (auto v : rows) {
            auto o = v.toObject();
            QString text = QString("Экран %1 мм · источник %2 мм · размытие %3 мм")
                               .arg(o["gap"].toDouble(), 0, 'g', 4)
                               .arg(o["lamp"].toDouble(), 0, 'g', 4)
                               .arg(o["blur"].toDouble(), 0, 'g', 3);
            if (o.contains("capture"))
                text += QString(" · принято %1% лучей").arg(100 * o["capture"].toDouble(), 0, 'g', 4);
            if (o.contains("error"))
                text += QString(" · ошибка %1").arg(o["error"].toDouble(), 0, 'g', 3);
            labels << text;
        }
        bool ok;
        auto option =
            QInputDialog::getItem(this, "Подбор расстояний", "Варианты установки", labels, 0, false, &ok);
        if (ok)
            commit(rows.at(labels.indexOf(option)).toObject()["scene"].toObject());
        return;
    }
    if (pendingSignature != opticalSignature()) {
        log->appendPlainText("Параметры изменились во время расчёта. Результат не применён.");
        return;
    }
    auto rows = answer["screens"].toArray();
    if (answer.contains("method"))
        log->appendPlainText(answer["method"].toString());
    if (answer.contains("warning"))
        log->appendPlainText(answer["warning"].toString());
    if (rows.isEmpty())
        fail("В расчёте отсутствуют экраны");
    for (auto v : rows)
        if (QImage(v.toObject()["image"].toString()).isNull())
            fail("Не удалось прочитать проекцию");
    if (answer.contains("profile")) {
        auto file = answer["profile"].toString();
        auto h = profile(file).plate;
        auto mm = std::minmax_element(h.offsets_mm.begin(), h.offsets_mm.end());
        auto s = scene;
        for (auto v : s["objects"].toArray()) {
            auto o = v.toObject();
            if (o["kind"] == "plate" && !o["reference"].toBool()) {
                o["profile"] = storeAsset(file, assets.path());
                o["design_n"] = h.refractive_index;
                o["min_relief"] = *mm.first;
                o["max_relief"] = *mm.second;
                put(s, o);
                break;
            }
        }
        commit(s);
    }
    frames = rows;
    resultDir = std::move(dir);
    signature = opticalSignature();
    {
        QSignalBlocker block(results);
        results->clear();
        for (auto v : rows) {
            auto row = v.toObject();
            auto title = row["name"].toString();
            if (row.contains("angle"))
                title += QString(" · %1°").arg(row["angle"].toDouble());
            if (row["intermediate"].toBool())
                title += " · промежуточный";
            results->addItem(title);
        }
    }
    for (auto v : rows) {
        auto row = v.toObject();
        QString text = row["name"].toString();
        if (row.contains("flux"))
            text += QString(" · поток %1").arg(row["flux"].toDouble(), 0, 'g', 6);
        if (row.contains("error"))
            text += QString(" · относительная ошибка %1").arg(row["error"].toDouble(), 0, 'g', 4);
        log->appendPlainText(text);
    }
    log->appendPlainText(QString("Расчёт завершён. Проекций: %1").arg(rows.size()));
    tabs->setCurrentIndex(1);
    refresh();
    compare();
}
void Window::compare() {
    for (auto p : panes)
        p->scene()->clear();
    const auto index = results->currentIndex();
    if (index < 0 || index >= frames.size())
        return;
    auto row = frames.at(index).toObject();
    if (row.isEmpty())
        return;
    QImage actual(row["image"].toString());
    actual = actual.convertToFormat(QImage::Format_Grayscale8);
    QImage target;
    if (row.contains("target"))
        target = QImage(row["target"].toString()).convertToFormat(QImage::Format_Grayscale8);
    if (!target.isNull() && target.size() != actual.size())
        fail("Размер цели и проекции различается");
    double lut[256];
    for (int i = 0; i < 256; ++i) {
        double x = i / 255.;
        lut[i] = x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4);
    }
    auto encode = [](double x) {
        x = std::clamp(x, 0., 1.);
        return qRound(255 * (x <= .0031308 ? 12.92 * x : 1.055 * std::pow(x, 1. / 2.4) - .055));
    };
    QImage delta;
    double gain = row["gain"].toDouble(1);
    if (!target.isNull()) {
        if (!row.contains("gain")) {
            double aa = 0, ab = 0;
            for (int y = 0; y < actual.height(); ++y)
                for (int x = 0; x < actual.width(); ++x) {
                    auto a = lut[actual.constScanLine(y)[x]];
                    aa += a * a;
                    ab += a * lut[target.constScanLine(y)[x]];
                }
            gain = aa ? ab / aa : 0;
        }
        delta = QImage(actual.size(), QImage::Format_RGB32);
        for (int y = 0; y < actual.height(); ++y) {
            auto a = actual.scanLine(y);
            auto b = target.constScanLine(y);
            auto dst = reinterpret_cast<QRgb *>(delta.scanLine(y));
            for (int x = 0; x < actual.width(); ++x) {
                double light = gain * lut[a[x]], d = light - lut[b[x]], t = encode(std::abs(d)) / 255.;
                a[x] = uchar(encode(light));
                QColor color = d < 0 ? QColor(33, 99, 166) : QColor(184, 68, 18);
                dst[x] = qRgb(qRound(247 + (color.red() - 247) * t), qRound(247 + (color.green() - 247) * t),
                              qRound(247 + (color.blue() - 247) * t));
            }
        }
    }
    QList<QImage> images{target, actual, delta};
    for (int i = 0; i < 3; ++i) {
        auto p = panes[i];
        p->scene()->setSceneRect(actual.rect());
        if (!images[i].isNull())
            p->scene()->addPixmap(QPixmap::fromImage(images[i]));
        p->fitInView(actual.rect(), Qt::KeepAspectRatio);
    }
}
void Window::viewportBenchmark(const QString &out) {
    view->setFixedSize(1600, 900);
    adjustSize();
    view->fit();
    auto timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    struct Stats {
        QElapsedTimer clock;
        QList<double> times;
        int count = 0;
    };
    auto stats = std::make_shared<Stats>();
    stats->clock.start();
    connect(timer, &QTimer::timeout, this, [this, timer, stats, out] {
        auto t = stats->clock.nsecsElapsed() / 1.e6;
        stats->clock.restart();
        if (stats->count == 0)
            view->snapshot(out + ".start.png");
        if (stats->count > 10)
            stats->times << t;
        if (++stats->count <= 130) {
            view->orbit(.012);
            return;
        }
        timer->stop();
        view->snapshot(out + ".end.png");
        std::sort(stats->times.begin(), stats->times.end());
        QJsonObject report{{"viewport", QJsonArray{view->width(), view->height()}},
                           {"gpu", view->gpuInfo()},
                           {"frames", stats->times.size()},
                           {"median_ms", stats->times[stats->times.size() / 2]},
                           {"p95_ms", stats->times[stats->times.size() * 95 / 100]}};
        QFile file(out);
        if (!file.open(QIODevice::WriteOnly)) {
            qApp->exit(2);
            return;
        }
        file.write(QJsonDocument(report).toJson());
        file.close();
        qApp->exit(0);
    });
    timer->start(0);
}
} // namespace raysculpt
