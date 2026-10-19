#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <functional>

namespace CustomSources {
// Definitions stay on the PC. Only typed output rows and action names are shared.
QString validate(const QJsonObject &definition);
QJsonObject run(const QJsonObject &definition, const std::function<bool()> &cancelled);
QJsonArray readings(const QJsonObject &definition, const QJsonObject &result);
} // namespace CustomSources
