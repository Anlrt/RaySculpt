#pragma once
#include "document.hpp"
#include <QImage>
namespace raysculpt {
void designPhotograph(const QStringList &arguments);
QSize pixels(const QJsonObject &screen, int resolution);
QImage goalPreview(const QString &path, const QJsonObject &screen, int resolution);
QJsonObject placementOptions(const QJsonObject &request);
QJsonObject solve(const QJsonObject &request);
} // namespace raysculpt
