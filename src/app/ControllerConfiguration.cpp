#include "app/SettingsStore.h"
#include "Controller.h"
#include "ConfigurationFile.h"
#include <QFileDialog>
#include <QFile>
#include <QFutureWatcher>
#include <QtConcurrent>

void Controller::exportConfiguration() {
    if (configurationBusy)
        return;
    const auto path = QFileDialog::getSaveFileName(nullptr, "Export WHAT configuration", "WHAT-config.yaml",
                                                   "WHAT configuration (*.yaml)");
    if (path.isEmpty())
        return;
    settings.sync();
    const auto profilePath = settings.fileName();
    configurationBusy = true;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const auto error = watcher->result();
        configurationBusy = false;
        emit configurationResult(error.isEmpty() ? "Configuration exported. Keep the file private: it "
                                                   "includes sensor names and connection addresses."
                                                 : error);
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path, profilePath] {
        QSettings profile(profilePath, SettingsStore::format());
        const auto data = ConfigurationFile::encode(ConfigurationFile::snapshot(profile));
        if (profile.status() != QSettings::NoError)
            return QString("Could not read the current configuration.");
        return ConfigurationFile::write(path, data);
    }));
}

void Controller::previewConfiguration() {
    if (configurationBusy)
        return;
    configurationDraft = {};
    const auto path = QFileDialog::getOpenFileName(nullptr, "Import WHAT configuration", {},
                                                   "WHAT configuration (*.yaml *.yml)");
    if (path.isEmpty())
        return;
    configurationBusy = true;
    auto *watcher = new QFutureWatcher<ConfigurationFile::Result>(this);
    connect(watcher, &QFutureWatcher<ConfigurationFile::Result>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        configurationBusy = false;
        if (!result.error.isEmpty())
            emit configurationResult(result.error);
        else {
            configurationDraft = result.values;
            emit configurationPreview(result.preview);
        }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path] { return ConfigurationFile::read(path); }));
}

void Controller::stageConfiguration(bool includePermissions) {
    if (configurationBusy || configurationDraft.isEmpty())
        return;
    auto values = configurationDraft;
    for (auto it = values.begin(); it != values.end();) {
        if (!includePermissions && ConfigurationFile::permission(it.key()))
            it = values.erase(it);
        else
            ++it;
    }
    if (values.isEmpty()) {
        emit configurationResult("No settings selected for import.");
        return;
    }
    configurationBusy = true;
    // Startup applies this file before creating modules. Applying here would mix new
    // permissions with already-running workers and leave the live profile half changed.
    const auto path = settings.fileName() + ".import.yaml";
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const auto error = watcher->result();
        configurationBusy = false;
        if (error.isEmpty())
            configurationDraft = {};
        emit configurationResult(error.isEmpty()
                                     ? "Import queued. Quit WHAT from the tray and reopen it to apply. "
                                       "Changes to these settings before restarting will be replaced."
                                     : error);
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(
        [path, values] { return ConfigurationFile::write(path, ConfigurationFile::encode(values)); }));
}

void Controller::cancelConfigurationImport() {
    if (configurationBusy)
        return;
    configurationDraft = {};
    const auto path = settings.fileName() + ".import.yaml";
    emit configurationResult(!QFile::exists(path) || QFile::remove(path)
                                 ? "Pending import cancelled."
                                 : "Could not remove the pending import.");
}
