#pragma once
#include "document.hpp"
#include "optics.hpp"
#include "../geometry.hpp"
namespace raysculpt {
struct OpticalProfile {
    caustic::HeightField plate;
    double source, screen, width, height;
};
OpticalProfile profile(const QString &file);
QJsonArray inspectScene(const QJsonObject &scene, const QString &assets, bool opticsOnly = false);
TopoDS_Shape splineSolid(const caustic::HeightField &plate);
caustic::HeightField designProfile(const QString &source, QString &previous);
void writeModels(const QString &source, const QString &directory, const QStringList &formats, int cells);
void exportPlate(const QStringList &arguments);
caustic::Mesh surfaceMesh(const caustic::HeightField &plate, int cells);
caustic::Mesh meshFile(const QString &path);
caustic::Mesh shapeMesh(const TopoDS_Shape &shape);
void exportMesh(const caustic::Mesh &mesh, const QString &path, const gp_Trsf &pose);
TopoDS_Shape displayShape(const QJsonObject &obj, const QString &folder);
} // namespace raysculpt
