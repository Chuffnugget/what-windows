#pragma once
#include <QJsonObject>
#include <QJsonArray>

namespace Displays {
// Layouts store logical monitor identities, never boot-specific adapter LUIDs.
QJsonObject query(bool includeSpecifications = false);
QJsonArray layout(const QJsonObject &state);
QString validateLayout(const QJsonArray &layout);
QJsonObject edit(const QJsonObject &state, const QJsonObject &args);
QString apply(const QJsonArray &layout, bool validateOnly = false);
bool matches(const QJsonArray &wanted, const QJsonArray &actual);
QJsonArray readings(const QJsonObject &state);
} // namespace Displays
