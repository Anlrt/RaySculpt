#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QString>
#include <QSize>
#include <gp_Trsf.hxx>
#include <TopoDS_Shape.hxx>
#include <filesystem>
namespace raysculpt {
inline constexpr auto idField = "id";
bool solid(const QString &kind);
QString label(const QString &kind);
QJsonObject newBody(const QString &kind);
QJsonObject newDocument();
void validateDocument(const QJsonObject &);
void storeMaterial(const QString &key, const QJsonObject &card);
QJsonObject object(const QJsonObject &scene, const QString &id);
void put(QJsonObject &scene, const QJsonObject &obj);
QSet<QString> consumed(const QJsonObject &scene);
gp_Trsf pose(const QJsonObject &item);
void assignPose(QJsonObject &obj, const gp_Trsf &tr);
void moveBody(QJsonObject &scene, const QString &id, const gp_Trsf &position);
QJsonObject resolved(const QJsonObject &scene, const QJsonObject &obj, QSet<QString> visiting = {});
QJsonObject loadProject(const QString &path, const QString &assets);
void saveProject(const QJsonObject &scene, const QString &assets, const QString &path);
QString storeAsset(const QString &path, const QString &assets);
QStringList diagnostics(const QJsonObject &scene);
QByteArray traceSettings(const QJsonObject &scene, int resolution, int rays);
TopoDS_Shape build_body(const QJsonObject &, const std::filesystem::path &);
void write_solid(const TopoDS_Shape &, const std::filesystem::path &, bool);
QString assetFile(const QString &folder, const QString &key);
QJsonArray vec(double x, double y, double z);
double indexOf(const QJsonObject &scene, const QJsonObject &plate);
void fail(const QString &message);
} // namespace raysculpt
