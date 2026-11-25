#pragma once
#include "geometry.hpp"
#include <QWidget>
#include <functional>
#include <QMap>
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_Manipulator.hxx>
#include <V3d_View.hxx>
namespace raysculpt {
class View : public QWidget {
    occ::handle<V3d_Viewer> viewer;
    occ::handle<V3d_View> camera;
    occ::handle<AIS_InteractiveContext> context;
    occ::handle<AIS_Manipulator> manipulator;
    struct Entry {
        double volume = 0;
        bool issue = false, overlayVisible = false;
        QByteArray geometry;
        occ::handle<AIS_Shape> item;
        occ::handle<AIS_InteractiveObject> relief;
    };
    QMap<QString, Entry> entries;
    QJsonObject current;
    QString folder, selected;
    QPoint last, press;
    QString planeDrag;
    gp_Trsf dragStart;
    bool moveMode = false, dragging = false, grid = true, relief = false;
    TopAbs_ShapeEnum selection = TopAbs_SHAPE;
    void initialize();
    void redrawManipulator();
    void showEvent(QShowEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    QPaintEngine *paintEngine() const override { return nullptr; }

  public:
    explicit View(QWidget *parent = nullptr);
    ~View() override;
    void sync(const QJsonObject &scene, const QString &assets);
    QSet<QString> issueObjects;
    void select(const QString &id);
    void fit(bool selectedOnly = false);
    void direction(V3d_TypeOfOrientation orientation);
    void setMove(bool enabled);
    void setGrid(bool enabled);
    void setRelief(bool enabled);
    void pickType(TopAbs_ShapeEnum type);
    double volume(const QString &id) const { return entries.value(id).volume; }
    QString gpuInfo() const;
    void orbit(double radians) { camera->Rotate(0., radians, 0.); }
    bool snapshot(const QString &file) { return camera->Dump(file.toUtf8().constData()); }
    std::function<void(QString, TopoDS_Shape, gp_Pnt, TopAbs_ShapeEnum)> picked;
    std::function<void(QString, gp_Trsf)> moved;
};
} // namespace raysculpt
