#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {
class ProjectWindow final : public QMainWindow {
    QString worker, source, verifyOutput;
    QJsonObject scene;
    QProcess process;
    QPushButton *open, *save;
    QComboBox *object, *material, *line;
    QLabel *index, *density;
    bool busy = false;

    void controls() {
        open->setEnabled(!busy);
        save->setEnabled(!busy && !source.isEmpty() && object->currentIndex() >= 0 &&
                         material->currentIndex() >= 0 && line->currentIndex() >= 0);
        object->setEnabled(!busy && object->count()!=0);
        material->setEnabled(!busy && material->count()!=0);
        line->setEnabled(!busy && line->count()!=0);
    }

    QJsonObject currentObject() const {
        const auto id=object->currentData().toString();
        for (const auto value : scene["objects"].toArray()) {
            const auto item=value.toObject();
            if (item["id"].toString()==id) return item;
        }
        return {};
    }

    void materialLines() {
        const QSignalBlocker block(line);
        const auto key=material->currentData().toString();
        const auto card=scene["materials"].toObject()[key].toObject();
        const auto values=card["indices"].toObject();
        line->clear();
        for (auto i=values.constBegin();i!=values.constEnd();++i) {
            const auto label=i.key().contains(QChar('.')) ? i.key()+QStringLiteral(" нм") : i.key();
            line->addItem(label,i.key());
        }
        const auto current=currentObject();
        const auto defaultLine=current["material"].toString()==key
            ? current["line"].toString() : card["default"].toString();
        const auto selected=line->findData(defaultLine);
        if (selected>=0) line->setCurrentIndex(selected);
        describe();
    }

    void describe() {
        const auto card=scene["materials"].toObject()[material->currentData().toString()].toObject();
        const auto value=card["indices"].toObject()[line->currentData().toString()];
        index->setText(value.isDouble() ? QString::number(value.toDouble(),'g',8) : QStringLiteral("—"));
        const auto rho=card["density"];
        density->setText(rho.isDouble() ? QString::number(rho.toDouble())+QStringLiteral(" г/см³")
                                     : QStringLiteral("—"));
        controls();
    }

    void chooseObject() {
        const QSignalBlocker block(material);
        material->setCurrentIndex(material->findData(currentObject()["material"].toString()));
        materialLines();
    }

    void command(QStringList arguments,QByteArray input,
                 std::function<void(QByteArray)> completed) {
        if (busy) return;
        busy=true;
        controls();
        connect(&process,&QProcess::finished,this,
            [this,completed=std::move(completed)](int exit,QProcess::ExitStatus status) {
                const auto stdoutData=process.readAllStandardOutput();
                const auto error=QString::fromUtf8(process.readAllStandardError()).trimmed();
                disconnect(&process,nullptr,this,nullptr);
                busy=false;
                controls();
                if (exit!=0 || status!=QProcess::NormalExit) {
                    statusBar()->showMessage(error);
                    if (!verifyOutput.isEmpty()) QApplication::exit(1);
                    return;
                }
                completed(stdoutData);
            });
        connect(&process,&QProcess::started,this,[this,input=std::move(input)] {
            if (!input.isEmpty()) process.write(input);
            process.closeWriteChannel();
        });
        connect(&process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
            if (error!=QProcess::FailedToStart) return;
            disconnect(&process,nullptr,this,nullptr);
            busy=false;
            controls();
            statusBar()->showMessage(process.errorString());
            if (!verifyOutput.isEmpty()) QApplication::exit(1);
        });
        process.start(worker,arguments);
    }

    void saveAs(const QString& path) {
        if (path.isEmpty()) return;
        const QJsonObject edit{{"object",object->currentData().toString()},
                              {"material",material->currentData().toString()},
                              {"line",line->currentData().toString()}};
        statusBar()->showMessage(QStringLiteral("Запись копии проекта…"));
        command({"project-save-as",source,path},QJsonDocument(edit).toJson(QJsonDocument::Compact),
                [this](const QByteArray&) {
                    statusBar()->showMessage(QStringLiteral("Копия проекта сохранена"));
                    if (!verifyOutput.isEmpty()) QApplication::exit();
                });
    }

public:
    explicit ProjectWindow(QString executable) : worker(std::move(executable)) {
        setWindowTitle(QStringLiteral("RaySculpt — материалы проекта"));
        resize(565,300);
        auto panel=new QWidget(this);
        auto layout=new QVBoxLayout(panel);
        auto actions=new QHBoxLayout;
        open=new QPushButton(QStringLiteral("Открыть CCAD…"),panel);
        save=new QPushButton(QStringLiteral("Сохранить копию…"),panel);
        actions->addWidget(open);
        actions->addWidget(save);
        actions->addStretch();
        layout->addLayout(actions);
        auto form=new QFormLayout;
        object=new QComboBox(panel);
        material=new QComboBox(panel);
        line=new QComboBox(panel);
        index=new QLabel(panel);
        density=new QLabel(panel);
        form->addRow(QStringLiteral("Тело"),object);
        form->addRow(QStringLiteral("Материал"),material);
        form->addRow(QStringLiteral("Длина волны"),line);
        form->addRow(QStringLiteral("Показатель преломления n"),index);
        form->addRow(QStringLiteral("Плотность"),density);
        layout->addLayout(form);
        layout->addStretch();
        setCentralWidget(panel);
        statusBar()->showMessage(QStringLiteral("Откройте проект, чтобы проверить или изменить материал"));
        connect(open,&QPushButton::clicked,this,[this] {
            const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("Открыть проект"),{},
                                                       QStringLiteral("Проект RaySculpt (*.ccad)"));
            if (!path.isEmpty()) load(path);
        });
        connect(save,&QPushButton::clicked,this,[this] {
            auto path=QFileDialog::getSaveFileName(this,QStringLiteral("Сохранить копию"),{},
                                                   QStringLiteral("Проект RaySculpt (*.ccad)"));
            if (!path.isEmpty() && !path.endsWith(".ccad",Qt::CaseInsensitive)) path+=".ccad";
            saveAs(path);
        });
        connect(object,&QComboBox::currentIndexChanged,this,[this] { chooseObject(); });
        connect(material,&QComboBox::currentIndexChanged,this,[this] { materialLines(); });
        connect(line,&QComboBox::currentIndexChanged,this,[this] { describe(); });
        controls();
    }

    void load(const QString& path) {
        statusBar()->showMessage(QStringLiteral("Чтение и проверка проекта…"));
        command({"project-read",path},{},[this,path](const QByteArray& result) {
            const auto document=QJsonDocument::fromJson(result);
            if (!document.isObject()) {
                statusBar()->showMessage(QStringLiteral("Рабочий процесс вернул некорректный проект"));
                if (!verifyOutput.isEmpty()) QApplication::exit(1);
                return;
            }
            scene=document.object();
            source=path;
            const auto cards=scene["materials"].toObject();
            const auto bodies=scene["objects"].toArray();
            {
                const QSignalBlocker bodyEvents(object), cardEvents(material);
                object->clear();
                material->clear();
                for (auto card=cards.constBegin();card!=cards.constEnd();++card)
                    material->addItem(card.value().toObject()["name"].toString(),card.key());
                for (const auto entry : bodies) {
                    const auto item=entry.toObject();
                    if (item["material"].isString())
                        object->addItem(item["name"].toString(),item["id"].toString());
                }
            }
            chooseObject();
            statusBar()->showMessage(QStringLiteral("Проект открыт. Выберите материал и сохраните копию."));
            if (!verifyOutput.isEmpty()) {
                const auto testMaterial=material->findData(QStringLiteral("bk7"));
                if (testMaterial<0 || object->count()==0) { QApplication::exit(1); return; }
                material->setCurrentIndex(testMaterial);
                saveAs(verifyOutput);
            }
        });
    }

    void roundTrip(const QString& source,const QString& output) {
        verifyOutput=output;
        load(source);
    }
};
}

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    const auto args=app.arguments();
    if (args.size()!=2 && args.size()!=3 && args.size()!=5)
        qFatal("Usage: raysculpt_project_editor WORKER [PROJECT | --check ORIGINAL COPY]");
    ProjectWindow window(args[1]);
    window.show();
    if (args.size()==3) window.load(args[2]);
    if (args.size()==5 && args[2]=="--check") {
        window.roundTrip(args[3],args[4]);
        QTimer::singleShot(30000,&app,[] { QApplication::exit(2); });
    }
    return app.exec();
}
