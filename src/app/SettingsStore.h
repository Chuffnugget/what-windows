#pragma once
#include <QSettings>

namespace SettingsStore {
QSettings::Format format();
// Prepare the YAML profile before any settings consumers or workers are started.
QString profilePath(const QString &directory, bool testing = false);
} // namespace SettingsStore
