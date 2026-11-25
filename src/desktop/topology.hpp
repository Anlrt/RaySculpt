#pragma once
#include "geometry.hpp"
namespace raysculpt {
QJsonObject pickReference(const QJsonObject &body, const QString &assets, const TopoDS_Shape &localShape,
                          TopAbs_ShapeEnum type, const gp_Pnt &point);
TopoDS_Shape element(const QJsonObject &scene, const QJsonObject &ref, const QString &assets);
gp_Trsf localFrame(const QJsonObject &scene, const QJsonObject &ref, const QString &assets);
void rebuildRelations(QJsonObject &scene, const QString &assets);
} // namespace raysculpt
