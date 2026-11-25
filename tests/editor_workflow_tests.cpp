#include "../src/desktop/window.hpp"
#include <QtWidgets>
#include <QTest>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace raysculpt;
static void require(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
}
static QAction *action(QWidget &window, const QString &title) {
    for (auto a : window.findChildren<QAction *>())
        if (a->text() == title)
            return a;
    throw std::runtime_error(("Missing action: " + title).toStdString());
}
static QTreeWidgetItem *named(QTreeWidget &tree, const QString &title) {
    auto rows = tree.findItems(title, Qt::MatchExactly | Qt::MatchRecursive);
    require(rows.size() == 1, ("Missing or duplicate tree item: " + title).toStdString().c_str());
    return rows.first();
}
int main(int argc, char **argv) {
    const bool hardware = argc > 1 && QString::fromLocal8Bit(argv[1]) == "--gpu";
    if (!hardware)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    try {
        QTemporaryDir tmp;
        require(tmp.isValid(), "Test directory");
        Window window;
        if (!hardware)
            window.findChild<QWidget *>("cadViewport")->hide();
        window.show();
        window.activateWindow();
        QTest::qWait(100);
        auto tree = window.findChild<QTreeWidget *>("modelTree");
        auto results = window.findChild<QComboBox *>("projectionScreens");
        require(tree && results && tree->topLevelItemCount() == 3, "Initial tree");

        QString image, unexpected;
        QString phase;
        int elapsedTicks = 0;
        bool approveDelete = false;
        QTimer dialogs;
        QObject::connect(&dialogs, &QTimer::timeout, [&] {
            if (++elapsedTicks > 2500) {
                std::cerr << "UI timeout in " << phase.toStdString() << std::endl;
                std::exit(2);
            }
            auto modal = app.activeModalWidget();
            if (elapsedTicks % 120 == 0)
                std::cerr << "Waiting for " << (modal ? modal->windowTitle().toStdString() : "no dialog")
                          << std::endl;
            if (!modal)
                for (QWidget *top : app.topLevelWidgets())
                    if (qobject_cast<QDialog *>(top) && top->isVisible()) {
                        modal = top;
                        break;
                    }
            if (auto f = qobject_cast<QFileDialog *>(modal)) {
                if (!f->property("inputSelected").toBool()) {
                    f->setProperty("inputSelected", true);
                    f->setDirectory(QFileInfo(image).absolutePath());
                    f->selectFile(QFileInfo(image).fileName());
                    if (auto edit = f->findChild<QLineEdit *>("fileNameEdit"))
                        edit->setText(image);
                    std::cerr << "File dialog: " << f->selectedFiles().join(",").toStdString() << std::endl;
                }
                if (auto edit = f->findChild<QLineEdit *>("fileNameEdit")) {
                    edit->setText(image);
                    QTest::keyClick(edit, Qt::Key_Return);
                } else
                    QMetaObject::invokeMethod(f, "accept");
            } else if (auto input = qobject_cast<QInputDialog *>(modal)) {
                if (!input->comboBoxItems().isEmpty())
                    input->setTextValue(input->comboBoxItems().first());
                input->accept();
            } else if (auto message = qobject_cast<QMessageBox *>(modal)) {
                if (approveDelete && message->button(QMessageBox::Yes))
                    message->button(QMessageBox::Yes)->click();
                else {
                    unexpected = message->text();
                    message->reject();
                }
            } else if (auto dialog = qobject_cast<QDialog *>(modal))
                dialog->accept();
        });
        dialogs.start(30);
        auto invoke = [&](QString title) {
            phase = title;
            std::cerr << title.toStdString() << std::endl;
            auto a = action(window, title);
            require(a->isEnabled(), ("Disabled: " + title).toStdString().c_str());
            a->trigger();
            require(unexpected.isEmpty(), unexpected.toStdString().c_str());
        };
        // Use actual tree input, including a child feature and its parent.
        invoke(label("box"));
        auto box = named(*tree, label("box"));
        tree->setCurrentItem(box);
        invoke("Срез параллельно XY…");
        box = named(*tree, label("box"));
        box->setExpanded(true);
        tree->setCurrentItem(named(*tree, "Срез XY"));
        tree->setFocus();
        QTest::keyClick(tree, Qt::Key_Delete);
        require(named(*tree, label("box"))->childCount() == 0, "Delete a cut without deleting the body");
        invoke("Отменить");
        box = named(*tree, label("box"));
        require(box->childCount() == 1, "Undo deletion of cut");
        tree->setCurrentItem(box);
        QTest::keyClick(tree, Qt::Key_Delete);
        require(unexpected.isEmpty(), unexpected.toStdString().c_str());
        require(tree->topLevelItemCount() == 3, "Delete a body using the tree");
        invoke("Отменить");
        require(tree->topLevelItemCount() == 4, "Undo deletion of body");
        // Exercise the right-click menu for the clicked row even with another body selected.
        box = named(*tree, label("box"));
        tree->setCurrentItem(named(*tree, label("plate")));
        auto where = tree->visualItemRect(box).center();
        bool contextDelete = false;
        phase = "context menu";
        std::cerr << "Context menu" << std::endl;
        QTimer menuDriver;
        QObject::connect(&menuDriver, &QTimer::timeout, [&] {
            for (QWidget *popup : app.topLevelWidgets()) {
                auto menu = qobject_cast<QMenu *>(popup);
                if (!menu || !menu->isVisible())
                    continue;
                for (auto a : menu->actions())
                    if (a->text().startsWith("Удалить объект")) {
                        menuDriver.stop();
                        a->trigger();
                        contextDelete = true;
                        break;
                    }
                menu->close();
                return;
            }
        });
        menuDriver.start(30);
        QMetaObject::invokeMethod(tree, "customContextMenuRequested", Q_ARG(QPoint, where));
        require(contextDelete && tree->topLevelItemCount() == 3, "Delete body from context menu");
        invoke("Добавить экран");
        require(tree->topLevelItemCount() == 4, "Add second screen");
        for (int i = 0; i < 2; i++) {
            auto title = label("screen") + (i ? " 2" : "");
            tree->setCurrentItem(named(*tree, title));
            image = tmp.filePath(QString("target-%1.png").arg(i));
            QImage goal(96, 96, QImage::Format_RGB32);
            for (int y = 0; y < goal.height(); ++y)
                for (int x = 0; x < goal.width(); ++x) {
                    auto dx = (x - 47.5) / 25, dy = (y - (i ? 25. : 70.)) / 17;
                    int val = qRound(80 + 165 * std::exp(-dx * dx - dy * dy));
                    goal.setPixel(x, y, qRgb(val, val, val));
                }
            require(goal.save(image), "Write goal");
            invoke("Назначить изображение…");
        }
        QCheckBox *lock = nullptr;
        require(QTest::qWaitFor(
                    [&] {
                        for (auto box : window.findChildren<QCheckBox *>("property.locked"))
                            if (box->isVisible()) {
                                lock = box;
                                return true;
                            }
                        return false;
                    },
                    2000),
                "Missing position lock");
        lock->click();
        approveDelete = true;
        tree->setFocus();
        QTest::keyClick(tree, Qt::Key_Delete);
        approveDelete = false;
        require(tree->topLevelItemCount() == 3, "Delete a fixed receiver with confirmation");
        invoke("Отменить");
        require(tree->topLevelItemCount() == 4, "Restore the fixed receiver and its image");
        invoke("Построить рельеф");
        require(QTest::qWaitFor([&] { return results->count() == 2 || !unexpected.isEmpty(); }, 95000),
                "Two-screen UI timeout");
        require(unexpected.isEmpty(), unexpected.toStdString().c_str());
        require(results->itemText(0) != results->itemText(1), "Distinct receiver labels");
        // Verify spatial content of the displayed projections, including vertical orientation.
        auto panes = window.findChildren<QGraphicsView *>();
        require(panes.size() == 3, "Comparison panes");
        for (int i = 0; i < 2; ++i) {
            results->setCurrentIndex(i);
            auto items = panes[1]->scene()->items();
            require(items.size() == 1, "Projection missing in comparison");
            auto img = static_cast<QGraphicsPixmapItem *>(items.first())->pixmap().toImage();
            double lower = 0, upper = 0;
            for (int y = img.height() / 8; y < 7 * img.height() / 8; y++)
                for (int x = img.width() / 6; x < 5 * img.width() / 6; x++)
                    (y >= img.height() / 2 ? lower : upper) += qGray(img.pixel(x, y));
            require(i ? upper > lower * 1.1 : lower > upper * 1.1,
                    "Projection is blank, swapped or vertically inverted");
        }
        std::cout << "Verified tree deletion and undo, right-click targeting, target assignment, synthesis "
                     "and two distinct displayed projections.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
