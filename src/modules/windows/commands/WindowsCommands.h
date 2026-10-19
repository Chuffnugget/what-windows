#pragma once
#include <QJsonObject>
#include <QString>

namespace WindowsCommands {
QString permission(const QString &action);
QString validate(const QString &action, const QJsonObject &arguments);
// Returns an empty string on success; never invokes a shell or accepts executable paths.
QString execute(const QString &action, const QJsonObject &arguments);
QJsonObject powerState();
} // namespace WindowsCommands
