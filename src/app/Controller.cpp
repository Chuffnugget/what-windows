#include "app/Architecture.h"
#include "app/UiMotion.h"
#include "app/StartupRegistration.h"
#include "app/SettingsStore.h"
#include "SoftwareVersions.h"
#include "app/Controller.h"
#include "app/DiagnosticReport.h"
#include "app/ConfigurationFile.h"
#include <QSysInfo>
#include <QDesktopServices>
#include "app/PackageIdentity.h"
#include <windows.h>
#include <appmodel.h>
#include <vector>
#include <utility>
#include <wtsapi32.h>
#include <dbt.h>
#include <QtConcurrent>
#include <QProcess>
#include <QJsonArray>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QTcpServer>
#include <QUuid>
#include <QSslConfiguration>
#include <QSslSocket>
#include <openssl/crypto.h>
#include <QDebug>

namespace {
QString secureServerName() {
    // Called before QWebSocketServer constructs any TLS objects. Do not reselect
    // an already-active backend (Qt warns even when it is the same backend).
    if (QSslSocket::activeBackend() != "openssl" && !QSslSocket::setActiveBackend("openssl"))
        throw std::runtime_error(
            "Could not select the OpenSSL TLS backend. Keep the bundled TLS plugins and DLLs together.");
    return "WHAT";
}
} // namespace

Controller::Controller(const QString &dir, bool testing, QObject *parent)
    : QObject(parent), directory(dir), testMode(testing),
      settings(SettingsStore::profilePath(dir, testing), SettingsStore::format()),
      server((secureServerName(), this)), discovery(this) {
    settingsFlushTimer.setSingleShot(true);
    settingsFlushTimer.setInterval(200);
    connect(&settingsFlushTimer, &QTimer::timeout, this, &Controller::flushSettingsAsync);
    settingsFlushJob.onStatus = [this](const QString &error) {
        if (!error.isEmpty())
            log("Settings persistence failed: " + error);
        if (settingsFlushPending)
            settingsFlushTimer.start();
    };
    uiRefreshTimer.setSingleShot(true);
    uiRefreshTimer.setInterval(100);
    connect(this, &Controller::changed, this, [this] {
        if (!uiRefreshTimer.isActive())
            uiRefreshTimer.start();
    });
    connect(&uiRefreshTimer, &QTimer::timeout, this, [this] {
        uiInfoValid = false;
        emit uiChanged();
    });
    diagnosticsRefreshTimer.setInterval(1000);
    connect(&diagnosticsRefreshTimer, &QTimer::timeout, this, [this] {
        diagnosticsDirty = true;
        emit diagnosticsChanged();
    });
    snapshotTimer.setSingleShot(true);
    snapshotTimer.setInterval(50);
    connect(&snapshotTimer, &QTimer::timeout, this, &Controller::dispatchSnapshot);
    snapshotJob.onStatus = [this](const QString &error) {
        if (!error.isEmpty()) {
            log("Snapshot preparation failed: " + error);
            snapshotPending = true;
            snapshotTimer.start();
        }
    };
    const auto importMessage = ConfigurationFile::applyPending(settings);
    if (!testMode) {
        QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      QSettings::NativeFormat);
        // Early versions did not save the default. Preserve tray startup for
        // existing autostart users; fresh installations still open their window.
        if (!settings.contains("start_minimised") &&
            StartupRegistration::isWhatCommand(run.value("WHAT").toString(), packageFamily()))
            settings.setValue("start_minimised", true);
        refreshStartup();
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && !startupBusy)
                refreshStartup();
        });
    }
    // Pairing-address visibility belongs to the current UI session.
    settings.remove("hide_connection");
    homeAssistantWeb = new HomeAssistantWeb(settings, directory, testMode, this);
    connect(homeAssistantWeb, &HomeAssistantWeb::configurationChanged, this, &Controller::scheduleSettingsFlush);
    connect(homeAssistantWeb, &HomeAssistantWeb::configurationChanged, this, &Controller::changed);
    connect(homeAssistantWeb, &HomeAssistantWeb::browserChanged, this, &Controller::changed);
    connect(homeAssistantWeb, &HomeAssistantWeb::diagnostic, this, &Controller::log);
    savedColours = new ColourPalette(settings, this);
    connect(savedColours, &ColourPalette::changed, this, &Controller::scheduleSettingsFlush);
    appClock.start();
    startAppMonitoring();
    refreshPublishingPolicies();
    browserJob.onStatus = [this](const QString &error) {
        if (!error.isEmpty()) {
            browserPending = false;
            log("Sensor browser preparation failed: " + error);
            emit browserLoadingChanged();
        }
    };
    browserSearchJob.onStatus = browserJob.onStatus;
    browserRefreshTimer.setInterval(200);
    connect(&browserRefreshTimer, &QTimer::timeout, this, [this] {
        if (browserPending && browserVisible && !browserInteracting) {
            refreshBrowserAsync();
        }
    });
    browserRefreshTimer.start();
    metadataTimer.setSingleShot(true);
    metadataTimer.setInterval(500);
    connect(&metadataTimer, &QTimer::timeout, this, &Controller::persistCatalog);
    sensorTray = new SensorTrayManager(settings.fileName(), testing, this);
    connect(sensorTray, &SensorTrayManager::configurationChanged, this, &Controller::scheduleSettingsFlush);
    connect(sensorTray->taskbarIcons(), &TaskbarIconManager::configurationChanged, this,
            &Controller::scheduleSettingsFlush);
    connect(&sensorTrayTimer, &QTimer::timeout, this, [this] {
        QSet<QString> trayIds;
        for (auto item : sensorTray->entries()) {
            const auto row = item.toMap();
            const auto sensor = row.value("sensor", row["id"]).toString();
            if (!sensor.isEmpty())
                trayIds.insert(sensor);
        }
        for (auto item : sensorTray->taskbarIcons()->entries()) {
            const auto sensor = item.toMap()["sensor"].toString();
            if (!sensor.isEmpty())
                trayIds.insert(sensor);
        }
        if (trayIds.isEmpty() && !sensorTray->browsing)
            return;
        if (trayJob.isRunning())
            return;
        const bool browsing = sensorTray->browsing;
        const auto build = browsing ? captureBrowserInput(true) : std::function<BrowserInput()>{};
        const auto selectedBuild = captureReadings(&trayIds);
        const auto entries = sensorTray->entries();
        const auto icons = sensorTray->taskbarIcons()->entries();
        trayJob.submit(
            [build, selectedBuild, browsing] {
                if (browsing)
                    return build().rows;
                QVariantList rows;
                for (const auto &value : selectedBuild())
                    rows.append(value.toObject().toVariantMap());
                return rows;
            },
            [this, entries, icons, browsing](QVariantList rows) {
                if (entries == sensorTray->entries() && icons == sensorTray->taskbarIcons()->entries() &&
                    browsing == sensorTray->browsing)
                    sensorTray->setReadings(rows, browsing);
            });
    });
    sensorTrayTimer.start(250);
    connect(&wallpaperTimer, &QTimer::timeout, this, &Controller::refreshWallpaper);
    wallpaperTimer.start(15000);
    initBeacn();
    QTimer::singleShot(0, this, &Controller::refreshWallpaper);
    if (!testMode)
        QTimer::singleShot(0, this, &Controller::ensureSpecs);
    auto refreshAddresses = [this] {
        if (addressJob.isRunning())
            return;
        addressJob.submit(
            [] {
                QStringList addresses;
                for (const auto &iface : QNetworkInterface::allInterfaces()) {
                    if (!(iface.flags() & QNetworkInterface::IsUp) ||
                        iface.flags() & QNetworkInterface::IsLoopBack)
                        continue;
                    for (const auto &entry : iface.addressEntries())
                        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                            addresses << entry.ip().toString();
                }
                addresses.removeDuplicates();
                return addresses;
            },
            [this](QStringList addresses) {
                if (cachedAddresses != addresses) {
                    cachedAddresses = addresses;
                    emit changed();
                }
            });
    };
    connect(&addressTimer, &QTimer::timeout, this, refreshAddresses);
    addressTimer.start(15000);
    refreshAddresses();
    setupDisplays();
    // Only polls while a shell command is streaming output; see handleCommand.
    commandStreamTimer.setInterval(100);
    connect(&commandStreamTimer, &QTimer::timeout, this, &Controller::flushCommandStream);
    if (!importMessage.isEmpty())
        log(importMessage);
    log("Configuration (QSettings YAML): " + settings.fileName());
    log("Configuration contents: app and module settings, sensor selections, custom sensors, "
        "saved commands and icon settings");
    log("Configuration import (created when scheduled): " + settings.fileName() + ".import.yaml");
    const auto mqttFiles = settings.value("mqtt/options").toMap();
    for (const auto &entry : {std::pair{"ca", "MQTT CA certificate"},
                              std::pair{"certificate", "MQTT client certificate"},
                              std::pair{"key", "MQTT private key"}}) {
        const auto path = mqttFiles.value(entry.first).toString();
        if (!path.isEmpty())
            log(QString::fromLatin1(entry.second) + ": " + QFileInfo(path).absoluteFilePath());
    }
    log("Executable: " + QCoreApplication::applicationFilePath());
    log("Profile: " + directory);
    log("Architecture: " + Architecture::name());
    for (const auto &reason : Architecture::unavailableModules())
        log(reason.toString());
    log("Worker threads: storage, Windows sensors, slow sources, HWiNFO, commands and WebSocket "
        "communications");
    connect(&server, &NetworkService::commandRequested, this, &Controller::handleCommand);
    initCustomSources();
    initAnnouncements();
    connect(&coretempTimer, &QTimer::timeout, this, &Controller::pollCoreTemp);
    coretempTimer.start(qBound(500, settings.value("modules/coretemp/poll_ms", 2000).toInt(), 60000));
    coretempJob.onStatus = [this](QString error) {
        workerStatus("Core Temp reader", error);
        if (!error.isEmpty())
            acceptCoreTemp({{"connected", false}, {"status", error}});
    };
    connect(&nvidiaTimer, &QTimer::timeout, this, &Controller::pollNvidia);
    nvidiaTimer.start(qBound(500, settings.value("modules/nvidia/poll_ms", 2000).toInt(), 60000));
    nvidiaJob.onStatus = [this](QString error) { workerStatus("NVIDIA NVML", error); };
    pollNvidia();
    connect(&icueTimer, &QTimer::timeout, this, &Controller::pollIcue);
    icueTimer.start(2000);
    inventoryJob.onStatus = [this](QString error) {
        workerStatus("Storage inventory", error);
        if (!error.isEmpty()) {
            inventoryClock.invalidate();
            publish();
            emit sensorsChanged();
        }
    };
    windowsJob.onStatus = [this](QString error) {
        workerStatus("Windows sensors", error);
        if (!error.isEmpty()) {
            windowsClock.invalidate();
            publish();
            emit sensorsChanged();
        }
    };
    slowJob.onStatus = [this](QString error) {
        workerStatus("Slow Windows sources", error);
        if (!error.isEmpty()) {
            windowsSlowClock.invalidate();
            publish();
            emit sensorsChanged();
        }
    };
    hwinfoJob.onStatus = [this](QString error) {
        workerStatus("HWiNFO reader", error);
        if (!error.isEmpty()) {
            latest.valid = false;
            latest.status = error;
            publish();
            emit sensorsChanged();
        }
    };
    certificateJob.onStatus = [this](QString error) {
        workerStatus("TLS certificate renewal", error);
        if (!error.isEmpty()) {
            settingsStatus = "Secure identity unavailable: " + error;
            if (!tlsReady)
                serverStatus = settingsStatus;
            emit changed();
        }
    };
    // One-off fix for beta profiles: early betas stored these filters as the
    // string "false", which QML treated as true. Reset them once.
    if (!settings.value("browser/boolean_encoding_v2", false).toBool()) {
        settings.setValue("browser/selected", false);
        settings.setValue("browser/favourites", false);
        settings.setValue("browser/boolean_encoding_v2", true);
        settings.sync();
    }
    connect(this, &Controller::sensorsChanged, this, [this] {
        browserPending = true;
        ++browserGeneration;
        if (!browserResultsReady)
            emit browserLoadingChanged();
    });
    for (const auto &id : settings.value("favourites").toStringList())
        favourites.insert(id);
    // Folding belongs to this session. Every newly discovered group starts closed.
    settings.remove("collapsed");
    settings.remove("icue/led_groups");
    // Searches don't persist between runs (early betas saved them).
    settings.remove("browser/query");
    const auto savedCatalog = QJsonDocument::fromJson(settings.value("catalog").toByteArray()).array();
    windows->restoreVolumes(savedCatalog);
    inventory->restoreVolumes(savedCatalog);
    for (const auto &value : savedCatalog) {
        auto row = value.toObject();
        row["value"] = QJsonValue::Null;
        row["available"] = false;
        catalog.insert(row["id"].toString(), row);
    }
    deviceId = settings.value("device_id").toString();
    if (deviceId.isEmpty()) {
        deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue("device_id", deviceId);
    }
    if (testMode && settings.contains("test_token"))
        token = settings.value("test_token").toString();
    else if (!testMode && settings.contains("token")) {
        if (!settings.contains("connections/installations")) {
            try {
                token = QString::fromLatin1(
                    unprotectSecret(QByteArray::fromBase64(settings.value("token").toByteArray())));
            } catch (const std::exception &) {
                // Preserve the encrypted credential for recovery. The installation
                // stays disconnected until its token is explicitly replaced.
                log("Pairing token could not be read. Use Replace Token in Pair This PC, then "
                    "reconfigure the existing Home Assistant entry.");
            }
        }
    } else
        generateToken();
    initialiseInstallations();
    for (const auto &s : settings.value("selected").toStringList())
        selected.insert(s);
    if (!QSslSocket::supportsSsl())
        throw std::runtime_error(
            "TLS unavailable. Keep the bundled OpenSSL DLLs and tls plugins beside WHAT.exe.");
    auto acceptIdentity = [this](const TlsIdentity &identity) {
        fingerprint = identity.fingerprint;
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setLocalCertificate(identity.certificate);
        ssl.setPrivateKey(identity.key);
        ssl.setProtocol(QSsl::TlsV1_2OrLater);
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone); // Clients authenticate using the application token.
        tlsConfiguration = ssl;
        if (QDateTime::currentDateTimeUtc().daysTo(identity.certificate.expiryDate()) < 90)
            log("Home Assistant TLS certificate expires soon or has expired. Renew in Settings and update the "
                "fingerprint in Home Assistant.");
        server.configure(deviceId, token, ssl);
        tlsReady = true;
    };
    if (testMode)
        acceptIdentity(TlsIdentity::loadOrCreate(directory, true));
    else
        certificateJob.submit([path = directory] { return TlsIdentity::loadOrCreate(path, false); },
                              [this, acceptIdentity](TlsIdentity identity) {
                                  acceptIdentity(identity);
                                  if (settings.value("service_enabled", true).toBool())
                                      startService();
                                  emit changed();
                              });
    connect(&server, &NetworkService::listenFinished, this,
            [this](quint64, bool ok, quint16 port, QString error) {
                if (!settings.value("service_enabled", true).toBool())
                    return;
                const auto desired = qBound(1024, settings.value("port", 48765).toInt(), 65535);
                if (port != desired) {
                    server.close();
                    server.listenAsync(QHostAddress::Any, quint16(desired));
                    return;
                }
                if (!ok) {
                    serverStatus = QString("Could not start on port %1: %2").arg(port).arg(error);
                    log(serverStatus);
                    if (activePort && port != activePort) {
                        settings.setValue("port", activePort);
                        server.listenAsync(QHostAddress::Any, quint16(activePort));
                    }
                    emit changed();
                    return;
                }
                activePort = port;
                serverStatus = QString("WebSocket server listening · TLS on port %1").arg(port);
                discovery.start(deviceId, port);
                publishTimer.start();
                log(serverStatus);
                emit changed();
            });
    connect(&server, &NetworkService::clientCount, this, [this](int count, quint64 total) {
        if (clientCount > 0 && count == 0)
            connectionWaitingSince = QDateTime::currentMSecsSinceEpoch();
        clientCount = count;
        connections = total;
        log(QString("Home Assistant clients: %1").arg(count));
        emit changed();
    });
    connect(&server, &NetworkService::transmitted, this,
            [this] { lastTransmission = QDateTime::currentDateTimeUtc(); });
    connect(&server, &NetworkService::haCommunication, this, [this](qint64 timestamp) {
        haCommunicationTime = timestamp;
        emit haCommunicationChanged();
    });
    connect(&server, &NetworkService::feedbackSupport, this, [this](bool supported) {
        haFeedbackAvailable = supported;
        emit haCommunicationChanged();
        if (!supported && !haEntityLookup.isEmpty()) {
            haEntityLookup["entities"] = QStringList{};
            haEntityLookup.remove("entries");
            haEntityLookup.remove("sources");
            haEntityLookup["message"] = "Connect Home Assistant with the updated WHAT integration to look up entity IDs.";
            emit haEntityChanged();
        }
    });
    connect(&server, &NetworkService::entityLookupResult, this,
            [this](const QString &reading, const QStringList &entities, const QString &message) {
        if (reading.isEmpty() || haEntityLookup["reading"].toString() != reading)
            return;
        const QVariantMap result{{"reading", reading}, {"entities", entities}, {"message", message}};
        if (haEntityLookup != result) {
            haEntityLookup = result;
            emit haEntityChanged();
        }
    });
    connect(&server, &NetworkService::installationEntityLookup, this,
            [this](const QString &installation, const QString &reading,
                   const QStringList &entities, const QString &message) {
        if (reading.isEmpty() || haEntityLookup["reading"].toString() != reading)
            return;
        auto sources = haEntityLookup.value("sources").toMap();
        sources[installation] = QVariantMap{{"entities", entities}, {"message", message}};
        QStringList all, messages;
        QVariantList entries;
        for (const auto &value : installations()) {
            const auto row = value.toMap();
            if (!row.value("enabled").toBool()) continue;
            const auto result = sources.value(row.value("id").toString()).toMap();
            for (const auto &entity : result.value("entities").toStringList()) {
                entries.append(QVariantMap{{"entity", entity}, {"installation", row.value("name")}});
                if (!all.contains(entity)) all.append(entity);
            }
            if (!result.value("message").toString().isEmpty())
                messages.append(row.value("name").toString() + ": " + result.value("message").toString());
        }
        haEntityLookup = {{"reading", reading}, {"entities", all}, {"entries", entries},
                          {"sources", sources}, {"message", messages.join('\n')}};
        emit haEntityChanged();
    });
    connect(&server, &NetworkService::diagnostic, this, &Controller::log);
    server.setDiagnostics(settings.value("diagnostics/logging", false).toBool());
    connect(&server, &NetworkService::transportState, this, [this](QString transport, QString tls) {
        connectionTransport = transport;
        connectionTls = tls;
        emit changed();
    });
    connect(&discovery, &Discovery::status, this, [this](const QString &s) {
        discoveryStatus = s;
        log(s);
        emit changed();
    });
    connect(&sampleTimer, &QTimer::timeout, this,
            testMode ? &Controller::sample : &Controller::sampleWindows);
    connect(&publishTimer, &QTimer::timeout, this, &Controller::publish);
    activePort = qBound(1024, settings.value("port", testMode ? 18765 : 48765).toInt(), 65535);
    sampleTimer.setInterval(int(qBound(0.5, settings.value("sample_seconds", 1).toDouble(), 60.0) * 1000));
    publishTimer.setInterval(qBound(1, settings.value("publish_seconds", 5).toInt(), 300) * 1000);
    if (settings.value("service_enabled", true).toBool())
        startService();
    else {
        serverStatus = "WebSocket server stopped";
        discoveryStatus = "Discovery stopped";
    }
    sampleTimer.start();
    sample();
    connect(&hwinfoTimer, &QTimer::timeout, this, &Controller::sampleHwinfo);
    hwinfoTimer.setInterval(qBound(100, settings.value("hwinfo_check_ms", 100).toInt(), 10000));
    if (!testMode)
        hwinfoTimer.start();
    log(QString("Streaming: native lock/display/audio events; HWiNFO check interval %1 ms; changes batched "
                "within 50 ms; slow sources scheduled separately")
            .arg(hwinfoTimer.interval()));
    log(QString("WHAT %1 started; HWiNFO source %2; collection interval %3 ms")
            .arg(QCoreApplication::applicationVersion(),
                 settings.value("sources/hwinfo", true).toBool() ? "enabled" : "disabled")
            .arg(sampleTimer.interval()));
    auto *healthLog = new QTimer(this);
    connect(healthLog, &QTimer::timeout, this, [this] {
        // Recheck certificate dates even if no modules or connections are active.
        emit changed();
        log(QString("Health: %1; last read: %2; sample age %3 s; %4 live HWiNFO readings; Windows %5; %6 "
                    "authenticated Home Assistant clients")
                .arg(sourceStatus(), latest.status)
                .arg(age())
                .arg(latest.readings.size())
                .arg(windowsClock.isValid() &&
                             windowsClock.elapsed() < qMax(30000, sampleTimer.interval() * 3)
                         ? "current"
                         : "waiting/stale")
                .arg(clientCount));
    });
    healthLog->start(60000);
    lighting = new DynamicLighting(this);
    connect(lighting, &DynamicLighting::changed, this, [this] {
        const auto status = lighting->state()["status"].toString();
        const bool failed = status.contains("failed", Qt::CaseInsensitive) ||
                            status.contains("timed out", Qt::CaseInsensitive) ||
                            status.contains("stopped", Qt::CaseInsensitive);
        workerStatus("Dynamic Lighting", failed ? status : QString{});
        emit dynamicLightingChanged();
        publish();
    });
    lighting->setEnabled(
        moduleEnabled("windows") && settings.value("modules/windows/lighting", false).toBool(), testMode);
    settings.sync();
}

Controller::~Controller() {
    // The browser's final PiP position still needs the live settings object.
    homeAssistantWeb->disconnect(this);
    delete homeAssistantWeb;
    homeAssistantWeb = nullptr;
    if (currentBrowserFilters) {
        // Queue the final filter preferences after any older in-flight save. The worker drains
        // accepted writes during shutdown and owns its settings instance.
        settingsFlushJob.post([path = settings.fileName(), filters = *currentBrowserFilters] {
            QSettings output(path, SettingsStore::format());
            for (auto it = filters.cbegin(); it != filters.cend(); ++it)
                if (it.key() != "query")
                    output.setValue("browser/" + it.key(), it.value());
            output.sync();
        });
    }
    settings.sync();
    if (lighting) {
        lighting->disconnect(this);
        delete lighting;
        lighting = nullptr;
    }
    // Revoke queued commands before their workers outlive the GUI during shutdown.
    ++*localIcueEpoch;
    ++*commandEpoch;
    qApp->removeNativeEventFilter(this);
    if (powerNotification)
        UnregisterPowerSettingNotification(powerNotification);
    if (nativeWindow)
        WTSUnRegisterSessionNotification(reinterpret_cast<HWND>(nativeWindow));
    discovery.stop();
    server.close();
}

void Controller::scheduleSettingsFlush() {
    // Do not restart an active timer: continuous edits must still reach disk periodically.
    settingsFlushPending = true;
    if (!settingsFlushTimer.isActive())
        settingsFlushTimer.start();
}

void Controller::flushSettingsAsync() {
    if (!settingsFlushPending || settingsFlushJob.isRunning())
        return;
    settingsFlushPending = false;
    const auto path = settings.fileName();
    const auto filters = currentBrowserFilters;
    settingsFlushJob.submit(
        [path, filters] {
            // QSettings instances share in-process changes; use a worker-owned instance.
            QSettings output(path, SettingsStore::format());
            if (filters)
                for (auto it = filters->cbegin(); it != filters->cend(); ++it)
                    if (it.key() != "query")
                        output.setValue("browser/" + it.key(), it.value());
            output.sync();
            return output.status() == QSettings::NoError;
        },
        [this](bool ok) {
            if (!ok)
                log("Could not persist settings");
            if (settingsFlushPending)
                settingsFlushTimer.start();
        });
}

void Controller::setStartupEnabled(bool enabled) {
    if (!testMode)
        refreshStartup(enabled);
}

void Controller::refreshStartup(std::optional<bool> enabled) {
    if (startupBusy) {
        if (enabled)
            pendingStartup = enabled;
        return;
    }
    startupBusy = true;
    QtConcurrent::run([executable = QCoreApplication::applicationFilePath(), family = packageFamily(), enabled] {
        QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      QSettings::NativeFormat);
        return StartupRegistration::update(run, executable, family, enabled);
    }).then(this, [this](StartupRegistration::State state) {
        startupBusy = false;
        startupActive = state.enabled;
        if (pendingStartup) {
            const auto requested = *pendingStartup;
            pendingStartup.reset();
            if (requested != startupActive) {
                refreshStartup(requested);
                return;
            }
        }
        if (!state.error.isEmpty()) {
            settingsStatus = state.error;
            log(state.error);
        }
        emit changed();
    });
}

void Controller::generateToken() {
    QByteArray bytes(32, Qt::Uninitialized);
    for (int i = 0; i < bytes.size(); i += 4) {
        const quint32 random = QRandomGenerator::system()->generate();
        memcpy(bytes.data() + i, &random, 4);
    }
    token = QString::fromLatin1(bytes.toHex());
    if (testMode)
        settings.setValue("test_token", token);
    else
        settings.setValue("token", protectSecret(token.toLatin1()).toBase64());
    settings.sync();
}

qint64 Controller::age() const {
    return pollClock.isValid() ? initialAge + pollClock.elapsed() / 1000 : -1;
}

bool Controller::healthy() const {
    return settings.value("sources/hwinfo", true).toBool() && latest.valid && age() >= 0 &&
           age() <= qMax(qint64(15), qint64(latest.pollPeriod / 1000) * 3 + sampleTimer.interval() / 1000);
}

QString Controller::sourceStatus() const {
    if (!settings.value("sources/hwinfo", true).toBool())
        return "HWiNFO disabled - Windows monitoring remains active";
    if (!latest.valid)
        return latest.status.isEmpty() ? QString("Waiting for the first HWiNFO sample") : latest.status;
    return healthy() ? QString("HWiNFO connected - %1 readings").arg(latest.readings.size())
                     : "HWiNFO data is stale. Check Sensors and Shared Memory Support in HWiNFO.";
}

QVariantMap Controller::uiInfo() {
    // QML reads this property from many bindings per notification. Check the
    // selection once per refresh, not once for every control on every page.
    if (uiInfoValid)
        return cachedUiInfo;
    if (!selectionCountJob.isRunning() && (!selectedCountsValid || countedSelection != selected ||
                                           countedMetadataRevision != catalogMetadataRevision)) {
        const auto chosen = selected;
        const auto rows = catalog;
        const auto revision = catalogMetadataRevision;
        selectionCountJob.submit(
            [chosen, rows] {
                QHash<QString, int> counts;
                for (const auto &id : chosen) {
                    if (id.startsWith("app_")) {
                        ++counts["windows"];
                        continue;
                    }
                    const auto it = rows.constFind(id);
                    if (it != rows.cend())
                        ++counts[it.value().value("source").toString()];
                }
                return counts;
            },
            [this, chosen, revision](QHash<QString, int> counts) {
                if (chosen != selected || revision != catalogMetadataRevision) {
                    uiRefreshTimer.start();
                    return;
                }
                countedSelection = chosen;
                countedMetadataRevision = revision;
                selectedSourceCounts = counts;
                selectedCountsValid = true;
                uiInfoValid = false;
                emit uiChanged();
            });
    }
    int count = 0;
    for (auto it = selectedSourceCounts.cbegin(); it != selectedSourceCounts.cend(); ++it)
        if (moduleEnabled(it.key()))
            count += it.value();
    cachedUiInfo = buildInfo(count);
    uiInfoValid = true;
    return cachedUiInfo;
}

QVariantMap Controller::info() const {
    QSet<QString> enabledSources;
    for (auto source : {"windows", "hwinfo", "icue", "coretemp", "nvidia", "beacn", "wallpaper", "custom"})
        if (moduleEnabled(source))
            enabledSources.insert(source);
    int activeSelected = 0;
    for (const auto &id : selected) {
        const auto it = catalog.constFind(id);
        const QString source = id.startsWith("app_")  ? QStringLiteral("windows")
                               : it == catalog.cend() ? QString{}
                                                      : it.value().value("source").toString();
        activeSelected += enabledSources.contains(source);
    }
    return buildInfo(activeSelected);
}

QVariantMap Controller::buildInfo(int activeSelected) const {
    const auto &motion = UiMotion::resolve(settings.value("ui/transition_style").toString());
    const auto filters = browserFilters();
    const auto now = QDateTime::currentDateTimeUtc();
    const auto certificate = tlsConfiguration.localCertificate();
    const auto expiry = certificate.expiryDate();
    const auto days = now.daysTo(expiry);
    QString certificateWarning;
    const bool certificateExpired = !certificate.isNull() && expiry <= now;
    if (!tlsReady && !certificateJob.isRunning())
        certificateWarning = "The Home Assistant certificate could not be loaded. "
                             "Renew it to restore the secure connection.";
    else if (certificateExpired)
        certificateWarning = "The Home Assistant certificate has expired. "
                             "Renew it and update the fingerprint in the existing Home Assistant entry.";
    else if (!certificate.isNull() && certificate.effectiveDate() > now)
        certificateWarning = "The Home Assistant certificate is not valid yet. "
                             "Check the PC's date and time before renewing it.";
    else if (!certificate.isNull() && days < 90)
        certificateWarning = QString("The Home Assistant certificate expires on %1. "
                                     "Renew it and update the fingerprint in the existing Home Assistant entry.")
                                 .arg(expiry.toLocalTime().toString("d MMM yyyy"));
    return {
        {"certificateExpiry", expiry.toString(Qt::ISODate)},
        {"certificateDays", days},
        {"certificateWarning", certificateWarning},
        {"certificateExpired", certificateExpired},
        {"certificateBusy", certificateJob.isRunning()},
        {"packaged", !packageFamily().isEmpty()},
        {"version", QCoreApplication::applicationVersion()},
        {"name", QHostInfo::localHostName()},
        {"deviceId", deviceId},
        {"addresses", cachedAddresses.join(", ")},
        {"connectionTransport", connectionTransport},
        {"connectionTls", connectionTls},
        {"connectionProgress", connectionProgress()},
        {"port", activePort},
        {"configuredPort", settings.value("port", activePort)},
        {"fingerprint", fingerprint},
        {"server", serverStatus},
        {"running", server.isListening()},
        {"settingsStatus", settingsStatus},
        {"discovery", discoveryStatus},
        {"clients", clientCount},
        {"healthy", healthy()},
        {"moduleIssues", moduleIssues()},
        {"wifiIssue", moduleEnabled("windows") && sourceOptions()["wifi"].toBool() ? wifiIssue : QString{}},
        {"architecture", Architecture::name()},
        {"unavailableModules", Architecture::unavailableModules()},
        {"status", sourceStatus()},
        {"age", age()},
        {"selected", activeSelected},
        {"total", catalog.size() + 8},
        {"windowsHealthy", moduleEnabled("windows") && windowsClock.isValid() &&
                               windowsClock.elapsed() < qMax(30000, sampleTimer.interval() * 3)},
        {"sources", sourceOptions().toVariantMap()},
        {"hwinfoEnabled", settings.value("sources/hwinfo", true).toBool()},
        {"browserQuery", filters.value("query", "")},
        {"browserSource", filters.value("source", "all")},
        {"browserSelected", filters.value("selected", false).toBool()},
        {"browserUnshared", filters.value("unshared", false).toBool()},
        {"browserFavourites", filters.value("favourites", false).toBool()},
        {"license", settings.value("license", "unknown")},
        {"startup", startupActive},
        {"startupAvailable", StartupRegistration::supported()},
        {"startupBusy", startupBusy},
        {"startMinimised", settings.value("start_minimised", false).toBool()},
        {"icueDirectEnabled", settings.value("modules/icue/direct", true).toBool()},
        {"icueAutomationEnabled", settings.value("modules/icue/automation", false).toBool()},
        {"icueAutomationControl", settings.value("modules/icue/automationControl", false).toBool()},
        {"icueAutomationStatus", automationData["status"].toString()},
        {"icueAutomationTelemetryStatus", automationData["telemetry_status"].toString()},
        {"coretempEnabled", moduleEnabled("coretemp")},
        {"coretempConnected", coretempHealthy()},
        {"coretempStatus",
         moduleEnabled("coretemp") ? coretempData["status"].toString("Connecting…") : "Disabled"},
        {"coretempPollMs", coretempTimer.interval()},
        {"nvidiaEnabled", moduleEnabled("nvidia")},
        {"nvidiaConnected", nvidiaHealthy()},
        {"nvidiaStatus", moduleEnabled("nvidia") ? nvidiaData["status"].toString("Connecting…") : "Disabled"},
        {"nvidiaPollMs", nvidiaTimer.interval()},
        {"icueConnected", icueData["connected"].toBool()},
        {"icueAutomationConnected", automationData["connected"].toBool()},
        {"animationsEnabled", settings.value("ui/animations", true).toBool()},
        {"transitionStyle", motion.id},
        {"transitionMode", motion.mode},
        {"transitionStyles", UiMotion::choices()},
        {"refractionStrength", qBound(0, settings.value("ui/refraction_strength", 100).toInt(), 150)},
        {"appControlEnabled", settings.value("controls/what", false).toBool()},
        {"diagnosticLogging", settings.value("diagnostics/logging", false).toBool()},
        {"crashDumps", settings.value("diagnostics/dumps", false).toBool()},
        {"windowAnimationsEnabled", settings.value("ui/window_animations", true).toBool()},
        {"icueEnabled", moduleEnabled("icue")},
        {"icueControl", settings.value("modules/icue/control", false).toBool()},
        {"icueStatus", icueData["status"].toString()},
        {"windowsEnabled", moduleEnabled("windows")},
        {"controlPermissions", controlPermissions().toVariantMap()},
        {"hwinfoCheckMs", hwinfoTimer.interval()},
        {"sampleSeconds", sampleTimer.interval() / 1000.0},
        {"publishSeconds", publishTimer.interval() / 1000},
        {"publishingNormalMs", settings.value("publishingNormalMs", 1000)},
        {"publishingHighMs", settings.value("publishingHighMs", 100)},
        {"testMode", testMode},
        {"startupNotice", settings.value("startup_notice", true).toBool()}};
}

QVariantList Controller::sensors() const {
    QVariantList result;
    for (const auto &value : allReadings()) {
        auto row = value.toObject().toVariantMap();
        row["enabled"] = selected.contains(row["id"].toString());
        const auto policy = publishingPolicies[row["id"].toString()].toObject().toVariantMap();
        row["shareScope"] = row["enabled"].toBool() ? policy.value("scope", 1).toInt() : 0;
        row["highFrequency"] = policy.value("high", false);
        row["decimals"] = policy.value("decimals", -1);
        row["favourite"] = favourites.contains(row["id"].toString());
        result.append(row);
    }
    return result;
}

void Controller::sample() {
    sampleHwinfo();
    sampleWindows();
}

void Controller::sampleHwinfo() {
    if (settings.value("sources/hwinfo", true).toBool()) {
        if (!hwinfoJob.isRunning()) {
            lastHwinfoAttempt = QDateTime::currentDateTimeUtc();
            const auto generation = collectionGeneration;
            const bool testing = testMode;
            const auto previousReadings = latest.readings;
            const bool wasValid = latest.valid;
            const auto prepare = prepareReadingsUpdate("hwinfo");

            struct PollResult {
                HwinfoSnapshot snapshot;
                std::optional<ReadingsUpdate> update;
                bool changed;
            };

            hwinfoJob.submit(
                [testing, previousReadings, wasValid, prepare] {
                    thread_local HwinfoReader cachedReader;
                    auto snapshot = cachedReader.read(testing);
                    const bool changed = previousReadings != snapshot.readings || wasValid != snapshot.valid;
                    std::optional<ReadingsUpdate> update;
                    if (changed && snapshot.valid)
                        update = prepare(snapshot.readings);
                    return PollResult{std::move(snapshot), std::move(update), changed};
                },
                [this, generation](PollResult result) {
                    if (generation != collectionGeneration) {
                        hwinfoJob.post([result = std::move(result)] {});
                        return;
                    }
                    const auto previous = sourceStatus();
                    const bool readingsChanged = result.changed;
                    std::swap(latest, result.snapshot);
                    if (latest.valid) {
                        lastHwinfoSuccess = QDateTime::currentDateTimeUtc();
                        if (result.update)
                            applyReadingsUpdate("hwinfo", *result.update);
                        if (latest.pollTime != lastPoll || !pollClock.isValid()) {
                            lastPoll = latest.pollTime;
                            initialAge = qMax(qint64(0), QDateTime::currentSecsSinceEpoch() - lastPoll);
                            pollClock.restart();
                        }
                    }
                    if (previous != sourceStatus())
                        log(sourceStatus());
                    if (readingsChanged || previous != sourceStatus()) {
                        publish();
                        emit sensorsChanged();
                    }
                    // The old snapshot and catalogue pages can contain thousands of rows.
                    hwinfoJob.post([result = std::move(result)] {});
                });
        }
    } else {
        latest.valid = false;
        latest.status = "Disabled";
    }
}

void Controller::restoreSharingScope(const QString &id) {
    auto policy = publishingPolicies.value(id).toObject();
    if (policy.contains("scope") && policy.value("scope").toInt() == 0) {
        policy["scope"] = 1;
        settings.setValue("publishing/" + id, policy.toVariantMap());
        publishingPolicies[id] = policy;
    }
}

void Controller::persistSelection() {
    settings.setValue("selected", QStringList(selected.begin(), selected.end()));
    settings.setValue("selection_initialized", true);
    settings.setValue("windows_selection_initialized", true);
    scheduleSettingsFlush();
    log(QString("Sensor sharing selection saved: %1 readings selected").arg(selected.size()));
    emit sensorsChanged();
    emit changed();
    publishPolicy();
    if (browserVisible && !browserInteracting)
        refreshBrowserAsync();
}

void Controller::setSensorPublishing(const QString &id, int scope, bool high, int decimals) {
    if (scope < 0 || scope > 3 || decimals < -1 || decimals > 6)
        return;
    const QSet<QString> requested{id};
    if (!catalog.contains(id) && allReadings(&requested).isEmpty())
        return;
    const QJsonObject policy{{"scope", scope}, {"high", high}, {"decimals", decimals}};
    settings.setValue("publishing/" + id, policy.toVariantMap());
    publishingPolicies[id] = policy;
    if (scope)
        selected.insert(id);
    else
        selected.remove(id);
    persistSelection();
}

void Controller::setPublishingIntervals(int normalMs, int highMs) {
    if (normalMs < 100 || normalMs > 60000 || highMs < 50 || highMs > normalMs)
        return;
    settings.setValue("publishingNormalMs", normalMs);
    settings.setValue("publishingHighMs", highMs);
    scheduleSettingsFlush();
    publishPolicy();
}

void Controller::setSensorEnabled(const QString &id, bool enabled) {
    if (enabled)
        restoreSharingScope(id);
    if (enabled)
        selected.insert(id);
    else
        selected.remove(id);
    persistSelection();
}

void Controller::selectSensors(const QString &mode) {
    selected.clear();
    for (const auto &value : allReadings()) {
        const auto r = value.toObject();
        if (mode == "all" || (mode == "recommended" && r["recommended"].toBool())) {
            const auto id = r["id"].toString();
            selected.insert(id);
            restoreSharingScope(id);
        }
    }
    persistSelection();
}

void Controller::saveSettings(double sampling, int publishing, int port, const QString &license,
                              bool startupEnabled) {
    settings.setValue("sample_seconds", qBound(0.5, sampling, 60.0));
    settings.setValue("publish_seconds", qBound(1, publishing, 300));
    port = qBound(1024, port, 65535);
    const auto oldPort = activePort;
    const bool wasRunning = server.isListening();
    bool portAccepted = true;
    if (wasRunning && port != oldPort) {
        QTcpServer probe;
        if (!probe.listen(testMode ? QHostAddress::LocalHost : QHostAddress::Any, quint16(port))) {
            settingsStatus =
                QString("Port %1 is unavailable. Still running on port %2; other settings saved.")
                    .arg(port)
                    .arg(oldPort);
            portAccepted = false;
        } else {
            probe.close();
            stopService();
            settings.setValue("port", port);
            if (!startService()) {
                settings.setValue("port", oldPort);
                const bool restored = startService();
                settingsStatus =
                    restored
                        ? QString("Could not use port %1. Restored port %2.").arg(port).arg(oldPort)
                        : "Could not restart the server. Check Diagnostics, choose another port and Start.";
                portAccepted = false;
            }
        }
    }
    if (portAccepted) {
        settings.setValue("port", port);
        if (!wasRunning)
            activePort = port;
        settingsStatus =
            port != oldPort && wasRunning
                ? QString("Port %1 applied. Reconfigure this PC's port in Home Assistant.").arg(port)
                : "Settings saved and applied";
    }
    settings.setValue("license",
                      QStringList{"unknown", "free", "pro"}.contains(license) ? license : "unknown");
    if (!testMode && !startupBusy && startupEnabled != startupActive)
        refreshStartup(startupEnabled);
    settings.sync();
    sampleTimer.setInterval(qBound(0.5, sampling, 60.0) * 1000);
    publishTimer.setInterval(qBound(1, publishing, 300) * 1000);
    log(settingsStatus);
    emit changed();
    publish();
}

void Controller::copyPairing() {
    const auto entries = settings.value("connections/installations").toList();
    if (!entries.isEmpty()) {
        copyInstallationPairing(entries.first().toMap().value("id").toString());
        return;
    }

}

void Controller::copyToken() {
    const auto entries = settings.value("connections/installations").toList();
    if (!entries.isEmpty()) {
        copyInstallationToken(entries.first().toMap().value("id").toString());
        return;
    }

}

void Controller::copyFingerprint() {
    QApplication::clipboard()->setText(fingerprint);
}

void Controller::renewCertificate() {
    if (certificateJob.isRunning())
        return;
    const auto path = directory;
    const bool testing = testMode;
    certificateJob.submit(
        [path, testing] {
            try {
                auto identity = TlsIdentity::renew(path, testing);
                return qMakePair(identity, QString());
            } catch (const std::exception &e) {
                return qMakePair(TlsIdentity{QSslCertificate(), QSslKey(), QString()},
                                 QString::fromUtf8(e.what()));
            }
        },
        [this](QPair<TlsIdentity, QString> result) {
            if (!result.second.isEmpty()) {
                log("Certificate renewal failed: " + result.second);
                settingsStatus = result.second;
                emit changed();
                emit certificateRenewalFinished(false, settingsStatus);
                return;
            }
            // Respect the current service preference, including a startup that
            // could not listen because loading the previous identity failed.
            const bool shouldRun = settings.value("service_enabled", true).toBool();
            if (server.isListening() || server.isStarting())
                stopService();
            tlsConfiguration.setLocalCertificate(result.first.certificate);
            tlsConfiguration.setPrivateKey(result.first.key);
            tlsConfiguration.setProtocol(QSsl::TlsV1_2OrLater);
            tlsConfiguration.setPeerVerifyMode(QSslSocket::VerifyNone);
            fingerprint = result.first.fingerprint;
            tlsReady = true;
            server.configure(deviceId, token, tlsConfiguration);
            if (shouldRun)
                startService();
            settingsStatus = "Certificate renewed. Update the certificate fingerprint in Home Assistant; "
                             "token and PC identity are unchanged.";
            log(settingsStatus);
            emit changed();
            emit certificateRenewalFinished(true, settingsStatus);
        });
    emit changed();
}

void Controller::log(const QString &message) {
    const auto module = DiagnosticReport::moduleForMessage(message);
    logLines.append(QDateTime::currentDateTime().toString(Qt::ISODate) +
                    QString(" [WHAT][%1][PID %2] ").arg(module).arg(GetCurrentProcessId()) + message);
    emit activityRecorded(module, message);
    qInfo().noquote() << logLines.last();
    while (logLines.size() > 500)
        logLines.removeFirst();
    emit logChanged();
}

void Controller::workerStatus(const QString &name, const QString &error) {
    const auto previous = workerErrors.value(name);
    if (previous == error)
        return;
    if (error.isEmpty()) {
        workerErrors.remove(name);
        log(name + " recovered");
    } else {
        workerErrors[name] = error;
        log(name + " failed: " + error);
    }
    emit changed();
}

void Controller::openDiagnosticFolder() {
    const auto folder = directory + "/diagnostics";
    QDir().mkpath(folder);
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void Controller::prepareDiagnosticReport() {
    if (reportBusy)
        return;
    reportBusy = true;
    reportText.clear();
    emit diagnosticReportChanged();
    const auto readings = captureReadings();
    const auto details = captureDiagnostics(false);
    const auto versions = captureDiagnosticVersions();
    const auto selected = this->selected;
    QSet<QString> enabledSources;
    for (auto source : {"windows", "hwinfo", "icue", "beacn", "coretemp", "nvidia", "wallpaper", "custom"})
        if (moduleEnabled(source))
            enabledSources.insert(source);
    const bool directEnabled = icueDirectEnabled(), automationEnabled = icueAutomationEnabled();
    const auto icueData = this->icueData, automationData = this->automationData,
               displayInfo = this->displayInfo;
    QJsonObject safeSettings;
    for (const auto &key : settings.allKeys()) {
        if (!(key.startsWith("modules/") || key.startsWith("sources/") || key == "port" ||
              key == "sample_seconds" || key == "publish_seconds" || key == "hwinfo_check_ms" ||
              key == "ui/animations"))
            continue;
        const auto value = settings.value(key);
        const auto type = value.metaType().id();
        if (type == QMetaType::Bool || type == QMetaType::Int || type == QMetaType::Double)
            safeSettings[key] = QJsonValue::fromVariant(value);
    }
    QJsonArray privacy{token,
                       fingerprint,
                       deviceId,
                       directory,
                       QCoreApplication::applicationFilePath(),
                       qEnvironmentVariable("USERNAME"),
                       qEnvironmentVariable("COMPUTERNAME"),
                       qEnvironmentVariable("USERPROFILE"),
                       settings.value("remote/url").toString()};
    for (auto item : nvidiaData["devices"].toArray())
        privacy.append(item.toObject()["uuid"]);
    QJsonObject state{{"Core Temp enabled", moduleEnabled("coretemp")},
                      {"Core Temp", coretempData["status"]},
                      {"HWiNFO", sourceStatus()},
                      {"Windows enabled", moduleEnabled("windows")},
                      {"iCUE enabled", moduleEnabled("icue")},
                      {"iCUE SDK", icueData["status"]},
                      {"iCUE Automation", automationData["status"]},
                      {"NVIDIA enabled", moduleEnabled("nvidia")},
                      {"NVIDIA", nvidiaData["status"]},
                      {"WebSocket", serverStatus},
                      {"Home Assistant clients", clientCount},
                      {"Home Assistant", connectionProgress().value("detail").toString()}};
    QJsonObject snapshot{
        {"system", QJsonObject{{"WHAT version", QCoreApplication::applicationVersion()},
                               {"Qt version", qVersion()},
                               {"OS", QSysInfo::prettyProductName()},
                               {"kernel", QSysInfo::kernelVersion()},
                               {"architecture", QSysInfo::currentCpuArchitecture()},
                               {"generated", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}},
        {"state", state},
        {"settings", safeSettings},
        {"private", privacy},
        {"logs", QJsonArray::fromStringList(logLines)}};
    reportJob.onStatus = [this](QString error) {
        if (!error.isEmpty()) {
            reportBusy = false;
            reportText = "Report generation failed. Try again.";
            emit diagnosticReportChanged();
        }
    };
    // Everything below is a local copy taken on the GUI thread; the explicit list
    // keeps member state (several members share these names) out of the worker.
    reportJob.submit(
        [readings, enabledSources, directEnabled, automationEnabled, selected, icueData, automationData,
         displayInfo, snapshot, details, versions] {
            QJsonArray rows;
            for (auto value : readings()) {
                auto row = value.toObject();
                rows.append(QJsonObject{
                    {"source", row["source"]},
                    {"group", row["group"]},
                    {"name", row["name"]},
                    {"unit", row["unit"]},
                    {"available", row["available"]},
                    {"shared",
                     (row.value("source") == "icue"
                          ? (row.value("icueAutomation").toBool() ? automationEnabled : directEnabled)
                          : enabledSources.contains(row.value("source").toString())) &&
                         selected.contains(row["id"].toString())}});
            }
            QJsonArray devices;
            for (auto item : icueData["devices"].toArray()) {
                auto d = item.toObject();
                devices.append(QJsonObject{{"module", "iCUE RGB SDK"},
                                           {"model", d["model"]},
                                           {"LED count", d["leds"].toArray().size()},
                                           {"topology", d["topology_status"]}});
            }
            for (auto item : automationData["controls"].toArray()) {
                auto d = item.toObject();
                devices.append(QJsonObject{
                    {"module", "iCUE Automation control"}, {"name", d["name"]}, {"kind", d["kind"]}});
            }
            for (auto item : displayInfo["monitors"].toArray()) {
                auto d = item.toObject();
                devices.append(QJsonObject{{"module", "Windows display"},
                                           {"name", d["name"]},
                                           {"connected", d["connected"]},
                                           {"width", d["width"]},
                                           {"height", d["height"]}});
            }
            auto report = snapshot;
            report["readings"] = rows;
            report["devices"] = devices;
            report["details"] = details();
            report["versions"] = versions();
            return DiagnosticReport::build(report);
        },
        [this](QString text) {
            reportText = text;
            reportBusy = false;
            emit diagnosticReportChanged();
        });
}

void Controller::copyLogSnapshot(const QString &text) {
    QApplication::clipboard()->setText(text);
}

void Controller::saveLogSnapshot(const QString &text) {
    const auto snapshot = text.toUtf8();
    const auto path = QFileDialog::getSaveFileName(nullptr, "Save WHAT activity log", "what-activity.txt",
                                                   "Text files (*.txt)");
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(snapshot) != snapshot.size() || !file.commit())
        log("Activity log export failed");
}

void Controller::exportLogs() {
    if (reportBusy || reportText.isEmpty())
        return;
    const auto path = QFileDialog::getSaveFileName(nullptr, "Export WHAT diagnostic report",
                                                   "what-diagnostics.txt", "Text files (*.txt)");
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(reportText.toUtf8()) < 0 || !file.commit())
        log("Diagnostic export failed");
}

QString Controller::uiDiagnostics() {
    if (diagnosticsDirty && !diagnosticsJob.isRunning()) {
        diagnosticsDirty = false;
        diagnosticsJob.submit(captureDiagnostics(), [this](QString text) {
            cachedDiagnostics = text;
            emit diagnosticsChanged();
        });
    }
    return cachedDiagnostics;
}

QString Controller::diagnostics() const {
    return captureDiagnostics()();
}

std::function<QJsonObject()> Controller::captureDiagnosticVersions() const {
    const auto files = beacnData.value("installation").toMap();
    const QMap<QString, QString> paths{
        {"BEACN app", beacnData.value("exe_path", files.value("appPath")).toString()},
        {"BEACN driver", beacnData.value("dll_path", files.value("driverPath")).toString()},
        {"Wallpaper Engine app", wallpaperData["executable"].toString()},
        {"NVIDIA NVML library", nvidiaData["library"].toString()},
        {"iCUE SDK client", QCoreApplication::applicationDirPath() + "/helpers/iCUESDK.x64_2019.dll"},
        {"Windows helper", QCoreApplication::applicationDirPath() + "/helpers/WHAT.WindowsHelper.exe"}};
    QJsonObject versions{{"BEACN API", beacnData.value("api_version").toString()},
                         {"BEACN mixer", beacnData.value("mixer_version").toString()},
                         {"iCUE app", icueData["app_version"]},
                         {"iCUE SDK client", icueData["sdk_version"]},
                         {"iCUE SDK server", icueData["server_version"]},
                         {"iCUE Automation server", automationData["version"]},
                         {"Core Temp shared memory", coretempData["shared_memory_version"]},
                         {"NVIDIA driver", nvidiaData["driver_version"]},
                         {"WebView2 Runtime", homeAssistantWeb->diagnostics()["runtime"].toString()}};
    return [paths, versions]() mutable {
        const auto installed = SoftwareVersions::installed(paths);
        for (auto it = installed.begin(); it != installed.end(); ++it)
            if (versions[it.key()].toVariant().toString().isEmpty())
                versions[it.key()] = it.value();
        for (auto label : {"BEACN app", "BEACN driver", "BEACN API", "BEACN mixer", "HWiNFO app",
                           "Core Temp app", "Core Temp shared memory", "NVIDIA driver", "NVIDIA NVML library",
                           "iCUE app", "iCUE SDK client", "iCUE SDK server", "iCUE Automation server",
                           "Wallpaper Engine app", "Windows helper", "WebView2 Runtime"}) {
            const auto version = versions[label].toVariant().toString();
            versions[label] = version.isEmpty() ? "Not reported" : version;
        }
        return versions;
    };
}

std::function<QString()> Controller::captureDiagnostics(bool includeVersions) const {
    const auto catalog = this->catalog;
    const auto latest = this->latest;
    const auto selected = this->selected;
    const auto workerErrors = this->workerErrors;
    const auto nvidiaData = this->nvidiaData;
    const auto directory = this->directory, serverStatus = this->serverStatus;
    const auto lastHwinfoAttempt = this->lastHwinfoAttempt, lastHwinfoSuccess = this->lastHwinfoSuccess;
    const bool hwinfoReady = healthy(), hwinfoEnabled = settings.value("sources/hwinfo", true).toBool();
    const bool windowsEnabled = moduleEnabled("windows"), windowsValid = windowsClock.isValid();
    const auto status = sourceStatus();
    const auto ageSeconds = age();
    const auto inventoryAge = inventoryClock.isValid() ? inventoryClock.elapsed() / 1000 : -1;
    const bool inventoryBusy = inventoryJob.isRunning();
    auto lastSample = [](const QElapsedTimer &clock) -> QJsonValue {
        return clock.isValid() ? QJsonValue(clock.elapsed() / 1000.0) : QJsonValue();
    };
    auto module = [&](const char *key, QJsonObject data, bool busy, const QElapsedTimer &clock) {
        // Only copy diagnostic fields. Profiles, readings, paths and command contents stay out.
        return QJsonObject{{"enabled", moduleEnabled(key)},
                           {"connected", data["connected"].toBool()},
                           {"status", data["status"]},
                           {"worker busy", busy},
                           {"last sample age (s)", lastSample(clock)}};
    };
    QJsonObject modules{
        {"custom", QJsonObject{{"enabled", moduleEnabled("custom")},
                              {"definitions", customSources->definitions().size()},
                              {"Home Assistant control", customSources->controlEnabled()},
                              {"cloud allowed", customSources->cloudEnabled()}}},
        {"windows", module("windows",
                           {{"connected", windowsClock.isValid()},
                            {"status", windowsClock.isValid() ? "Has samples" : "No samples"}},
                           windowsJob.isRunning(), windowsClock)},
        {"hwinfo", QJsonObject{{"enabled", hwinfoEnabled},
                               {"connected", hwinfoReady},
                               {"status", status},
                               {"worker busy", hwinfoJob.isRunning()},
                               {"last sample age (s)", ageSeconds}}},
        {"coretemp", module("coretemp", coretempData, coretempJob.isRunning(), coretempClock)},
        {"nvidia", module("nvidia", nvidiaData, nvidiaJob.isRunning(), nvidiaClock)},
        {"icue", module("icue", icueData, icueJob.isRunning(), icueClock)},
        {"beacn", QJsonObject{{"enabled", moduleEnabled("beacn")},
                              {"connected", beacnData.value("connected").toBool()},
                              {"ready", beacnData.value("ready").toBool()},
                              {"status", beacnData.value("status").toString()},
                              {"worker busy", beacnModule.busy()},
                              {"Home Assistant control", settings.value("modules/beacn/control", false).toBool()},
                              {"cloud control", settings.value("modules/beacn/cloud", false).toBool()}}},
        {"wallpaper",
         QJsonObject{
             {"enabled", moduleEnabled("wallpaper")},
             {"running", wallpaperData["running"].toBool()},
             {"installed", wallpaperData["installed"].toBool()},
             {"status", wallpaperData["status"]},
             {"worker busy", wallpaperJob.isRunning()},
             {"Home Assistant control", settings.value("modules/wallpaper/control", false).toBool()},
             {"cloud control", settings.value("modules/wallpaper/cloud", false).toBool()},
             {"share wallpaper path", settings.value("modules/wallpaper/share_path", false).toBool()}}}};
    auto browser = homeAssistantWeb->diagnostics();
    browser.remove("host"); // The interactive module shows it; copied reports do not need local hostnames.
    browser.remove("runtime"); // Version numbers use the software-version allowlist.
    modules["Home Assistant Web"] = QJsonObject::fromVariantMap(browser);
    auto win = modules["windows"].toObject();
    win["collection permissions"] = sourceOptions();
    win["Home Assistant control permissions"] = controlPermissions();
    win["media worker busy"] = mediaJob.isRunning();
    win["media sample age (s)"] = lastSample(mediaClock);
    win["slow collector busy"] = slowJob.isRunning();
    win["slow sample age (s)"] = lastSample(windowsSlowClock);
    win["displays"] = QJsonObject{{"enabled", displaysEnabled()},
                                  {"status", displayInfo["status"]},
                                  {"worker busy", displayJob.isRunning()},
                                  {"monitors", displayInfo["monitors"].toArray().size()}};
    const auto lightingState = lighting ? lighting->state() : QJsonObject{};
    win["Dynamic Lighting"] =
        QJsonObject{{"enabled", dynamicLightingState().value("enabled").toBool()},
                    {"Home Assistant control", settings.value("controls/dynamic_lighting", false).toBool()},
                    {"cloud control", settings.value("modules/windows/lighting_cloud", false).toBool()},
                    {"status", lightingState["status"]},
                    {"devices", lightingState["devices"].toArray().size()}};
    modules["windows"] = win;
    auto icue = modules["icue"].toObject();
    icue["RGB enabled"] = icueDirectEnabled();
    icue["RGB Home Assistant control"] = settings.value("modules/icue/control", false).toBool();
    icue["devices"] = icueData["devices"].toArray().size();
    icue["Automation"] =
        QJsonObject{{"enabled", icueAutomationEnabled()},
                    {"connected", automationData["connected"].toBool()},
                    {"status", automationData["status"]},
                    {"Home Assistant control", settings.value("modules/icue/automationControl", false).toBool()},
                    {"worker busy", icueAutomationJob.isRunning()},
                    {"last sample age (s)", lastSample(automationClock)},
                    {"RGB handoff pending", icueRgbHandoff},
                    {"RGB reset pending", icueReset}};
    modules["icue"] = icue;
    QJsonArray destinations;
    for (const auto &value : installations()) {
        const auto row = value.toMap();
        const auto broker = row.value("mqtt").toMap();
        // Do not include endpoints, usernames, paths, tokens or passwords.
        destinations.append(QJsonObject{{"type", row.value("type").toString()},
            {"enabled", row.value("enabled").toBool()}, {"controls", row.value("controls").toBool()},
            {"connected", row.value("connected").toBool()},
            {"state", QJsonObject::fromVariantMap(row.value("state").toMap())},
            {"mqtt_tls", broker.value("tls").toBool()}, {"mqtt_scope", broker.value("scope").toString()}});
    }
    const QJsonObject connections{
        {"installations", destinations}, {"active route", connectionTransport},
        {"active security", connectionTls}, {"WebSocket", serverStatus},
        {"WebSocket listening", server.isListening()}, {"Home Assistant clients", clientCount},
        {"normal publishing interval (ms)", settings.value("publishingNormalMs", 1000).toInt()},
        {"fast publishing interval (ms)", settings.value("publishingHighMs", 100).toInt()}};
    const auto policies = publishingPolicies;
    const auto versions = captureDiagnosticVersions();
    return [=]() mutable {
        int cached = 0, available = 0, selectedCount = 0, missing = 0;
        QSet<QString> liveIds;
        for (const auto &value : latest.readings)
            liveIds.insert(value.toObject()["id"].toString());
        for (const auto &row : catalog) {
            if (row.value("source") != "hwinfo")
                continue;
            const auto id = row.value("id").toString();
            ++cached;
            available += hwinfoReady && row.value("available").toBool();
            selectedCount += selected.contains(id);
            missing += !liveIds.contains(id);
        }
        auto when = [](const QDateTime &t) {
            return t.isValid() ? t.toString(Qt::ISODateWithMs) : QString("Never");
        };
        QString workerDetails;
        for (auto it = workerErrors.cbegin(); it != workerErrors.cend(); ++it)
            workerDetails += "\n" + it.key() + ": " + it.value();
        workerDetails += "\nStorage last successful scan: " +
                         (inventoryAge >= 0 ? QString::number(inventoryAge) + " s ago" : "Never / failed") +
                         (inventoryBusy ? " (scan running)" : "");
        QMap<QString, QJsonObject> counts;
        for (auto it = catalog.cbegin(); it != catalog.cend(); ++it) {
            const auto &row = it.value();
            auto &count = counts[row.value("source").toString()];
            auto increment = [&](const char *key) { count[key] = count[key].toInt() + 1; };
            increment("cached readings");
            if (!selected.contains(it.key()))
                continue;
            increment("selected readings");
            const auto policy = policies[it.key()].toObject();
            const int scope = policy.value("scope").toInt(1);
            if (scope & 1)
                increment("local sharing selected");
            if (scope & 2)
                increment("cloud sharing selected");
            if (policy["high"].toBool())
                increment("fast updates selected");
        }
        auto nvidia = modules["nvidia"].toObject();
        nvidia["errors"] = nvidiaData["errors"];
        QJsonArray gpuDiagnostics;
        for (const auto &value : nvidiaData["devices"].toArray()) {
            auto gpu = value.toObject();
            gpu.remove("uuid");
            gpuDiagnostics.append(gpu);
        }
        nvidia["devices"] = gpuDiagnostics;
        modules["nvidia"] = nvidia;
        QString moduleDetails = "\n\nCONNECTIONS\n" +
                                QString::fromUtf8(QJsonDocument(connections).toJson(QJsonDocument::Indented));
        moduleDetails += "Sharing counts are saved selections, including unavailable readings. "
                         "Disabled modules do not publish. Modules use the common transport above.\n";
        const QMap<QString, QString> names{{"windows", "Windows"},
                                           {"hwinfo", "HWiNFO"},
                                           {"coretemp", "Core Temp"},
                                           {"nvidia", "NVIDIA NVML"},
                                           {"icue", "iCUE RGB and Automation"},
                                           {"beacn", "BEACN"},
                                           {"wallpaper", "Wallpaper Engine"}};
        for (auto it = modules.begin(); it != modules.end(); ++it) {
            auto state = it.value().toObject();
            auto count = counts.value(it.key());
            for (auto key : {"cached readings", "selected readings", "local sharing selected",
                             "cloud sharing selected", "fast updates selected"})
                state[key] = count[key].toInt();
            moduleDetails += "\n" + names.value(it.key()) + "\n" +
                             QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Indented));
        }
        QString versionDetails;
        if (includeVersions)
            versionDetails = "\nSOFTWARE VERSIONS (installation scan cached for up to 60 seconds)\n" +
                             QString::fromUtf8(QJsonDocument(versions()).toJson(QJsonDocument::Indented));
        return QString("WHAT %1 | Qt %2 | Windows %3\n")
                   .arg(QCoreApplication::applicationVersion(), qVersion(), QSysInfo::kernelVersion()) +
               versionDetails + moduleDetails + "\nHWiNFO READ DETAILS\n" +
               QString(
                   "WHAT %1 | PID %2\nExecutable: %3\nProfile: %4\nHWiNFO source: %5 | Availability gate: "
                   "%6\nLast read attempt: %7\nLast successful parse: %8\nRead result: %9\n%10\nHWiNFO "
                   "timestamp: %11 | Age: %12 s | Poll interval: %13 ms\nLive readings: %14 | Saved: %15 | "
                   "Available: %16 | Selected: %17 | Saved IDs absent from last read: %18\nWindows "
                   "collector: %19\nWebSocket: %20")
                   .arg(QCoreApplication::applicationVersion())
                   .arg(QCoreApplication::applicationPid())
                   .arg(QCoreApplication::applicationFilePath(), directory)
                   .arg(hwinfoEnabled ? "Enabled" : "Disabled", status, when(lastHwinfoAttempt),
                        when(lastHwinfoSuccess), latest.status, latest.details)
                   .arg(latest.pollTime)
                   .arg(ageSeconds)
                   .arg(latest.pollPeriod)
                   .arg(latest.readings.size())
                   .arg(cached)
                   .arg(available)
                   .arg(selectedCount)
                   .arg(missing)
                   .arg(!windowsEnabled ? "Module disabled"
                        : windowsValid  ? "Has samples"
                                        : "No samples",
                        serverStatus) +
               "\nWORKERS\n" + workerDetails;
    };
}

void Controller::copyDiagnostics() {
    if (!reportBusy && !reportText.isEmpty())
        QApplication::clipboard()->setText(reportText);
}

QJsonObject Controller::snapshot() const {
    return captureSnapshot()();
}

std::function<QJsonObject()> Controller::captureSnapshot() const {
    auto icueState = icueData;
    icueState["enabled"] = icueDirectEnabled();
    icueState["control_enabled"] =
        icueDirectEnabled() && settings.value("modules/icue/control", false).toBool();
    if (!icueDirectEnabled() || !icueClock.isValid() || icueClock.elapsed() > 15000)
        icueState["connected"] = false;
    const auto buildReadings = captureReadings(&selected);
    const auto selectedIds = selected;
    QSet<QString> enabledSources;
    for (auto source : {"windows", "hwinfo", "nvidia", "coretemp", "beacn", "custom"})
        if (moduleEnabled(source))
            enabledSources.insert(source);
    const bool direct = icueDirectEnabled(), automation = icueAutomationEnabled();
    const QJsonObject header{
        {"_publishing", publishingPolicies},
        {"_default_scope", 1},
        {"_normal_ms", settings.value("publishingNormalMs", 1000).toInt()},
        {"_fast_ms", settings.value("publishingHighMs", 100).toInt()},
        {"type", "snapshot"},
        {"protocol", 2},
        {"device_id", deviceId},
        {"healthy", healthy()},
        {"hwinfo_enabled", settings.value("sources/hwinfo", true).toBool()},
        {"windows_healthy", moduleEnabled("windows") && windowsClock.isValid() &&
                                windowsClock.elapsed() < qMax(30000, sampleTimer.interval() * 3)},
        {"status", sourceStatus()},
        {"sample_time", double(QDateTime::currentSecsSinceEpoch())},
        {"sample_age", double(age())},
        {"publish_seconds", publishTimer.interval() / 1000},
        {"license", settings.value("license", "unknown").toString()},
        {"app", appState()},
        {"entity_cleanup", entityCleanupRequest},
        {"_cleanup_installation", entityCleanupInstallation},

        {"wallpaper", wallpaperShared()},
        {"custom", customSources->shared(true)},
        {"announcements", announcements->state()},
        {"notifications", richNotifications->state()},
        {"beacn", beacnShared()},
        {"dynamic_lighting", lightingShared()},
        {"displays", displaysState(true)},
        {"controls", controlState()},
        {"icue", icueState},
        {"icue_automation", icueAutomationState()}};
    return [=] {
        QJsonArray readings;
        for (const auto &r : buildReadings()) {
            const auto row = r.toObject();
            if (row["source"] == "icue" ? (row["icueAutomation"].toBool() ? automation : direct)
                                        : enabledSources.contains(row["source"].toString()))
                readings.append(r);
        }
        auto frame = header;
        frame["sensors"] = readings;
        frame["selected_sensor_ids"] =
            QJsonArray::fromStringList(QStringList(selectedIds.begin(), selectedIds.end()));
        return frame;
    };
}

void Controller::publish() {
    server.setInterval(publishTimer.interval());
    snapshotPending = true;
    if (!snapshotTimer.isActive())
        snapshotTimer.start();
    emit changed();
}

void Controller::publishEvent(QJsonArray ids) {
    if (snapshotTasks.size() >= 64) {
        log("Snapshot event backlog exceeded; replacing it with current state");
        publishPolicy();
    }
    const auto build = captureSnapshot();
    snapshotTasks.enqueue({[build, ids] {
                               auto frame = build();
                               frame["_event_ids"] = ids;
                               return frame;
                           },
                           true, false, snapshotEpoch});
    dispatchSnapshot();
}

void Controller::dispatchSnapshot() {
    if (snapshotJob.isRunning())
        return;
    if (snapshotTasks.isEmpty()) {
        if (!snapshotPending)
            return;
        snapshotPending = false;
        snapshotTasks.enqueue({captureSnapshot(), false, false, snapshotEpoch});
    }
    const auto task = snapshotTasks.dequeue();
    snapshotJob.submit(task.build, [this, task](QJsonObject frame) {
        // Permission/selection changes invalidate older in-flight captures.
        if (task.epoch == snapshotEpoch) {
            if (task.reset)
                server.replaceSnapshot(frame);
            else if (task.event)
                server.setEventSnapshot(frame);
            else
                server.setSnapshot(frame);
        }
        if (!snapshotTasks.isEmpty() || snapshotPending)
            snapshotTimer.start();
    });
}

void Controller::refreshPublishingPolicies() {
    publishingPolicies = {};
    for (const auto &key : settings.allKeys())
        if (key.startsWith("publishing/")) {
            auto policy = QJsonObject::fromVariantMap(settings.value(key).toMap());
            if (!policy.contains("scope"))
                policy["scope"] = 1;
            publishingPolicies[key.mid(11)] = policy;
        }
}

void Controller::publishPolicy() {
    ++snapshotEpoch;
    snapshotTasks.clear();
    snapshotPending = false;
    // Invalidate the network policy immediately; do not leak stale queued data.
    server.replaceSnapshot({});
    snapshotTasks.enqueue({captureSnapshot(), false, true, snapshotEpoch});
    dispatchSnapshot();
    emit changed();
}

void Controller::addSensorToTray(const QString &id) {
    const QSet<QString> wanted{id};
    QVariantList rows;
    for (auto value : allReadings(&wanted)) {
        auto row = value.toObject();
        if (readingBrowserEnabled(row))
            rows << row.toVariantMap();
    }
    sensorTray->setReadings(rows, false);
    sensorTray->add(id);
}

namespace {
const QVariantMap defaultUiTimings{
    {"hover", 140},
    {"tooltips", 350},
    {"controls", 120},
    {"panels", 250},
    {"lists", 170},
    {"pages", 320},
    {"dialogEnter", 250},
    {"dialogExit", 180},
    {"windowEnter", 400},
    {"windowExit", 300},
    {"logo", 1500},
    {"logoPause", 180},
    {"toast", 3500}
};

int boundedUiTiming(const QString &key, int value) {
    const int minimum = key == "logo" ? 250 : key == "toast" ? 500 : 0;
    const int maximum = key == "logo"        ? 10000
                        : key == "logoPause" ? 5000
                        : key == "toast"     ? 30000
                                             : 2000;
    return qBound(minimum, value, maximum);
}
} // namespace

QString Controller::windowMode() const {
    const auto mode = settings.value("ui/window_mode", "normal").toString();
    return mode == "maximised" || mode == "fullscreen" ? mode : QString("normal");
}

void Controller::setWindowMode(const QString &mode) {
    if ((mode != "normal" && mode != "maximised" && mode != "fullscreen") || mode == windowMode())
        return;
    settings.setValue("ui/window_mode", mode);
    scheduleSettingsFlush();
}

void Controller::setTransitionStyle(const QString &style) {
    if (!UiMotion::find(style))
        return;
    settings.setValue("ui/transition_style", style);
    scheduleSettingsFlush();
    emit changed();
}

void Controller::setRefractionStrength(int percent) {
    settings.setValue("ui/refraction_strength", qBound(0, percent, 150));
    scheduleSettingsFlush();
    emit changed();
}

QVariantMap Controller::uiTimings() const {
    auto values = defaultUiTimings;
    for (auto it = values.begin(); it != values.end(); ++it) {
        bool valid = false;
        const int stored = settings.value("ui/timing/" + it.key(), it.value()).toInt(&valid);
        it.value() = boundedUiTiming(it.key(), valid ? stored : it.value().toInt());
    }
    return values;
}

void Controller::setUiTiming(const QString &key, int milliseconds) {
    if (!defaultUiTimings.contains(key))
        return;
    settings.setValue("ui/timing/" + key, boundedUiTiming(key, milliseconds));
    scheduleSettingsFlush();
    emit changed();
}

void Controller::resetUiTimings() {
    settings.remove("ui/timing");
    scheduleSettingsFlush();
    emit changed();
}
