#pragma once
#include "view.hpp"
#include <QMainWindow>
#include <QTemporaryDir>
#include <QProcess>
#include <QTreeWidget>
#include <QFormLayout>
#include <QTabWidget>
#include <QLabel>
#include <QComboBox>
#include <QGraphicsView>
#include <QPlainTextEdit>
#include <memory>

namespace raysculpt {
class Window : public QMainWindow {
  public:
    Window();
    void open(const QString &path);
    void viewportBenchmark(const QString &output);

  protected:
    bool eventFilter(QObject *, QEvent *) override;
    void closeEvent(QCloseEvent *) override;

  private:
    View *view;
    QTreeWidget *tree;
    QFormLayout *fields;
    QTabWidget *tabs;
    QComboBox *results;
    QList<QGraphicsView *> panes;
    QPlainTextEdit *log;
    QLabel *state;
    QAction *traceAction, *designAction, *cancelAction, *undoAction, *redoAction;
    QJsonObject scene, saved, inspectedScene;
    QJsonArray inspectionRows;
    QString selected, filename;
    QList<QJsonObject> undoStack, redoStack;
    QTemporaryDir assets;
    std::unique_ptr<QTemporaryDir> resultDir, pendingDir;
    QJsonArray frames;
    QByteArray signature, pendingSignature;
    int traceRays = 2000000, traceResolution = 256;
    QJsonObject anchorReference, selectedReference;
    QString previousFeatureObject;
    TopoDS_Shape previousFeature;
    QProcess job;
    bool updating = false;
    TopoDS_Shape feature;
    QString featureObject;
    void guard(const std::function<void()> &);
    void commit(QJsonObject);
    void refresh();
    void properties();
    void select(const QString &);
    bool readyToClose();
    bool save(bool as);
    void add(QString kind, QString file = {}, bool replaceLight = false);
    void exportBody();
    void chooseImage();
    void turntable();
    void run(bool design);
    void launch(QJsonObject request);
    void finish(int code);
    void compare();
    void section();
    void booleanOp();
    void sketch();
    void mate();
    void mateElements();
    void loadProfile();
    void measure();
    void placement();
    void inspect();
    void material();
    void history();
    void synchronizeViews();
    QByteArray opticalSignature() const;
};
} // namespace raysculpt
