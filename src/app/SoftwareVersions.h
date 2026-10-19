#pragma once
#include <QJsonObject>
#include <QMap>
#include <QString>

namespace SoftwareVersions {
// Call from a diagnostics worker. Reads file metadata without loading vendor DLLs.
QJsonObject installed(const QMap<QString, QString> &knownPaths);
} // namespace SoftwareVersions
