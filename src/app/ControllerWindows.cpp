#include "app/Architecture.h"
#include "app/SettingsStore.h"
#include "modules/windows/media/MediaSessions.h"
#include "app/Controller.h"
#include "app/PackageIdentity.h"
#include "modules/windows/commands/WindowsCommands.h"
#include <windows.h>
#include <wtsapi32.h>
#include <dbt.h>
#include <QtConcurrent>
#include <QApplication>
#include <QProcess>
#include <QJsonDocument>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QUrl>
#include <utility>

namespace {
QString searchText(const QVariantMap &row) {
    const auto name = row.value("name").toString();
    QString text = row.value("source").toString() + " / " + row.value("group").toString() + " " + name;
    const auto unit = row.value("unit").toString().trimmed();
    text += " " + unit;
    if (unit == QString::fromUtf8("°C") || unit == QString::fromUtf8("°F"))
        text += " temperature temp";
    if (name.contains("usage", Qt::CaseInsensitive) || name.contains("utilization", Qt::CaseInsensitive) ||
        name.contains("utilisation", Qt::CaseInsensitive) || name.contains("load", Qt::CaseInsensitive))
        text += " usage load utilization utilisation";
    return text;
}

bool matchesSearch(const QString &text, const QStringList &words) {
    for (const auto &word : words)
        if (!text.contains(word, Qt::CaseInsensitive))
            return false;
    return true;
}
} // namespace

bool Controller::startService() {
    if (server.isListening() || server.isStarting())
        return true;
    settings.setValue("service_enabled", true);
    connectionWaitingSince = QDateTime::currentMSecsSinceEpoch();
    scheduleSettingsFlush();
    if (!tlsReady) {
        serverStatus = "Preparing secure connection identity…";
        emit changed();
        return true;
    }
    const int port = qBound(1024, settings.value("port", testMode ? 18765 : 48765).toInt(), 65535);
    if (!testMode) {
        serverStatus = "Starting secure connection…";
        server.listenAsync(QHostAddress::Any, quint16(port));
        emit changed();
        return true;
    }
    if (!server.listen(testMode ? QHostAddress::LocalHost : QHostAddress::Any, quint16(port))) {
        serverStatus = QString("Could not start on port %1: %2").arg(port).arg(server.errorString());
        log(serverStatus);
        emit changed();
        return false;
    }
    activePort = port;
    serverStatus = QString("WebSocket server listening · TLS on port %1").arg(port);
    if (!testMode)
        discovery.start(deviceId, quint16(port));
    else
        discoveryStatus = "Test mode: loopback only; discovery and startup changes disabled";
    publishTimer.start();
    log(serverStatus);
    emit changed();
    return true;
}

void Controller::stopService() {
    ++*commandEpoch;
    settings.setValue("service_enabled", false);
    scheduleSettingsFlush();
    publishTimer.stop();
    server.close();
    discovery.stop();
    clientCount = 0;
    serverStatus = "WebSocket server stopped · sensors still collecting";
    discoveryStatus = "Discovery stopped";
    log(serverStatus);
    emit changed();
    emit sensorsChanged();
}

bool Controller::restartService() {
    stopService();
    return startService();
}

void Controller::selectFiltered(bool enabled) {
    // Use the filter predicates directly so collapsed rows remain included.
    const auto filters = browserFilters();
    const auto query = filters.value("query").toString().trimmed();
    const auto words = query.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const auto source = filters.value("source", "all").toString();
    for (const auto &value : allReadings()) {
        const auto row = value.toObject();
        const auto id = row["id"].toString();
        if (!readingBrowserEnabled(row))
            continue;
        if (source != "all" && !source.split(',').contains(row["source"].toString()))
            continue;
        if (filters.value("selected", false).toBool() && !selected.contains(id))
            continue;
        if (filters.value("unshared", false).toBool() && selected.contains(id))
            continue;
        if (filters.value("favourites", false).toBool() && !favourites.contains(id))
            continue;
        if (!matchesSearch(searchText(row.toVariantMap()), words))
            continue;
        if (enabled) {
            selected.insert(id);
            restoreSharingScope(id);
        } else
            selected.remove(id);
    }
    persistSelection();
}

bool Controller::moduleEnabled(const QString &module) const {
    if (!Architecture::moduleAvailable(module))
        return false;
    if (module == "custom")
        return customSources && customSources->enabled();
    if (module == "beacn")
        return settings.value("modules/beacn/enabled", false).toBool();
    if (module == "wallpaper")
        return settings.value("modules/wallpaper/enabled", false).toBool();
    if (module == "coretemp")
        return settings.value("modules/coretemp/enabled", false).toBool();
    if (module == "nvidia")
        return settings.value("modules/nvidia/enabled", false).toBool();
    if (module == "icue")
        return settings.value("modules/icue/enabled", false).toBool();
    if (module == "hwinfo")
        return settings.value("sources/hwinfo", true).toBool();
    if (module == "windows")
        return settings.value("modules/windows/enabled", true).toBool();
    return false;
}

bool Controller::readingModuleEnabled(const QJsonObject &row) const {
    if (row["source"] == "icue")
        return row["icueAutomation"].toBool() ? icueAutomationEnabled() : icueDirectEnabled();
    return moduleEnabled(row["source"].toString());
}

bool Controller::readingBrowserEnabled(const QJsonObject &row) const {
    if (!readingModuleEnabled(row))
        return false;
    if (row["source"] != "windows")
        return true;
    const auto id = row["id"].toString();
    const auto options = sourceOptions();
    if (id.startsWith("win_display_") && !displaysEnabled())
        return false;
    if (id.startsWith("win_net_") && (id.endsWith("_wifi_signal") || id.endsWith("_ssid")) &&
        !options["wifi"].toBool())
        return false;
    if (id.startsWith("win_foreground_") && !options["foreground"].toBool())
        return false;
    if (id == "win_foreground_title" && !options["window_titles"].toBool())
        return false;
    if (id.startsWith("win_microphone_") && !options["microphone"].toBool())
        return false;
    if (id.startsWith("win_events_") && !options["events"].toBool())
        return false;
    if (id.startsWith("win_events_") && id.endsWith("_message") && !options["event_messages"].toBool())
        return false;
    if (id.startsWith("win_security_") && !options["security"].toBool())
        return false;
    if (id.startsWith("win_media_") && !options["media"].toBool())
        return false;
    if ((id == "win_media_title" || id == "win_media_artist") && !options["media_titles"].toBool())
        return false;
    if (id.startsWith("win_notifications_") && !options["notifications"].toBool())
        return false;
    if ((id == "win_notifications_title" || id == "win_notifications_body") &&
        !options["notification_contents"].toBool())
        return false;
    if ((id == "win_user" || id == "win_idle" || id == "win_locked" || id == "win_remote") &&
        !options["session"].toBool())
        return false;
    return true;
}

void Controller::setModuleEnabled(const QString &module, bool enabled) {
    if (!Architecture::moduleAvailable(module))
        return;
    if (module == "haweb") {
        homeAssistantWeb->setEnabled(enabled);
        return;
    }
    if (module == "custom") {
        customSources->setEnabled(enabled);
        return;
    }
    if (module == "beacn") {
        saveBeacn(enabled, settings.value("modules/beacn/control", false).toBool(),
                  settings.value("modules/beacn/cloud", false).toBool());
        return;
    }
    if (module == "wallpaper") {
        auto state = wallpaperState();
        saveWallpaper(enabled, state["control_enabled"].toBool(), state["share_path"].toBool(),
                      state["cloud"].toBool(), state["override"].toString());
        return;
    }
    if (module == "coretemp") {
        ++coretempGeneration;
        settings.setValue("modules/coretemp/enabled", enabled);
        scheduleSettingsFlush();
        coretempClock.invalidate();
        coretempData = {{"connected", false}, {"status", enabled ? "Connecting…" : "Disabled"}};
        mergeReadings("coretemp", {});
        browserPending = true;
        if (browserVisible && !browserInteracting)
            refreshBrowserAsync();
        publishPolicy();
        emit changed();
        emit sensorsChanged();
        pollCoreTemp();
        return;
    }
    if (module == "nvidia") {
        ++nvidiaGeneration;
        settings.setValue("modules/nvidia/enabled", enabled);
        scheduleSettingsFlush();
        nvidiaClock.invalidate();
        nvidiaData = {{"connected", false}, {"status", enabled ? "Connecting…" : "Disabled"}};
        mergeReadings("nvidia", {});
        browserPending = true;
        if (browserVisible && !browserInteracting)
            refreshBrowserAsync();
        publishPolicy();
        emit changed();
        emit sensorsChanged();
        log(enabled ? "NVIDIA module enabled" : "NVIDIA module disabled");
        pollNvidia();
        return;
    }

    if (module == "icue") {
        ++*localIcueEpoch;
        localIcueQueue.clear();
        ++*commandEpoch;
        settings.setValue("modules/icue/enabled", enabled);
        scheduleSettingsFlush();
        if (!enabled) {
            icueReset = true;
            acceptIcue({});
            acceptIcueAutomation({});
        } else {
            icueClock.invalidate();
            automationClock.invalidate();
            icueData = {};
            automationData = {};
            icueData["enabled"] = true;
            icueData["status"] = "Connecting to iCUE";
        }
        pollIcue();
        browserPending = true;
        if (browserVisible && !browserInteracting)
            refreshBrowserAsync();
        publishPolicy();
        emit changed();
        emit sensorsChanged();
        return;
    }
    if (module != "hwinfo" && module != "windows")
        return;
    ++*commandEpoch;
    settings.setValue(module == "hwinfo" ? "sources/hwinfo" : "modules/windows/enabled", enabled);
    if (module == "hwinfo")
        latest = {};
    ++collectionGeneration;
    refreshSlow = true;
    inventorySchedule.invalidate();
    slowSchedule.invalidate();
    mediaSchedule.invalidate();
    if (module == "windows") {
        if (lighting)
            lighting->setEnabled(enabled && settings.value("modules/windows/lighting", false).toBool(),
                                 testMode);
        windowsClock.invalidate();
        windowsSlowClock.invalidate();
        mediaClock.invalidate();
        inventoryClock.invalidate();
        if (enabled && nativeWindow)
            audioMonitor.start(reinterpret_cast<HWND>(nativeWindow));
        else
            audioMonitor.stop();
        if (announcements)
            announcements->setPermitted(controlPermissions()["announcements"].toBool());
        if (richNotifications)
            richNotifications->setPermitted(controlPermissions()["notifications"].toBool());
    }
    if (!enabled) {
        QStringList unavailable;
        for (auto it = catalog.cbegin(); it != catalog.cend(); ++it)
            if (it.value().value("source") == module)
                unavailable.append(it.key());
        for (const auto &id : unavailable) {
            catalog[id]["available"] = false;
            catalog[id]["value"] = QJsonValue::Null;
        }
    }
    browserPending = true;
    if (browserVisible && !browserInteracting)
        refreshBrowserAsync();
    sample();
    publishPolicy();
    emit sensorsChanged();
    log(QString("%1 module %2; saved selections retained").arg(module, enabled ? "enabled" : "disabled"));
}

void Controller::setPollingInterval(const QString &module, int milliseconds) {
    if (module == "coretemp") {
        milliseconds = qBound(500, milliseconds, 60000);
        settings.setValue("modules/coretemp/poll_ms", milliseconds);
        coretempTimer.setInterval(milliseconds);
    } else if (module == "nvidia") {
        milliseconds = qBound(500, milliseconds, 60000);
        settings.setValue("modules/nvidia/poll_ms", milliseconds);
        nvidiaTimer.setInterval(milliseconds);
    } else if (module == "windows") {
        milliseconds = qBound(500, milliseconds, 60000);
        settings.setValue("sample_seconds", milliseconds / 1000.0);
        sampleTimer.setInterval(milliseconds);
    } else if (module == "hwinfo") {
        milliseconds = qBound(100, milliseconds, 10000);
        settings.setValue("hwinfo_check_ms", milliseconds);
        hwinfoTimer.setInterval(milliseconds);
    } else
        return;
    scheduleSettingsFlush();
    emit changed();
    log(QString("%1 polling interval: %2 ms").arg(module).arg(milliseconds));
}

QJsonObject Controller::sourceOptions() const {
    QJsonObject options;
    for (const auto key :
         {"hwinfo", "session", "wifi", "auto_drives", "foreground", "window_titles", "microphone", "events",
          "event_messages", "media", "media_titles", "notifications", "notification_contents", "security"})
        options[key] =
            settings
                .value("sources/" + QString(key),
                       QString(key) == "hwinfo" || QString(key) == "session" ||
                           QString(key) == "wifi" || QString(key) == "auto_drives")
                .toBool();
    return options;
}

void Controller::setLicense(const QString &license) {
    if (!QStringList{"unknown", "free", "pro"}.contains(license))
        return;
    settings.setValue("license", license);
    scheduleSettingsFlush();
    emit changed();
    publish();
}

void Controller::saveSources(const QVariantMap &options) {
    if (options.contains("hwinfo"))
        setModuleEnabled("hwinfo", options.value("hwinfo").toBool());
    const auto valid = sourceOptions();
    if (options.contains("media") && !options.value("media").toBool())
        ++*commandEpoch;
    for (auto it = options.cbegin(); it != options.cend(); ++it)
        if (valid.contains(it.key()))
            settings.setValue("sources/" + it.key(), it.value().toBool());
    ++collectionGeneration;
    refreshSlow = true;
    scheduleSettingsFlush();
    browserPending = true;
    if (browserVisible && !browserInteracting)
        refreshBrowserAsync();
    sample();
    publishPolicy();
    emit sensorsChanged();
    log("Collection sources and privacy settings saved");
}

void Controller::sampleWindows() {
    pollDisplays();
    if (!moduleEnabled("windows"))
        return;
    if (controlPermissions()["power_plan"].toBool() && !powerJob.isRunning() &&
        (!powerClock.isValid() || powerClock.elapsed() > 10000)) {
        powerClock.restart();
        powerJob.onStatus = [this](QString error) { workerStatus("Power plans", error); };
        powerJob.submit([] { return WindowsCommands::powerState(); },
                        [this](QJsonObject info) {
                            powerInfo = info;
                            publish();
                        });
    }
    const auto generation = collectionGeneration;
    const auto eventsAtStart = eventCounter;
    auto options = sourceOptions();
    if (!mediaJob.isRunning() &&
        (!mediaSchedule.isValid() || mediaSchedule.elapsed() >= 15000 || refreshSlow)) {
        mediaSchedule.restart();
        const bool enabled = options["media"].toBool() && !testMode;
        const bool titles = options["media_titles"].toBool();
        mediaJob.onStatus = [this](QString error) { workerStatus("Media sessions", error); };
        mediaJob.submit([enabled, titles] { return enabled ? readMediaSessions(titles) : QJsonArray{}; },
                        [this, generation](QJsonArray rows) {
                            if (generation != collectionGeneration) {
                                mediaSchedule.invalidate();
                                return;
                            }
                            mergeReadings("windows", rows, 7);
                            mediaClock.restart();
                            publish();
                            emit changed();
                            emit sensorsChanged();
                        });
    }
    options["refresh_slow"] = refreshSlow;
    if (!slowJob.isRunning())
        refreshSlow = false;
    auto fast = options;
    fast["_fast_only"] = true;
    auto slow = options;
    slow["_slow_only"] = true;
    if ((!inventorySchedule.isValid() || inventorySchedule.elapsed() >= 10000 ||
         options["refresh_slow"].toBool()) &&
        !inventoryJob.isRunning()) {
        inventorySchedule.restart();
        auto opts = options;
        opts["_inventory_only"] = true;
        inventoryJob.submit([collector = inventory, opts] { return collector->collect(opts); },
                            [this, generation](QJsonArray rows) {
                                if (generation != collectionGeneration)
                                    return;
                                mergeReadings("windows", rows, 3);
                                inventoryClock.restart();
                                publish();
                                emit sensorsChanged();
                            });
    }
    auto collect = [](std::shared_ptr<WindowsCollector> collector, QJsonObject options) {
        return collector->collect(options);
    };
    windowsJob.submit([collector = windows, fast] {
                          auto rows = collector->collect(fast);
                          return std::make_pair(std::move(rows), collector->wifiIssue());
                      },
                      [this, generation, eventsAtStart](std::pair<QJsonArray, QString> result) {
                          if (generation != collectionGeneration)
                              return;
                          auto rows = std::move(result.first);
                          if (wifiIssue != result.second) {
                              wifiIssue = result.second;
                              log(wifiIssue.isEmpty() ? "Wi-Fi access warning cleared" : wifiIssue);
                          }
                          for (qsizetype i = 0; i < rows.size(); ++i) {
                              const auto id = rows[i].toObject()["id"].toString();
                              if (eventVersions.value(id) > eventsAtStart)
                                  rows[i] = catalog.value(id);
                          }
                          if (rows.isEmpty())
                              windowsClock.invalidate();
                          else {
                              mergeReadings("windows", rows, 1);
                              windowsClock.restart();
                          }
                          publish();
                          emit changed();
                          emit sensorsChanged();
                      });
    if ((!slowSchedule.isValid() || slowSchedule.elapsed() >= 10000 || options["refresh_slow"].toBool()) &&
        !slowJob.isRunning()) {
        slowSchedule.restart();
        slowJob.submit([collector = slowWindows, slow, collect] { return collect(collector, slow); },
                       [this, generation](QJsonArray rows) {
                           if (generation != collectionGeneration)
                               return;
                           for (auto value : rows) {
                               const auto row = value.toObject();
                               if (row["id"] == "win_notifications_status" &&
                                   catalog.value("win_notifications_status")["value"] != row["value"])
                                   log("Notifications: " + row["value"].toString());
                           }
                           mergeReadings("windows", rows, 2);
                           windowsSlowClock.restart();
                           publish();
                           emit changed();
                           emit sensorsChanged();
                       });
    }
}

void Controller::mergeReadings(const QString &source, const QJsonArray &rows, int scope) {
    applyReadingsUpdate(source, prepareReadingsUpdate(source, scope)(rows), scope);
}

std::function<Controller::ReadingsUpdate(const QJsonArray &)>
Controller::prepareReadingsUpdate(const QString &source, int scope) {
    if (catalogSourceIndexSize != catalog.size()) {
        catalogSourceIds.clear();
        for (auto it = catalog.cbegin(); it != catalog.cend(); ++it)
            catalogSourceIds[it.value().value("source").toString()].insert(it.key());
        catalogSourceIndexSize = catalog.size();
    }
    const auto sourceIds = catalogSourceIds.value(source);
    const auto before = catalog;
    const bool autoDrives = settings.value("sources/auto_drives", true).toBool();
    // All captures own their data. This closure also runs on the HWiNFO worker.
    return [source, scope, before, sourceIds, autoDrives](const QJsonArray &rows) {
        ReadingsUpdate update{before, before, sourceIds};
        update.hasReadings = !rows.isEmpty();
        auto &catalog = update.after;
        QSet<QString> incoming;
        for (const auto &value : rows)
            incoming.insert(value.toObject().value("id").toString());
        for (const auto &id : sourceIds) {
            const auto previous = catalog.value(id);
            if (incoming.contains(id))
                continue;
            const int rowScope = id.startsWith("win_media_")     ? 7
                                 : id.startsWith("win_display_") ? 6
                                 : source == "icue" ? (previous.value("icueAutomation").toBool() ? 4 : 5)
                                 : id.startsWith("win_disk_")
                                     ? 3
                                     : (id.startsWith("win_security_") || id.startsWith("win_events_") ||
                                                id.startsWith("win_notifications_")
                                            ? 2
                                            : 1);
            if (previous.value("source") == source && (scope == 0 || scope == rowScope)) {
                if (previous.value("available").toBool())
                    catalog[id]["available"] = false;
                if (!previous.value("value").isNull())
                    catalog[id]["value"] = QJsonValue::Null;
            }
        }
        for (const auto &value : rows) {
            auto row = value.toObject();
            const auto id = row["id"].toString();
            if (id.isEmpty())
                continue;
            row["source"] = source;
            if (source == "hwinfo") {
                row["kind"] = "number";
                row["available"] = !row["value"].isNull();
                row["state_class"] = "measurement";
                const auto name = row["name"].toString().toLower();
                row["recommended"] = name.contains("cpu package") || name == "gpu temperature" ||
                                     name == "gpu power" || name == "gpu core load" ||
                                     name == "total cpu usage";
            }
            if (!catalog.contains(id) && catalog.size() >= 9990)
                continue;
            update.metadataChanged = update.metadataChanged || !catalog.contains(id) ||
                                     catalog.value(id).value("group") != row["group"] ||
                                     catalog.value(id).value("name") != row["name"];
            if (!catalog.contains(id) && id.startsWith("win_disk_") &&
                (id.endsWith("_connected") || id.endsWith("_percent")) && autoDrives)
                update.autoSelected.insert(id);
            catalog[id] = row;
            update.sourceIds.insert(id);
            if (row["recommended"].toBool())
                update.recommended.insert(id);
        }
        return update;
    };
}

void Controller::applyReadingsUpdate(const QString &source, const ReadingsUpdate &update, int scope) {
    const bool indexValid = catalogSourceIndexSize == catalog.size();
    auto sourceIds = update.sourceIds;
    if (!catalog.applyChanges(update.before, update.after, 9990)) {
        // Another collector can fill the remaining slots while this job runs.
        sourceIds.removeIf([this](const QString &id) { return !catalog.contains(id); });
    }
    catalogSourceIds[source] = sourceIds;
    if (indexValid)
        catalogSourceIndexSize = catalog.size();
    selected.unite(update.autoSelected & sourceIds);
    const QString selectionKey = source == "beacn"      ? "beacn_selection_initialized"
                                 : source == "custom"   ? "custom_selection_initialized"
                                 : source == "coretemp" ? "coretemp_selection_initialized"
                                 : source == "nvidia"   ? "nvidia_selection_initialized"
                                 : source == "icue"   ? (scope == 4 ? "icue_automation_selection_initialized"
                                                                    : "icue_selection_initialized")
                                 : source == "hwinfo" ? "selection_initialized"
                                                      : "windows_selection_initialized";
    const bool initialize =
        (source != "windows" || scope == 0 || scope == 1) && !settings.value(selectionKey, false).toBool();
    if (initialize && update.hasReadings) {
        selected.unite(update.recommended & sourceIds);
        settings.setValue(selectionKey, true);
        settings.setValue("selected", QStringList(selected.begin(), selected.end()));
    }
    if (update.metadataChanged) {
        ++catalogMetadataRevision;
        settings.setValue("selected", QStringList(selected.begin(), selected.end()));
        metadataPending = true;
        if (!metadataTimer.isActive())
            metadataTimer.start();
    }
}

void Controller::persistCatalog() {
    if (!metadataPending)
        return;
    if (metadataJob.isRunning()) {
        metadataTimer.start();
        return;
    }
    metadataPending = false;
    const auto rows = catalog;
    const auto path = settings.fileName();
    metadataJob.submit(
        [rows, path] {
            QJsonArray metadata;
            for (auto row : rows) {
                row.remove("value");
                row.remove("available");
                metadata.append(row);
            }
            QSettings output(path, SettingsStore::format());
            output.setValue("catalog", QJsonDocument(metadata).toJson(QJsonDocument::Compact));
            output.sync();
            return output.status() == QSettings::NoError;
        },
        [this](bool ok) {
            if (!ok)
                log("Could not persist sensor catalogue");
            if (metadataPending)
                metadataTimer.start();
        });
}

QJsonArray Controller::allReadings(const QSet<QString> *ids) const {
    return captureReadings(ids)();
}

std::function<QJsonArray()> Controller::captureReadings(const QSet<QString> *ids) const {
    // Capture implicitly shared data and scalar state only. Worker code never reads this.
    const auto rows = catalog;
    const bool subset = ids != nullptr;
    const auto wanted = ids ? *ids : QSet<QString>{};
    const auto options = sourceOptions();
    const bool windowsReady = moduleEnabled("windows") && windowsClock.isValid() &&
                              windowsClock.elapsed() < qMax(30000, sampleTimer.interval() * 3);
    const bool hwinfoReady = healthy(), hwinfoValid = latest.valid;
    const auto hwinfoStatus = latest.status;
    const bool automationReady = icueAutomationState()["connected"].toBool();
    const bool directReady = icueDirectEnabled() && icueData["connected"].toBool() && icueClock.isValid() &&
                             icueClock.elapsed() < 15000;
    const bool beacnEnabled = moduleEnabled("beacn"), beacnReady = beacnData.value("ready").toBool();
    const bool coretempReady = coretempHealthy(), nvidiaReady = nvidiaHealthy();
    const bool customReady = moduleEnabled("custom");
    const bool storageReady = inventoryClock.isValid() && inventoryClock.elapsed() < 30000;
    const bool mediaReady = moduleEnabled("windows") && mediaClock.isValid() && mediaClock.elapsed() < 45000;
    const bool slowReady = windowsSlowClock.isValid() && windowsSlowClock.elapsed() < 120000;
    const bool displaysReady = displaysEnabled() && displayInfo["available"].toBool() &&
                               displayClock.isValid() && displayClock.elapsed() < 30000;
    QJsonArray appReadings;
    auto add = [&](QString id, QString name, QJsonValue value, QString kind, QString unit = "") {
        if (ids && !ids->contains("app_" + id))
            return;
        appReadings.append(QJsonObject{{"id", "app_" + id},
                                       {"source", "windows"},
                                       {"group", "App health"},
                                       {"name", name},
                                       {"value", value},
                                       {"available", !value.isNull()},
                                       {"kind", kind},
                                       {"unit", unit},
                                       {"type", 0},
                                       {"recommended", false},
                                       {"state_class", kind == "number" ? "measurement" : ""}});
    };
    add("uptime", "WHAT uptime", double(appClock.elapsed()) / 1000, "number", "s");
    add("clients", "Home Assistant connections", int(clientCount), "number");
    add("reconnections", "Repeat connections since app start", double(connections > 0 ? connections - 1 : 0),
        "number");
    add("last_transmission", "Last snapshot sent",
        lastTransmission.isValid() ? QJsonValue(lastTransmission.toString(Qt::ISODateWithMs))
                                   : QJsonValue(QJsonValue::Null),
        "timestamp");
    add("hwinfo_enabled", "HWiNFO enabled", options["hwinfo"], "binary");
    add("hwinfo_healthy", "HWiNFO healthy",
        options["hwinfo"].toBool() ? QJsonValue(healthy()) : QJsonValue(QJsonValue::Null), "binary");
    add("windows_healthy", "Windows collection healthy", windowsReady, "binary");
    add("display_on", "Console display on", displayState, "binary");
    return [=] {
        QJsonArray result;
        // Preserve the public catalogue order independently of snapshot storage pages.
        // Sorting happens on the worker, not on the GUI thread.
        QStringList orderedIds;
        if (subset)
            orderedIds = wanted.values();
        else {
            orderedIds.reserve(rows.size());
            for (auto it = rows.cbegin(); it != rows.cend(); ++it)
                orderedIds.append(it.key());
        }
        std::sort(orderedIds.begin(), orderedIds.end());
        for (const auto &id : orderedIds) {
            const auto it = rows.constFind(id);
            if (it == rows.cend())
                continue;
            auto row = it.value();
            const QString source = row.value("source").toString();
            bool allowed = true;
            if (source == "beacn")
                allowed = beacnEnabled && (id == "beacn_ready" || beacnReady);
            else if (source == "coretemp")
                allowed = coretempReady;
            else if (source == "nvidia")
                allowed = nvidiaReady;
            else if (source == "custom")
                allowed = customReady;
            else if (source == "hwinfo")
                allowed = hwinfoReady;
            else if (source == "icue")
                allowed = row.value("icueAutomation").toBool() ? automationReady : directReady;
            else {
                const bool slow = id.startsWith("win_security_") || id.startsWith("win_events_") ||
                                  id.startsWith("win_notifications_");
                const bool storage = id.startsWith("win_disk_");
                allowed = storage ? storageReady : slow ? slowReady : windowsReady;
                if (id.startsWith("win_display_"))
                    allowed = displaysReady;
                if (id.startsWith("win_events_"))
                    allowed = allowed && options["events"].toBool();
                if (id.startsWith("win_events_") && id.endsWith("_message"))
                    allowed = allowed && options["event_messages"].toBool();
                if (id.startsWith("win_net_") && (id.endsWith("_wifi_signal") || id.endsWith("_ssid")))
                    allowed = allowed && options["wifi"].toBool();
                if (id.startsWith("win_foreground_"))
                    allowed = allowed && options["foreground"].toBool();
                if (id == "win_foreground_title")
                    allowed = allowed && options["window_titles"].toBool();
                if (id.startsWith("win_microphone_"))
                    allowed = allowed && options["microphone"].toBool();
                if (id.startsWith("win_security_"))
                    allowed = allowed && options["security"].toBool();
                if (id.startsWith("win_media_"))
                    allowed = mediaReady && options["media"].toBool();
                if (id == "win_media_title" || id == "win_media_artist")
                    allowed = allowed && options["media_titles"].toBool();
                if (id.startsWith("win_notifications_"))
                    allowed = allowed && options["notifications"].toBool();
                if (id == "win_notifications_title" || id == "win_notifications_body")
                    allowed = allowed && options["notification_contents"].toBool();
                if (id == "win_user" || id == "win_idle" || id == "win_locked" || id == "win_remote")
                    allowed = allowed && options["session"].toBool();
            }
            const bool available = allowed && row.value("available").toBool();
            if (row.value("available").toBool() != available)
                row["available"] = available;
            if (!row.value("available").toBool()) {
                if (!row.value("value").isNull())
                    row["value"] = QJsonValue::Null;
                if (source == "hwinfo") {
                    row["unavailable_reason"] =
                        !options["hwinfo"].toBool() ? "HWiNFO source disabled"
                        : !hwinfoValid              ? hwinfoStatus
                        : !hwinfoReady
                            ? "HWiNFO timestamp is stale"
                            : "Reading absent from current HWiNFO output or its value is non-finite";
                }
            }
            result.append(row);
        }
        for (const auto &row : appReadings)
            result.append(row);
        return result;
    };
}

QVariantMap Controller::browserFilters() const {
    if (currentBrowserFilters)
        return *currentBrowserFilters;
    QVariantMap result{{"query", QString{}},
                       {"source", settings.value("browser/source", "all").toString()},
                       {"selected", settings.value("browser/selected", false).toBool()},
                       {"unshared", settings.value("browser/unshared", false).toBool()},
                       {"favourites", settings.value("browser/favourites", false).toBool()}};
    if (result["source"].toString().isEmpty())
        result["source"] = "all";
    return result;
}

QVariantList Controller::browser() const {
    const auto rows = browserInput();
    QSet<QString> categories;
    for (const auto &value : rows) {
        const auto row = value.toMap();
        categories.insert(
            row.value("categoryKey", row["source"].toString() + " / " + row["group"].toString()).toString());
    }
    return formatBrowser(rows, browserFilters(), browserFolding.closed(categories));
}

// Prepare the expensive diff off-thread; apply it only if the user's filters and permissions
// still match. Sensor values may advance meanwhile without invalidating the whole result.
void Controller::refreshBrowserAsync() {
    const auto generation = browserGeneration;
    const auto filters = browserFilters();
    const auto folding = browserFolding;
    const auto chosen = selected;
    const auto stars = favourites;
    const auto options = sourceOptions();
    const auto policyGeneration = collectionGeneration;
    const auto metadataRevision = catalogMetadataRevision;
    const auto permissionsRevision = snapshotEpoch;
    // Refilter the latest prepared catalogue immediately. A normal background
    // refresh follows to pick up newer values. Permissions and metadata must match.
    const bool reuseInput = !browserResultsReady && browserInputCache &&
                            browserInputCache->selected == chosen && browserInputCache->favourites == stars &&
                            browserInputCache->options == options &&
                            browserInputCache->policyGeneration == policyGeneration &&
                            browserInputCache->metadataRevision == metadataRevision &&
                            browserInputCache->permissionsRevision == permissionsRevision;
    // Interactive searches must never queue behind catalogue collection/formatting.
    auto &worker = reuseInput ? browserSearchJob : browserJob;
    if (worker.isRunning())
        return;
    browserPending = false;
    const auto cached = reuseInput ? browserInputCache : nullptr;
    const auto input = cached ? std::function<BrowserInput()>{} : captureBrowserInput();
    const auto previous = browserModel.snapshot();
    const auto modelRevision = browserModel.generation();

    struct BrowserUpdate {
        BrowserModel::Prepared model;
        BrowserModel::Categories grouped;
        QList<int> visibleRows;
        QSet<QString> categories;
        QSet<QString> closed;
        QHash<QString, QStringList> categoryReadings;
        QVariantList previous;
        std::shared_ptr<const CachedBrowserInput> input;
    };

    worker.submit(
        [input, cached, filters, folding, previous, modelRevision, chosen, stars, options, policyGeneration,
         metadataRevision, permissionsRevision] {
            auto prepared = cached;
            if (!prepared) {
                const auto fresh = input();
                prepared = std::make_shared<const CachedBrowserInput>(
                    CachedBrowserInput{indexBrowserRows(fresh.rows), fresh.categoryReadings, chosen, stars,
                                       options, policyGeneration, metadataRevision, permissionsRevision});
            }
            QSet<QString> categories;
            for (const auto &group : prepared->index.grouped.groups)
                categories.insert(group.name);
            auto closed = folding.closed(categories);
            auto grouped = filterBrowserIndex(prepared->index, filters);
            auto visible = BrowserModel::visibleCategoryRows(grouped, closed);
            const auto next = BrowserModel::categorySnapshot(grouped, visible, closed);
            auto diff = BrowserModel::prepare(previous, next, modelRevision);
            return BrowserUpdate{
                std::move(diff),   std::move(grouped),         std::move(visible), std::move(categories),
                std::move(closed), prepared->categoryReadings, previous,           std::move(prepared)};
        },
        [this, generation, usedCache = bool(cached), filters, folding, chosen, stars, options,
         policyGeneration, metadataRevision, permissionsRevision](BrowserUpdate update) {
            const bool inputCurrent =
                chosen == selected && stars == favourites && options == sourceOptions() &&
                policyGeneration == collectionGeneration && permissionsRevision == snapshotEpoch;
            if (!usedCache && inputCurrent && metadataRevision == catalogMetadataRevision)
                browserInputCache.swap(update.input);
            if (!browserVisible || browserInteracting || filters != browserFilters() ||
                folding != browserFolding || !inputCurrent ||
                update.model.revision != browserModel.generation()) {
                browserPending = true;
                // Apply the latest click as soon as an obsolete job finishes.
                if (browserVisible && !browserInteracting)
                    QTimer::singleShot(0, this, &Controller::refreshBrowserAsync);
            } else {
                browserCategories.swap(update.categories);
                browserCategoryReadings.swap(update.categoryReadings);
                browserCategoriesRevision = metadataRevision;
                browserCategoriesPolicy = policyGeneration;
                browserModel.apply(update.model);
                browserModel.swapCategories(update.grouped, update.visibleRows, update.closed);
                browserResultsReady = true;
                // New sensor values must not starve an otherwise valid completed frame.
                browserPending = usedCache || generation != browserGeneration;
            }
            emit browserLoadingChanged();
            // Free thousands of old or discarded row maps away from the GUI.
            browserJob.post([update = std::move(update)] {});
        });
    emit browserLoadingChanged();
}

QVariantList Controller::browserInput() const {
    return captureBrowserInput()().rows;
}

// Copy the inputs before returning the closure. A worker may execute it after this
// Controller is gone, so the closure must never implicitly capture or dereference this.
std::function<Controller::BrowserInput()> Controller::captureBrowserInput(bool readingsOnly) const {
    const auto buildReadings = captureReadings();
    const auto policies = publishingPolicies;
    const auto selected = this->selected;
    const auto favourites = this->favourites;
    const auto options = sourceOptions();
    QSet<QString> sources;
    for (auto source : {"windows", "hwinfo", "nvidia", "coretemp", "beacn", "wallpaper", "custom"})
        if (moduleEnabled(source))
            sources.insert(source);
    const auto wallpaper = wallpaperState();
    const auto custom = customSources ? customSources->shared() : QJsonObject{};
    const auto beacn = beacnState();
    const auto icue = icueData;
    const bool direct = icueDirectEnabled();
    const bool available =
        icueData["connected"].toBool() && icueClock.isValid() && icueClock.elapsed() < 15000;
    const auto automation = icueAutomationState();
    const bool displaysEnabled = this->displaysEnabled();
    const auto displayInfo = this->displayInfo;
    const bool displayBusy = (displayJob.isRunning() && !displayQuerying) || displayCommandQueued,
               displayPending = !displayRollback.isEmpty();
    return [=] {
        const auto readings = buildReadings();
        auto allowed = [&](const auto &row) {
            if (row["source"] == "icue") {
                if (!(row["icueAutomation"].toBool() ? automation["enabled"].toBool() : direct))
                    return false;
            } else if (!sources.contains(row["source"].toString()))
                return false;
            if (row["source"] != "windows")
                return true;
            const auto id = row["id"].toString();
            if (id.startsWith("win_display_") && !displaysEnabled)
                return false;
            if (id.startsWith("win_net_") && (id.endsWith("_wifi_signal") || id.endsWith("_ssid")) &&
                !options["wifi"].toBool())
                return false;
            if (id.startsWith("win_foreground_") && !options["foreground"].toBool())
                return false;
            if (id == "win_foreground_title" && !options["window_titles"].toBool())
                return false;
            if (id.startsWith("win_microphone_") && !options["microphone"].toBool())
                return false;
            if (id.startsWith("win_events_") && !options["events"].toBool())
                return false;
            if (id.startsWith("win_events_") && id.endsWith("_message") &&
                !options["event_messages"].toBool())
                return false;
            if (id.startsWith("win_security_") && !options["security"].toBool())
                return false;
            if (id.startsWith("win_media_") && !options["media"].toBool())
                return false;
            if ((id == "win_media_title" || id == "win_media_artist") && !options["media_titles"].toBool())
                return false;
            if (id.startsWith("win_notifications_") && !options["notifications"].toBool())
                return false;
            if ((id == "win_notifications_title" || id == "win_notifications_body") &&
                !options["notification_contents"].toBool())
                return false;
            if ((id == "win_user" || id == "win_idle" || id == "win_locked" || id == "win_remote") &&
                !options["session"].toBool())
                return false;
            return true;
        };
        QVariantList ordinary;
        for (auto value : readings) {
            const auto object = value.toObject();
            if (readingsOnly && !allowed(object))
                continue;
            auto row = object.toVariantMap();
            if (readingsOnly) {
                ordinary.append(row);
                continue;
            }
            const auto id = object["id"].toString();
            const auto policy = policies[id].toObject();
            row["enabled"] = selected.contains(id);
            row["shareScope"] = selected.contains(id) ? policy["scope"].toInt(1) : 0;
            row["highFrequency"] = policy["high"].toBool();
            row["decimals"] = policy["decimals"].toInt(-1);
            row["favourite"] = favourites.contains(id);
            ordinary.append(row);
        }
        if (readingsOnly)
            return BrowserInput{ordinary, {}};
        auto browserRows = makeIcueBrowserRows(ordinary, icue, direct, available, automation, favourites);
        if (custom["enabled"].toBool()) {
            const auto cloudActions = custom["_cloud_actions"].toArray();
            for (auto value : custom["actions"].toArray()) {
                const auto action = value.toObject();
                browserRows.append(QVariantMap{{"id", action["id"].toString()},
                    {"source", "custom"}, {"group", "Saved Commands"},
                    {"name", action["name"].toString()}, {"controlOnly", true},
                    {"control", "custom"}, {"available", action["local_available"].toBool()},
                    {"enabled", action["available"].toBool()},
                    {"haAllowed", action["available"].toBool()},
                    {"cloudAllowed", action["available"].toBool() && custom["cloud"].toBool()
                                         && cloudActions.contains(action["id"])},
                    {"favourite", favourites.contains(action["id"].toString())}});
            }
        }
        if (wallpaper.value("enabled").toBool()) {
            browserRows.append(QVariantMap{
                {"id", "wallpaper_controls"},
                {"source", "wallpaper"},
                {"group", "Playback and wallpapers"},
                {"name", "Player, current wallpaper, playlists, profiles and monitor controls"},
                {"controlOnly", true},
                {"control", "wallpaper"},
                {"available", true},
                {"enabled", wallpaper.value("control_enabled")},
                {"favourite", favourites.contains("wallpaper_controls")},
                {"tooltip",
                 "Open Wallpaper Engine controls. Home Assistant actions use the module control permission."}});
        }
        if (beacn.value("enabled").toBool())
            for (auto v : beacn.value("controls").toList()) {
                auto c = v.toMap();
                const auto controlId = c.value("id").toString();
                const auto id = "beacn_control_" + controlId;
                const bool channel = controlId.startsWith("volume_") || controlId.startsWith("mute_") ||
                                     (controlId.startsWith("audience_") && controlId != "audience_device");
                browserRows.append(QVariantMap{
                    {"id", id},
                    {"source", "beacn"},
                    {"group", channel ? "Mixer controls" : "App controls"},
                    {"name", c.value("name")},
                    {"controlOnly", true},
                    {"control", "beacn"},
                    {"beacn", c},
                    {"available", beacn.value("ready").toBool() && c.value("available").toBool() &&
                                      !beacn.value("busy").toBool()},
                    {"enabled", beacn.value("control_enabled").toBool()},
                    {"favourite", favourites.contains(id)},
                    {"tooltip",
                     "Control BEACN locally. Home Assistant control uses the BEACN module permission."}});
            }
        if (displaysEnabled)
            for (auto v : displayInfo["monitors"].toArray()) {
                auto m = v.toObject();
                QVariantList options{QVariantMap{{"id", "disconnect"}, {"name", "Disconnected"}},
                                     QVariantMap{{"id", "extend"}, {"name", "Extended"}}};
                QString value = m["enabled"].toBool() ? "extend" : "disconnect";
                for (auto other : displayInfo["monitors"].toArray()) {
                    auto o = other.toObject();
                    if (o["id"] == m["id"] || !o["enabled"].toBool())
                        continue;
                    options.append(QVariantMap{{"id", "mirror:" + o["id"].toString()},
                                               {"name", "Mirror " + o["name"].toString()}});
                    if (m["enabled"].toBool() && m["group"] == o["group"])
                        value = "mirror:" + o["id"].toString();
                }
                browserRows.prepend(QVariantMap{
                    {"id", "display_control_" + m["id"].toString()},
                    {"source", "windows"},
                    {"group", "Displays / " + m["name"].toString()},
                    {"name", "Display controls"},
                    {"favourite", favourites.contains("display_control_" + m["id"].toString())},
                    {"controlOnly", true},
                    {"control", "display"},
                    {"monitor", m["id"].toString()},
                    {"options", options},
                    {"value", value},
                    {"available", displayInfo["available"].toBool() && !displayBusy && !displayPending},
                    {"enabled", true},
                    {"primary", m["primary"].toBool()},
                    {"desktopEnabled", m["enabled"].toBool()}});
            }
        BrowserInput result;
        for (const auto &value : browserRows) {
            const auto row = value.toMap();
            // Category sharing includes unavailable and filtered readings. Build
            // its index before applying browser permissions or search filters.
            const auto category =
                row.value("categoryKey", row["source"].toString() + " / " + row["group"].toString())
                    .toString();
            auto &ids = result.categoryReadings[category];
            if (!row["controlOnly"].toBool())
                ids.append(row["id"].toString());
            if (allowed(row))
                result.rows.append(value);
        }
        return result;
    };
}

// Precompute names, aliases and group metadata once per catalogue refresh. A
// query only scans these compact entries and copies references to matching rows.
Controller::BrowserSearchIndex Controller::indexBrowserRows(const QVariantList &rows) {
    BrowserSearchIndex index;
    index.grouped = BrowserModel::prepareCategories(formatBrowser(rows, {{"source", "all"}}, {}));
    index.entries.resize(index.grouped.rows.size());
    for (const auto &group : index.grouped.groups)
        for (int n = group.first + 1; n < group.first + group.count; ++n) {
            const auto row = index.grouped.rows[n].toMap();
            index.entries[n] = {searchText(row), row.value("source").toString(),
                                row.value("enabled").toBool(), row.value("favourite").toBool()};
        }
    return index;
}

BrowserModel::Categories Controller::filterBrowserIndex(const BrowserSearchIndex &index,
                                                        const QVariantMap &filters) {
    const auto words =
        filters.value("query").toString().trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const auto source = filters.value("source", "all").toString();
    const auto sources = source.split(',');
    const bool selected = filters.value("selected").toBool();
    const bool unshared = filters.value("unshared").toBool();
    const bool favourites = filters.value("favourites").toBool();
    if (words.isEmpty() && source == "all" && !selected && !unshared && !favourites)
        return index.grouped;

    QList<QList<int>> matches(index.grouped.groups.size());
    QSet<QString> visibleGroups, writableGroups;
    for (int g = 0; g < index.grouped.groups.size(); ++g) {
        const auto &group = index.grouped.groups[g];
        bool writable = false;
        for (int n = group.first + 1; n < group.first + group.count; ++n) {
            const auto &entry = index.entries[n];
            if ((source != "all" && !sources.contains(entry.source)) || (selected && !entry.selected) ||
                (unshared && entry.selected) || (favourites && !entry.favourite) ||
                !matchesSearch(entry.text, words))
                continue;
            matches[g].append(n);
            writable |= !index.grouped.rows[n].toMap().value("control").toString().isEmpty();
        }
        if (!matches[g].isEmpty()) {
            visibleGroups.insert(group.name);
            if (writable)
                writableGroups.insert(group.name);
            for (const auto &parent : group.ancestors) {
                visibleGroups.insert(parent);
                if (writable)
                    writableGroups.insert(parent);
            }
        }
    }
    BrowserModel::Categories result;
    result.rows.reserve(index.grouped.rows.size());
    for (int g = 0; g < index.grouped.groups.size(); ++g) {
        const auto &group = index.grouped.groups[g];
        if (!visibleGroups.contains(group.name))
            continue;
        result.groups.append(
            {group.name, group.ancestors, int(result.rows.size()), 1 + int(matches[g].size())});
        auto header = index.grouped.rows[group.first].toMap();
        header["writable"] = writableGroups.contains(group.name);
        result.rows.append(header);
        for (const int row : matches[g])
            result.rows.append(index.grouped.rows[row]);
    }
    return result;
}

QVariantList Controller::formatBrowser(QVariantList browserRows, QVariantMap filters,
                                       QSet<QString> collapsed) {
    QMap<QString, QVariantList> groups;
    QMap<QString, QPair<int, int>> counts;
    QMap<QString, QVariantMap> sharingCounts, metadata;
    QSet<QString> writableGroups;
    const auto query = filters.value("query").toString().trimmed();
    const auto words = query.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const auto source = filters.value("source", "all").toString();
    const auto sources = source.split(',');
    const bool selectedOnly = filters.value("selected", false).toBool();
    const bool unsharedOnly = filters.value("unshared", false).toBool();
    const bool favouritesOnly = filters.value("favourites", false).toBool();
    for (const auto &value : browserRows) {
        const auto row = value.toMap();
        auto key =
            row.value("categoryKey", row["source"].toString() + " / " + row["group"].toString()).toString();
        metadata[key] = row;
        if (!row["controlOnly"].toBool()) {
            counts[key].first += row["enabled"].toBool() ? 1 : 0;
            ++counts[key].second;
            auto &n = sharingCounts[key];
            const int scope = row["shareScope"].toInt();
            for (auto bit : {1, 2, 3})
                if ((scope & bit) == bit)
                    n[QString::number(bit)] = n[QString::number(bit)].toInt() + 1;
            if (row["highFrequency"].toBool())
                n["high"] = n["high"].toInt() + 1;
        }
        if (source != "all" && !sources.contains(row["source"].toString()))
            continue;
        if (selectedOnly && !row["enabled"].toBool())
            continue;
        if (unsharedOnly && row["enabled"].toBool())
            continue;
        if (favouritesOnly && !row["favourite"].toBool())
            continue;
        if (!matchesSearch(searchText(row), words))
            continue;
        auto entry = row;
        entry["header"] = false;
        entry["category"] = key;
        groups[key].append(entry);
        if (!row.value("control").toString().isEmpty())
            writableGroups.insert(key);
    }
    // Include ancestors and their control capability even while descendants are
    // collapsed. Search results describe only the entries matching the filters.
    const auto visibleKeys = groups.keys();
    for (const auto &key : visibleKeys) {
        const bool writable = writableGroups.contains(key);
        auto parent = metadata[key]["parentCategory"].toString();
        QSet<QString> seen;
        while (!parent.isEmpty() && !seen.contains(parent)) {
            seen.insert(parent);
            if (!groups.contains(parent))
                groups.insert(parent, {});
            if (writable)
                writableGroups.insert(parent);
            parent = metadata[parent]["parentCategory"].toString();
        }
    }
    QVariantList result;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const auto meta = metadata[it.key()];
        const auto parent = meta["parentCategory"].toString();
        int depth = 0;
        bool ancestorClosed = false;
        auto ancestor = parent;
        QSet<QString> seen;
        while (!ancestor.isEmpty() && !seen.contains(ancestor)) {
            seen.insert(ancestor);
            ++depth;
            ancestorClosed |= collapsed.contains(ancestor);
            ancestor = metadata[ancestor]["parentCategory"].toString();
        }
        if (ancestorClosed)
            continue;
        const bool closed = collapsed.contains(it.key());
        QString title = meta["categoryName"].toString();
        if (title.isEmpty())
            title = it.key().startsWith("coretemp")    ? "Core Temp" + it.key().mid(8)
                    : it.key().startsWith("hwinfo")    ? "HWiNFO" + it.key().mid(6)
                    : it.key().startsWith("icue")      ? "iCUE" + it.key().mid(4)
                    : it.key().startsWith("nvidia")    ? "NVIDIA" + it.key().mid(6)
                    : it.key().startsWith("wallpaper") ? "Wallpaper Engine" + it.key().mid(9)
                    : it.key().startsWith("beacn")     ? "BEACN" + it.key().mid(5)
                    : it.key().startsWith("custom")    ? "Custom" + it.key().mid(6)
                                                       : "Windows" + it.key().mid(7);
        result.append(QVariantMap{{"header", true},
                                  {"category", it.key()},
                                  {"parentCategory", parent},
                                  {"name", title},
                                  {"collapsed", closed},
                                  {"selected", counts[it.key()].first},
                                  {"total", counts[it.key()].second},
                                  {"sharingCounts", sharingCounts[it.key()]},
                                  {"controlOnly", counts[it.key()].second == 0},
                                  {"writable", writableGroups.contains(it.key())},
                                  {"depth", depth}});
        if (!closed)
            for (const auto &value : it.value()) {
                auto row = value.toMap();
                row["depth"] = depth;
                result.append(row);
            }
    }
    return result;
}

void Controller::setBrowserFilters(const QString &query, const QString &source, bool selectedOnly,
                                   bool favouritesOnly, bool unsharedOnly) {
    const QVariantMap filters{{"query", query.left(256)},
                              {"source", source.isEmpty() ? QString("all") : source},
                              {"selected", selectedOnly && !unsharedOnly},
                              {"favourites", favouritesOnly},
                              {"unshared", unsharedOnly}};
    auto previous = browserFilters();
    if (filters == previous)
        return;
    currentBrowserFilters = filters;
    browserResultsReady = false;
    // Keep the query in memory. Only filter preferences need a disk write.
    previous["query"] = filters["query"];
    if (filters != previous)
        scheduleSettingsFlush();
    emit sensorsChanged();
    emit changed();
    if (browserVisible && !browserInteracting)
        refreshBrowserAsync();
}

void Controller::setCategoryCollapsed(const QString &category, bool close) {
    if (close == browserFolding.defaultCollapsed)
        browserFolding.exceptions.remove(category);
    else
        browserFolding.exceptions.insert(category);
    if (browserCategoriesPolicy == collectionGeneration &&
        browserModel.setCollapsed(browserFolding.closed(browserCategories)))
        return;
    emit sensorsChanged();
    if (browserVisible && !browserInteracting)
        refreshBrowserAsync();
}

void Controller::collapseCategories(bool close) {
    browserFolding = {close, {}};
    if (browserCategoriesPolicy == collectionGeneration &&
        browserModel.setCollapsed(browserFolding.closed(browserCategories)))
        return;
    emit sensorsChanged();
    if (browserVisible && !browserInteracting)
        refreshBrowserAsync();
}

QStringList Controller::categorySensorIds(const QString &category) const {
    if (browserCategoriesRevision == catalogMetadataRevision &&
        browserCategoriesPolicy == collectionGeneration && browserCategoryReadings.contains(category))
        return browserCategoryReadings.value(category);
    // Callers can change a category before the browser has prepared its first frame.
    QStringList ids;
    for (const auto &value : icueBrowserRows(sensors())) {
        const auto row = value.toMap();
        if (row["controlOnly"].toBool() ||
            row.value("categoryKey", row["source"].toString() + " / " + row["group"].toString()).toString() !=
                category)
            continue;
        ids.append(row["id"].toString());
    }
    return ids;
}

void Controller::setCategoryEnabled(const QString &category, bool enabled) {
    for (const auto &id : categorySensorIds(category)) {
        if (enabled) {
            selected.insert(id);
            restoreSharingScope(id);
        } else
            selected.remove(id);
    }
    persistSelection();
}

void Controller::setCategoryPublishing(const QString &category, int bit, bool enabled) {
    if (bit != 1 && bit != 2 && bit != 3 && bit != 4)
        return;
    for (const auto &id : categorySensorIds(category)) {
        auto policy = publishingPolicies.value(id).toObject();
        if (bit == 4)
            policy["high"] = enabled;
        else {
            const int scope = selected.contains(id) ? policy["scope"].toInt(1) : 0;
            const int updated = enabled ? scope | bit : scope & ~bit;
            policy["scope"] = updated;
            if (updated)
                selected.insert(id);
            else
                selected.remove(id);
        }
        settings.setValue("publishing/" + id, policy.toVariantMap());
        publishingPolicies[id] = policy;
    }
    persistSelection();
}

void Controller::setFavourite(const QString &id, bool enabled) {
    if (enabled)
        favourites.insert(id);
    else
        favourites.remove(id);
    settings.setValue("favourites", QStringList(favourites.begin(), favourites.end()));
    emit sensorsChanged();
    if (browserVisible && !browserInteracting)
        refreshBrowserAsync();
}

void Controller::requestNotificationAccess() {
    if (!moduleEnabled("windows"))
        return;
    if (packageFamily().isEmpty()) {
        log("Reading Windows notifications requires the MSIX installation launched from Start. Portable "
            "monitoring and startup notices remain available.");
        return;
    }
    const QString helper = QCoreApplication::applicationDirPath() + "/helpers/WHAT.WindowsHelper.exe";
    if (!QFileInfo::exists(helper)) {
        const QString message =
            "helpers/WHAT.WindowsHelper.exe is missing from the application folder. Build the Windows bridge "
            "with build-windows-bridge.cmd, then rerun CMake to copy it into the Qt build. Notification "
            "access also requires the optional packaged installation launched from the Start menu.";
        log(message);
        if (!testMode)
            QMessageBox::information(nullptr, "Notification helper missing", message);
        return;
    }
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        log(status == QProcess::NormalExit && code == 0
                ? "Notification access request completed. Check Windows / Notifications collection status."
                : QString("Notification helper exited unsuccessfully (%1).").arg(code));
        process->deleteLater();
        sampleWindows();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, helper](QProcess::ProcessError error) {
        log("Notification helper error: " + process->errorString() + " (" + helper + ")");
        if (error == QProcess::FailedToStart)
            process->deleteLater();
    });
    process->start(helper, {"--request-notifications"});
}

void Controller::enableNativeEvents(quintptr window) {
    if (moduleEnabled("windows"))
        audioMonitor.start(reinterpret_cast<HWND>(window));
    nativeWindow = window;
    qApp->installNativeEventFilter(this);
    powerNotification = RegisterPowerSettingNotification(
        reinterpret_cast<HWND>(window), &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
    WTSRegisterSessionNotification(reinterpret_cast<HWND>(window), NOTIFY_FOR_THIS_SESSION);
}

bool Controller::nativeEventFilter(const QByteArray &, void *message, qintptr *) {
    if (!moduleEnabled("windows"))
        return false;
    const auto *msg = static_cast<MSG *>(message);
    if (msg->message == WM_DISPLAYCHANGE || msg->message == WM_DEVICECHANGE) {
        displayClock.invalidate();
        pollDisplays();
    }
    if (msg->message == WM_POWERBROADCAST && msg->wParam == PBT_POWERSETTINGCHANGE) {
        const auto *setting = reinterpret_cast<POWERBROADCAST_SETTING *>(msg->lParam);
        if (setting->PowerSetting == GUID_CONSOLE_DISPLAY_STATE && setting->DataLength == sizeof(DWORD)) {
            DWORD state;
            memcpy(&state, setting->Data, sizeof(state));
            displayState = state != 0;
            publishEvent({"app_display_on"});
            emit sensorsChanged();
        }
    }
    auto patch = [this](const QString &id, QJsonValue value) {
        if (!catalog.contains(id))
            return;
        auto row = catalog.value(id);
        row["value"] = value;
        row["available"] = true;
        catalog[id] = row;
        eventVersions[id] = ++eventCounter;
    };
    if (msg->message == AudioMonitor::VolumeMessage) {
        patch("win_audio_volume", double(msg->wParam) / 10);
        patch("win_audio_muted", bool(msg->lParam));
        publishEvent({"win_audio_volume", "win_audio_muted"});
        emit sensorsChanged();
    }
    if (msg->message == AudioMonitor::DeviceMessage) {
        audioMonitor.refresh();
        sampleWindows();
    }
    if (msg->message == WM_WTSSESSION_CHANGE && settings.value("sources/session", true).toBool()) {
        if (msg->wParam == WTS_SESSION_LOCK || msg->wParam == WTS_SESSION_UNLOCK) {
            patch("win_locked", msg->wParam == WTS_SESSION_LOCK);
            publishEvent({"win_locked"});
            emit sensorsChanged();
        }
    }
    if (msg->message == WM_DEVICECHANGE || msg->message == WM_WTSSESSION_CHANGE) {
        ++collectionGeneration;
        refreshSlow = true;
        inventorySchedule.invalidate();
        sampleWindows();
    }
    return false;
}
