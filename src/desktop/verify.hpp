#pragma once
#include <QStringList>
#include <QJsonObject>
namespace raysculpt {
QJsonObject verifySolid(const QStringList &args);
QJsonObject verifyFacets(const QStringList &args, bool onExit);
} // namespace raysculpt
