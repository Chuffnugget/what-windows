#pragma once
#include <QJsonObject>
#include <QJsonArray>

namespace LightingLink {
QJsonArray effects(const QJsonObject &reply);
QJsonObject state(const QJsonObject &reply, const QJsonArray &effects, const QString &error = {});
QJsonArray readings(const QJsonObject &state);
QString validate(const QJsonObject &args, const QJsonObject &state);
QJsonObject activation(const QJsonObject &args, const QJsonObject &state);
bool matches(const QJsonObject &payload, const QJsonObject &state);
} // namespace LightingLink
