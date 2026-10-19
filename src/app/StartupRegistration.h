#pragma once
#include <QSettings>
#include <QString>
#include <optional>

namespace StartupRegistration {
struct State {
    bool enabled = false;
    QString error;
};

bool supported();
bool isWhatCommand(const QString &command, const QString &family);
// Run on a worker. An absent preference reads the state and migrates the old Run entry.
State update(QSettings &run, const QString &executable, const QString &family,
             std::optional<bool> enabled = {});
} // namespace StartupRegistration
