#pragma once
#include <QString>
#include <QVariantMap>
QVariantMap discoverInstalledFiles(QString appPath, QString driverPath, bool search);
bool verifiedDriverFile(const QString &path);
// Unrecognised builds are probed for the current native ABI before live checks.
bool compatibleBeacnAppFile(const QString &path, QString *reason = nullptr);
QString beacnFileVersion(const QString &path);

// Stage the exact bundled helper outside WindowsApps for the external BEACN loader.
QString prepareBeacnHook(const QString &source, const QString &directory, QString *error);
