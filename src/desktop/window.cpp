#include "window.hpp"
#include "jobs.hpp"
#include "topology.hpp"
#include <QtWidgets>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <QElapsedTimer>
#include <algorithm>

namespace raysculpt {
Window::Window() : scene(newDocument()), saved(scene) {
    resize(1380, 850);
    tabs = new QTabWidget(this);
    view = new View(tabs);
    tabs->addTab(view, "Модель");
    setCentralWidget(tabs);
    auto page = new QWidget;
    auto panel = new QVBoxLayout(page);
    results = new QComboBox;
    panel->addWidget(results);
    auto columns = new QHBoxLayout;
    panel->addLayout(columns, 1);
    for (auto name : {"Цель", "Проекция", "Ошибка яркости"}) {
        auto column = new QVBoxLayout;
        columns->addLayout(column, 1);
        column->addWidget(new QLabel(name));
        auto pane = new QGraphicsView;
        pane->setScene(new QGraphicsScene(pane));
        pane->setDragMode(QGraphicsView::ScrollHandDrag);
        pane->viewport()->installEventFilter(this);
        pane->setRenderHint(QPainter::SmoothPixmapTransform);
        pane->setBackgroundBrush(QColor("#dee5e8"));
        panes << pane;
        column->addWidget(pane);
    }
    auto controls = new QHBoxLayout;
    panel->addLayout(controls);
    for (auto factor : {.8, 1., 1.25}) {
        auto b = new QPushButton(factor == 1 ? "Вписать" : factor < 1 ? "−" : "+");
        controls->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, factor] {
            for (auto p : panes)
                if (factor == 1)
                    p->fitInView(p->sceneRect(), Qt::KeepAspectRatio);
                else
                    p->scale(factor, factor);
        });
    }
    auto download = new QPushButton("Сохранить проекцию…");
    controls->addWidget(download);
    connect(download, &QPushButton::clicked, this, [this] {
        guard([&] {
            if (results->currentIndex() < 0)
                return;
            auto dest = QFileDialog::getSaveFileName(this, "Сохранить проекцию", {}, "PNG (*.png)");
            if (!dest.isEmpty() &&
                !QImage(frames.at(results->currentIndex()).toObject()["image"].toString()).save(dest))
                fail("Ошибка записи изображения");
        });
    });
    synchronizeViews();
    tabs->addTab(page, "Проекция");
    auto dock = [&](QString name, QWidget *widget, Qt::DockWidgetArea side) {
        auto d = new QDockWidget(name, this);
        d->setWidget(widget);
        addDockWidget(side, d);
        return d;
    };
    tree = new QTreeWidget;
    tree->setHeaderHidden(true);
    dock("Дерево модели", tree, Qt::LeftDockWidgetArea);
    auto area = new QScrollArea;
    area->setWidgetResizable(true);
    auto form = new QWidget;
    fields = new QFormLayout(form);
    area->setWidget(form);
    area->setMinimumWidth(265);
    dock("Параметры объекта", area, Qt::RightDockWidgetArea);
    log = new QPlainTextEdit;
    log->setReadOnly(true);
    log->setMaximumBlockCount(500);
    auto journal = dock("Сообщения", log, Qt::BottomDockWidgetArea);
    journal->hide();
    state = new QLabel;
    statusBar()->addWidget(state, 1);
    statusBar()->addPermanentWidget(new QLabel("мм    °"));
    auto file = menuBar()->addMenu("Файл"), edit = menuBar()->addMenu("Правка"),
         model = menuBar()->addMenu("Модель"), optic = menuBar()->addMenu("Оптика"),
         display = menuBar()->addMenu("Вид");
    auto quick = addToolBar("Команды");
    auto optical = addToolBar("Оптика");
    insertToolBarBreak(optical);
    optical->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    using Fn = std::function<void()>;
    auto action = [&](QMenu *menu, QString title, Fn fn, QString key = {}, QString shortcut = {},
                      QToolBar *bar = nullptr) {
        auto a = menu->addAction(title);
        if (!key.isEmpty())
            a->setIcon(QIcon(":/icons/" + key + ".svg"));
        a->setShortcut(QKeySequence(shortcut));
        connect(a, &QAction::triggered, this, [this, fn] { guard(fn); });
        if (bar)
            bar->addAction(a);
        return a;
    };
    action(
        file, "Создать",
        [this] {
            if (!readyToClose())
                return;
            scene = newDocument();
            saved = scene;
            anchorReference = {};
            selectedReference = {};
            feature.Nullify();
            previousFeature.Nullify();
            selected = {};
            filename.clear();
            frames = {};
            undoStack.clear();
            redoStack.clear();
            refresh();
            view->fit();
        },
        "new", "Ctrl+N", quick);
    action(
        file, "Открыть…",
        [this] {
            auto path = QFileDialog::getOpenFileName(this, "Открыть проект", {}, "RaySculpt (*.ccad)");
            if (!path.isEmpty() && readyToClose())
                open(path);
        },
        "open", "Ctrl+O", quick);
    action(file, "Сохранить", [this] { save(false); }, "save", "Ctrl+S", quick);
    action(file, "Сохранить как…", [this] { save(true); }, {}, "Ctrl+Shift+S");
    action(file, "Открыть расчёт по профилю…", [this] { loadProfile(); });
    action(
        file, "Импорт модели…",
        [this] {
            auto f = QFileDialog::getOpenFileName(this, "Импорт модели", {},
                                                  "Модель (*.step *.stp *.brep *.stl *.obj)");
            if (!f.isEmpty())
                add(f.endsWith(".obj", Qt::CaseInsensitive) || f.endsWith(".stl", Qt::CaseInsensitive)
                        ? "mesh"
                        : "cad",
                    f);
        },
        "import");
    action(file, "Экспорт выбранного тела…", [this] { exportBody(); }, "export");
    action(file, "Сохранить журнал…", [this] {
        auto path = QFileDialog::getSaveFileName(this, "Журнал", {}, "Текст (*.txt)");
        if (path.isEmpty())
            return;
        QSaveFile f(path);
        auto data = log->toPlainText().toUtf8();
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
            fail(f.errorString());
    });
    undoAction = action(
        edit, "Отменить",
        [this] {
            redoStack << scene;
            scene = undoStack.takeLast();
            refresh();
        },
        "undo", "Ctrl+Z", quick);
    redoAction = action(
        edit, "Повторить",
        [this] {
            undoStack << scene;
            scene = redoStack.takeLast();
            refresh();
        },
        "redo", "Ctrl+Y", quick);
    action(
        edit, "Удалить выбранный объект",
        [this] {
            auto s = scene;
            auto target = object(s, selected);
            if (target.isEmpty())
                return;
            const auto row = tree->currentItem();
            const auto step = row ? row->data(0, Qt::UserRole + 1).toString() : QString{};
            if (!step.isEmpty()) {
                if (step == "operation") {
                    auto steps = target["operations"].toArray();
                    steps.removeAt(row->data(0, Qt::UserRole + 2).toInt());
                    target["operations"] = steps;
                } else if (step == "mate") {
                    target.remove("parent");
                    target.remove("constraint");
                } else if (step == "cut")
                    target.remove("cut");
                else
                    fail("Базовое построение удаляется вместе с телом");
                put(s, target);
                commit(s);
                return;
            }
            if (target["locked"].toBool())
                fail("Сначала снимите фиксацию объекта");
            auto a = s["objects"].toArray();
            for (int i = 0; i < a.size(); ++i) {
                auto o = a[i].toObject();
                for (auto op : o["operations"].toArray())
                    if (op.toObject()["tool"] == selected)
                        fail("Тело используется в операции");
                if (o["parent"] == selected) {
                    o.remove("parent");
                    o.remove("constraint");
                    a[i] = o;
                }
            }
            for (int i = 0; i < a.size(); ++i)
                if (a[i].toObject()["id"] == selected) {
                    a.removeAt(i);
                    break;
                }
            s["objects"] = a;
            if (s["turntable"].toObject()["screen"] == selected)
                s.remove("turntable");
            commit(s);
        },
        {}, "Delete");
    for (auto k : {"plate", "box", "sphere", "cylinder", "screen", "point", "area", "sun"})
        action(model, label(k), [this, k] { add(k); });
    action(model, "Срез параллельно XY…", [this] {
        auto o = object(scene, selected);
        if (!QStringList{"box", "cylinder", "sphere", "prism", "cad"}.contains(o["kind"].toString()))
            fail("Выберите тело для среза");
        bool ok;
        auto z = QInputDialog::getDouble(this, "Срез", "Сохранить часть ниже локальной Z, мм",
                                         o["cut"].toDouble(o["dims"].toObject()["h"].toDouble() / 2), -1e6,
                                         1e6, 4, &ok);
        if (ok) {
            o["cut"] = z;
            auto s = scene;
            put(s, o);
            commit(s);
        }
    });
    action(model, "Убрать срез XY", [this] {
        auto o = object(scene, selected);
        if (!o.contains("cut"))
            return;
        o.remove("cut");
        auto s = scene;
        put(s, o);
        commit(s);
    });
    action(optic, "Выровнять по оси пластины", [this] {
        auto o = object(scene, selected);
        if (!QStringList{"sun", "area", "point", "screen"}.contains(o["kind"].toString()))
            fail("Выберите экран или источник");
        for (auto v : scene["objects"].toArray()) {
            auto p = v.toObject();
            if (p["kind"] != "plate" || p["reference"].toBool())
                continue;
            auto base = pose(p);
            auto d = (base.Inverted() * pose(o)).TranslationPart().Modulus();
            double z =
                o["kind"] == "screen"
                    ? std::max(d, p["dims"].toObject()["h"].toDouble() + p["max_relief"].toDouble() + 1)
                    : -std::max(1., d);
            gp_Trsf t;
            t.SetTranslation(gp_Vec(0, 0, z));
            auto s = scene;
            moveBody(s, selected, base * t);
            commit(s);
            return;
        }
        fail("Добавьте оптическую пластину");
    });
    action(model, "Эскиз и выдавливание…", [this] { sketch(); });
    action(model, "Сечение плоскостью…", [this] { section(); });
    action(model, "Булева операция…", [this] { booleanOp(); });
    action(model, "История операций…", [this] { history(); });
    action(model, "Сопряжение выбранных элементов…", [this] { mateElements(); }, "snap");
    action(model, "Связать положение…", [this] { mate(); }, "snap");
    action(model, "Измерить…", [this] { measure(); }, "measure");
    action(model, "Проверить геометрию", [this] { inspect(); }, "inspect");
    auto addButton = new QToolButton(quick);
    addButton->setText("Добавить тело");
    addButton->setIcon(QIcon(":/icons/solid.svg"));
    addButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    addButton->setPopupMode(QToolButton::InstantPopup);
    auto choices = new QMenu(addButton);
    for (auto k : {"plate", "box", "sphere", "cylinder"})
        action(choices, label(k), [this, k] { add(k); });
    action(choices, "Эскиз и выдавливание…", [this] { sketch(); });
    addButton->setMenu(choices);
    quick->addWidget(addButton);
    auto dragging = action(
        model, "Переместить / повернуть",
        [this] { view->setMove(static_cast<QAction *>(sender())->isChecked()); }, "move", "M", quick);
    dragging->setCheckable(true);
    auto sources = new QToolButton(optical);
    sources->setText("Источник");
    sources->setIcon(QIcon(":/icons/light.svg"));
    sources->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    sources->setPopupMode(QToolButton::InstantPopup);
    auto lights = new QMenu(sources);
    for (auto k : {"point", "area", "sun"})
        action(lights, label(k), [this, k] { add(k, {}, true); });
    sources->setMenu(lights);
    optical->addWidget(sources);
    action(optic, "Добавить экран", [this] { add("screen"); }, "screen", {}, optical);
    action(optic, "Назначить изображение…", [this] { chooseImage(); }, "target", {}, optical);
    action(optic, "Виды 3D-объекта…", [this] { turntable(); });
    action(optic, "Убрать 3D-цель", [this] {
        auto s = scene;
        s.remove("turntable");
        commit(s);
    });
    action(optic, "Параметры трассировки…", [this] {
        bool ok;
        auto count =
            QInputDialog::getInt(this, "Трассировка", "Число лучей", traceRays, 1000, 200000000, 1000, &ok);
        if (!ok)
            return;
        auto size = QInputDialog::getInt(this, "Трассировка", "Длинная сторона изображения, пикс.",
                                         traceResolution, 16, 1024, 1, &ok);
        if (ok) {
            traceRays = count;
            traceResolution = size;
        }
    });
    action(optic, "Материалы…", [this] { material(); }, "library");
    action(optic, "Подобрать расстояние…", [this] { placement(); }, "placement");
    designAction = action(optic, "Построить рельеф", [this] { run(true); }, "design", {}, optical);
    traceAction = action(optic, "Проверить проекцию", [this] { run(false); }, "trace", "F5", optical);
    cancelAction = action(optic, "Остановить", [this] { job.kill(); }, "cancel", {}, optical);
    for (auto item : QList<QPair<QString, V3d_TypeOfOrientation>>{{"Изометрия", V3d_XposYnegZpos},
                                                                  {"Спереди", V3d_Yneg},
                                                                  {"Сверху", V3d_Zpos},
                                                                  {"Справа", V3d_Xpos}})
        action(display, item.first, [this, item] { view->direction(item.second); });
    action(display, "Показать всё", [this] { view->fit(); }, {}, "F");
    action(display, "Приблизить выбранное", [this] { view->fit(true); }, {}, "Shift+F");
    auto grid =
        action(display, "Сетка", [this] { view->setGrid(static_cast<QAction *>(sender())->isChecked()); });
    grid->setCheckable(true);
    grid->setChecked(true);
    auto relief = action(display, "Окраска высоты рельефа",
                         [this] { view->setRelief(static_cast<QAction *>(sender())->isChecked()); });
    relief->setCheckable(true);
    for (auto item : QList<QPair<QString, TopAbs_ShapeEnum>>{{"Выбирать тела", TopAbs_SHAPE},
                                                             {"Выбирать грани", TopAbs_FACE},
                                                             {"Выбирать рёбра", TopAbs_EDGE},
                                                             {"Выбирать вершины", TopAbs_VERTEX}})
        action(display, item.first, [this, item] { view->pickType(item.second); });
    display->addAction(journal->toggleViewAction());
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *row, int) {
        if (!row)
            return;
        auto type = row->data(0, Qt::UserRole + 1).toString();
        guard([&] {
            if (type == "operation")
                history();
            else if (type == "sketch")
                sketch();
            else if (type == "mate") {
                if (object(scene, selected).contains("constraint"))
                    mateElements();
                else
                    mate();
            } else if (type == "cut") {
                auto o = object(scene, selected);
                bool ok;
                auto cut = QInputDialog::getDouble(this, "Срез", "Координата Z, мм", o["cut"].toDouble(),
                                                   -1e6, 1e6, 4, &ok);
                if (ok) {
                    o["cut"] = cut;
                    auto s = scene;
                    put(s, o);
                    commit(s);
                }
            }
        });
    });
    connect(tree, &QTreeWidget::currentItemChanged, this, [this](auto row, auto) {
        if (!updating && row)
            select(row->data(0, Qt::UserRole).toString());
    });
    connect(results, &QComboBox::currentIndexChanged, this, [this] { compare(); });
    view->picked = [this](QString id, TopoDS_Shape shape, gp_Pnt point, TopAbs_ShapeEnum type) {
        guard([&] {
            auto ref = pickReference(resolved(scene, object(scene, id)), assets.path(), shape, type, point);
            anchorReference = selectedReference;
            selectedReference = ref;
            previousFeature = feature;
            previousFeatureObject = featureObject;
            feature = element(scene, ref, assets.path()).Moved(TopLoc_Location(pose(object(scene, id))));
            featureObject = id;
            select(id);
        });
    };
    view->moved = [this](QString id, gp_Trsf tr) {
        guard([&] {
            auto s = scene;
            moveBody(s, id, tr);
            commit(s);
        });
    };
    connect(&job, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) { guard([&] { finish(code); }); });
    connect(&job, &QProcess::errorOccurred, this, [this](auto code) {
        if (code == QProcess::FailedToStart) {
            log->appendPlainText(job.errorString());
            pendingDir.reset();
            refresh();
        }
    });
    QTimer::singleShot(0, this, [this] {
        guard([&] {
            refresh();
            view->fit();
        });
    });
}
void Window::guard(const std::function<void()> &work) {
    QString message;
    try {
        work();
        return;
    } catch (const Standard_Failure &e) {
        message = QString::fromUtf8(e.GetMessageString());
    } catch (const std::exception &e) {
        message = QString::fromUtf8(e.what());
    }
    log->appendPlainText(message);
    QMessageBox::warning(this, "RaySculpt", message);
}
void Window::open(const QString &path) {
    auto loaded = loadProject(path, assets.path());
    rebuildRelations(loaded, assets.path());
    scene = loaded;
    saved = scene;
    anchorReference = {};
    selectedReference = {};
    feature.Nullify();
    previousFeature.Nullify();
    filename = path;
    frames = {};
    selected.clear();
    undoStack.clear();
    redoStack.clear();
    refresh();
    view->fit();
}
bool Window::save(bool as) {
    QString path = filename;
    if (as || path.isEmpty())
        path = QFileDialog::getSaveFileName(this, "Сохранить проект", path, "RaySculpt (*.ccad)");
    if (path.isEmpty())
        return false;
    saveProject(scene, assets.path(), path);
    filename = path;
    saved = scene;
    refresh();
    return true;
}
bool Window::readyToClose() {
    if (job.state() != QProcess::NotRunning) {
        QMessageBox::information(this, "Расчёт", "Сначала остановите текущий расчёт.");
        return false;
    }
    if (saved == scene)
        return true;
    auto choice = QMessageBox::question(this, "Проект", "Сохранить изменения?",
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    return choice == QMessageBox::Discard || (choice == QMessageBox::Save && save(false));
}
void Window::closeEvent(QCloseEvent *e) {
    e->ignore();
    guard([&] {
        if (readyToClose())
            e->accept();
    });
}
void Window::commit(QJsonObject next) {
    if (next == scene)
        return;
    validateDocument(next);
    rebuildRelations(next, assets.path());
    auto old = scene;
    scene = next;
    try {
        refresh();
    } catch (...) {
        scene = old;
        refresh();
        throw;
    }
    undoStack << old;
    if (undoStack.size() > 80)
        undoStack.removeFirst();
    redoStack.clear();
    undoAction->setEnabled(true);
    redoAction->setEnabled(false);
}
void Window::select(const QString &id) {
    selected = id;
    QSignalBlocker b(tree);
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto row = tree->topLevelItem(i);
        if (row->data(0, Qt::UserRole) == id &&
            (!tree->currentItem() || tree->currentItem()->data(0, Qt::UserRole) != id))
            tree->setCurrentItem(row);
    }
    view->select(id);
    properties();
}
void Window::refresh() {
    QScopedValueRollback block(updating, true);
    tree->clear();
    auto items = scene["objects"].toArray();
    if (object(scene, selected).isEmpty())
        selected = items.at(0).toObject()["id"].toString();
    bool target = true;
    for (auto v : items) {
        auto o = v.toObject();
        QString k = o["kind"].toString();
        auto row = new QTreeWidgetItem(tree, {o["name"].toString()});
        row->setIcon(0, QIcon(":/icons/" +
                              QString(k == "screen" ? "screen"
                                      : solid(k)    ? "solid"
                                                    : "light") +
                              ".svg"));
        row->setData(0, Qt::UserRole, o["id"].toString());
        auto step = [&](QString name, QString field, int index = -1) {
            auto child = new QTreeWidgetItem(row, {name});
            child->setData(0, Qt::UserRole, o["id"].toString());
            child->setData(0, Qt::UserRole + 1, field);
            child->setData(0, Qt::UserRole + 2, index);
        };
        if (k == "prism")
            step("Эскиз", "sketch");
        if (o.contains("cut"))
            step("Срез XY", "cut");
        if (o.contains("parent"))
            step("Связь с опорой", "mate");
        int seq = 0;
        for (auto entry : o["operations"].toArray()) {
            auto op = entry.toObject();
            static const QMap<QString, QString> types{{"section", "Сечение"},
                                                      {"subtract", "Вычитание"},
                                                      {"union", "Объединение"},
                                                      {"intersect", "Пересечение"}};
            step(types.value(op["kind"].toString()) + (op["suppressed"].toBool() ? " (выключена)" : ""),
                 "operation", seq++);
        }

        if (o["id"] == selected)
            tree->setCurrentItem(row);
        if (k == "screen" && !o.contains("image"))
            target = false;
    }
    bool idle = job.state() == QProcess::NotRunning;
    auto issues = diagnostics(scene);
    bool ready = issues.isEmpty();
    target |= scene.contains("turntable");
    traceAction->setEnabled(idle && ready);
    designAction->setEnabled(idle && ready && target);
    cancelAction->setEnabled(!idle);
    undoAction->setEnabled(!undoStack.isEmpty());
    redoAction->setEnabled(!redoStack.isEmpty());
    state->setText(!idle     ? "Идёт расчёт…"
                   : !ready  ? issues.join(" · ")
                   : !target ? "Назначьте изображение экрану"
                   : !frames.isEmpty() && opticalSignature() != signature
                       ? "Проекция рассчитана для прежних параметров"
                       : "Сцена готова к расчёту");
    setWindowTitle((filename.isEmpty() ? "Новый проект" : QFileInfo(filename).fileName()) +
                   QString(saved != scene ? " *" : "") + " — RaySculpt");
    view->issueObjects.clear();
    if (scene == inspectedScene) {
        for (const auto value : inspectionRows) {
            auto issue = value.toObject();
            for (const auto id : issue["objects"].toArray()) {
                view->issueObjects.insert(id.toString());
                for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                    auto row = tree->topLevelItem(i);
                    if (row->data(0, Qt::UserRole) == id.toString()) {
                        row->setForeground(0, QColor("#b52729"));
                        row->setToolTip(0, issue["message"].toString());
                    }
                }
            }
        }
    }
    if (!issues.isEmpty()) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto row = tree->topLevelItem(i);
            for (auto message : issues)
                if (message.contains(row->text(0), Qt::CaseInsensitive)) {
                    view->issueObjects.insert(row->data(0, Qt::UserRole).toString());
                    row->setForeground(0, QColor("#b52729"));
                    row->setToolTip(0, message);
                    break;
                }
        }
    }
    view->sync(scene, assets.path());
    view->select(selected);
    properties();
    if (frames.isEmpty() && results->count())
        results->clear();
}
void Window::add(QString k, QString path, bool replaceLight) {
    if (scene.contains("turntable") && k == "screen")
        fail("Сначала уберите цель с серией видов");
    auto s = scene;
    auto o = newBody(k);
    QJsonObject plate, lamp;
    int count = 0;
    for (auto item : scene["objects"].toArray()) {
        auto existing = item.toObject();
        const auto kind = existing["kind"].toString();
        if (kind == k)
            ++count;
        if (kind == "plate" && plate.isEmpty() && !existing["reference"].toBool())
            plate = existing;
        if (QStringList{"point", "area", "sun"}.contains(kind) &&
            (lamp.isEmpty() || existing["id"] == selected))
            lamp = existing;
    }
    if (replaceLight && !lamp.isEmpty()) {
        if (lamp["name"] == label(lamp["kind"].toString()))
            lamp["name"] = label(k);
        lamp["kind"] = k;
        lamp["dims"] = o["dims"];
        o = lamp;
    } else {
        if (count)
            o["name"] = label(k) + " " + QString::number(count + 1);
        if (!path.isEmpty()) {
            o["asset"] = storeAsset(path, assets.path());
            o["name"] = QFileInfo(path).completeBaseName();
        } else if (!plate.isEmpty()) {
            if (solid(k)) {
                gp_Pnt origin(45. * (count + 1), 0, 0);
                origin.Transform(pose(plate));
                o["pos"] = vec(origin.X(), origin.Y(), origin.Z());
            } else {
                gp_Trsf offset;
                offset.SetTranslation(k == "screen" ? gp_Vec(65. * count, 0, 125) : gp_Vec(0, 0, -240));
                assignPose(o, pose(plate) * offset);
            }
        }
    }
    selected = o["id"].toString();
    put(s, o);
    commit(s);
    tabs->setCurrentIndex(0);
    view->fit();
}
bool Window::eventFilter(QObject *obj, QEvent *event) {
    if (event->type() == QEvent::Wheel || event->type() == QEvent::MouseButtonDblClick) {
        for (auto pane : panes)
            if (pane->viewport() == obj) {
                if (event->type() == QEvent::MouseButtonDblClick) {
                    for (auto p : panes)
                        p->fitInView(p->sceneRect(), Qt::KeepAspectRatio);
                    return true;
                }
                auto wheel = static_cast<QWheelEvent *>(event);
                auto anchor = pane->mapToScene(wheel->position().toPoint());
                auto center = pane->mapToScene(pane->viewport()->rect().center());
                double factor = std::pow(1.2, wheel->angleDelta().y() / 120.);
                for (auto p : panes) {
                    p->scale(factor, factor);
                    p->centerOn(anchor + (center - anchor) / factor);
                }
                return true;
            }
    }
    return QMainWindow::eventFilter(obj, event);
}
void Window::synchronizeViews() {
    for (auto p : panes) {
        for (auto axis : {Qt::Horizontal, Qt::Vertical}) {
            auto bar = axis == Qt::Horizontal ? p->horizontalScrollBar() : p->verticalScrollBar();
            connect(bar, &QScrollBar::valueChanged, this, [this, p, axis](int v) {
                for (auto other : panes)
                    if (other != p) {
                        auto b = axis == Qt::Horizontal ? other->horizontalScrollBar()
                                                        : other->verticalScrollBar();
                        QSignalBlocker mute(b);
                        b->setValue(v);
                    }
            });
        }
    }
}
} // namespace raysculpt
