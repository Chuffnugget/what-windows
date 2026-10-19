#pragma once
#include <QJsonObject>
#include <QStringList>

namespace WallpaperEngine {
QString discover(const QString &overridePath = {});
bool running(const QString &executable);
QString validate(const QJsonObject &args, bool checkFiles = true);
QStringList arguments(const QJsonObject &args);
QJsonObject execute(const QString &executable, const QJsonObject &args);
QJsonObject inspect(const QString &overridePath, bool queryWallpaper);
} // namespace WallpaperEngine
