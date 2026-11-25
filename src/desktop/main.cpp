#include "window.hpp"
#include "jobs.hpp"
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QStyleHints>
#include <QStyle>
#include <QMessageBox>
#include <QTimer>

int main(int argc, char **argv) {
    auto resources = QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteDir().filePath("occt-resources");
    if (QDir(resources).exists()) {
        qputenv("CSF_OCCTResourcePath", resources.toUtf8());
        qputenv("CSF_ShadersDirectory", (resources + "/Shaders").toUtf8());
    }
    if (argc > 1 && QByteArray(argv[1]) == "--roundtrip") {
        QCoreApplication app(argc, argv);
        auto args = app.arguments();
        if (args.size() != 4 || QFileInfo::exists(args[3]))
            return 2;
        try {
            QTemporaryDir dir;
            auto scene = raysculpt::loadProject(args[2], dir.path());
            raysculpt::saveProject(scene, dir.path(), args[3]);
            return 0;
        } catch (...) {
            return 1;
        }
    }
    if (argc > 1 && QByteArray(argv[1]) == "--job") {
        QCoreApplication app(argc, argv);
        const auto args = app.arguments();
        if (args.size() != 4)
            return 2;
        QJsonObject result;
        try {
            QFile f(args[2]);
            if (!f.open(QIODevice::ReadOnly))
                raysculpt::fail(f.errorString());
            result = raysculpt::solve(QJsonDocument::fromJson(f.readAll()).object());
        } catch (const Standard_Failure &e) {
            result["error"] = QString::fromUtf8(e.GetMessageString());
        } catch (const std::exception &e) {
            result["error"] = QString::fromUtf8(e.what());
        }
        QSaveFile out(args[3]);
        auto bytes = QJsonDocument(result).toJson();
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
            return 3;
        return result.contains("error") ? 1 : 0;
    }
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
    QApplication::setPalette(app.style()->standardPalette());
    try {
        raysculpt::Window window;
        auto args = app.arguments();
        window.show();
        if (args.size() > 1)
            window.open(args[1]);
        if (args.size() == 4 && args[2] == "--viewport-benchmark")
            QTimer::singleShot(250, &window, [&] { window.viewportBenchmark(args[3]); });
        return app.exec();
    } catch (const Standard_Failure &e) {
        QMessageBox::critical(nullptr, "RaySculpt", QString::fromUtf8(e.GetMessageString()));
    } catch (const std::exception &e) {
        QMessageBox::critical(nullptr, "RaySculpt", QString::fromUtf8(e.what()));
    }
    return 1;
}
