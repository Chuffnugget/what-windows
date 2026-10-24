#include "Controller.h"
#include "AppRuntime.h"
#include <QCoreApplication>
#include <QApplication>
#include <QPalette>
#include <QClipboard>
#include <cmath>

void Controller::requestHaEntity(const QVariantMap &row) {
    const auto id = row.value("id").toString();
    QStringList keys;
    if (!id.isEmpty() && id.size() <= 256) {
        keys.append(id);
        if (row.value("control") == "beacn")
            keys.append("beacn_" + row.value("beacn").toMap().value("id").toString());
        if (id.startsWith("icue_auto_"))
            keys.append("icue_automation_" + id.mid(10));
        if (id.startsWith("icue_lighting_control_"))
            keys.append("icue_lighting_" + id.mid(22));
        if (row.contains("icueProperty")) {
            const auto prefix = id.left(id.lastIndexOf('_') + 1);
            const auto property = row.value("icueProperty").toInt();
            keys.append(prefix + (property == 5 ? "equalizer" : "control_" + QString::number(property)));
        }
        if (row.value("control") == "display")
            keys.append("windows_displays_arrangement");
        if (row.value("control") == "wallpaper") {
            for (const auto *suffix : {"current", "pause", "play", "stop", "mute", "nextWallpaper", "hideIcons", "showIcons"})
                keys.append("wallpaper_engine_" + QString::fromLatin1(suffix));
        }
    }
    haEntityLookup = {{"reading", id}, {"entities", QStringList{}},
        {"message", haFeedbackAvailable ? "Looking up the Home Assistant entity ID…"
            : "Connect Home Assistant with the updated WHAT integration to look up entity IDs."}};
    emit haEntityChanged();
    server.setEntityLookup(id, keys);
}

bool Controller::copyHaEntity(const QString &entity) {
    if (!haEntityLookup.value("entities").toStringList().contains(entity))
        return false;
    QApplication::clipboard()->setText(entity);
    return QApplication::clipboard()->ownsClipboard();
}

QString Controller::uiTheme() const {
    const auto name = settings.value("ui/theme", "blue").toString();
    return QStringList{"blue", "dark-grey", "slate", "forest", "plum"}.contains(name) ? name : "blue";
}

int Controller::sidebarWidth() const {
    return qBound(180, settings.value("ui/sidebar_width", 204).toInt(), 360);
}

bool Controller::sidebarCollapsed() const {
    return settings.value("ui/sidebar_collapsed", false).toBool();
}

void Controller::setSidebarLayout(int width, bool collapsed) {
    width = qBound(180, width, 360);
    if (width == sidebarWidth() && collapsed == sidebarCollapsed())
        return;
    settings.setValue("ui/sidebar_width", width);
    settings.setValue("ui/sidebar_collapsed", collapsed);
    scheduleSettingsFlush();
    emit sidebarChanged();
}

void Controller::setUiTheme(const QString &name) {
    if (!QStringList{"blue", "dark-grey", "slate", "forest", "plum"}.contains(name) || name == uiTheme())
        return;
    settings.setValue("ui/theme", name);
    scheduleSettingsFlush();
    emit uiThemeChanged();
}

namespace {
QStringList normaliseModuleOrder(const QStringList &saved) {
    const QStringList defaults{"beacn", "wallpaper", "icue", "nvidia", "coretemp",
                               "hwinfo", "custom", "windows", "haweb"};
    QStringList order;
    for (const auto &key : saved)
        if (defaults.contains(key) && !order.contains(key))
            order.append(key);
    // New modules appear at the end without disturbing the user's existing order.
    for (const auto &key : defaults)
        if (!order.contains(key))
            order.append(key);
    return order;
}
}

QStringList Controller::moduleOrder() const {
    return normaliseModuleOrder(settings.value("ui/module_order").toStringList());
}

void Controller::setModuleOrder(const QStringList &requested) {
    const auto order = normaliseModuleOrder(requested);
    if (order == moduleOrder())
        return;
    settings.setValue("ui/module_order", order);
    scheduleSettingsFlush();
    emit moduleOrderChanged();
}

void Controller::applyWidgetTheme(const QVariantMap &colours) {
    // Theme.qml owns the colours. Apply them only when a theme changes, without
    // recreating menus or storing a second palette in the profile.
    const QPair<QPalette::ColorRole, const char *> roles[] = {
        {QPalette::Window, "canvas"}, {QPalette::Base, "field"},
        {QPalette::Button, "button"}, {QPalette::AlternateBase, "hover"},
        {QPalette::Shadow, "pressed"}, {QPalette::Mid, "outline"},
        {QPalette::Dark, "separator"}, {QPalette::Midlight, "muted"},
        {QPalette::WindowText, "ink"}, {QPalette::Text, "ink"},
        {QPalette::ButtonText, "ink"}, {QPalette::ToolTipBase, "surface"},
        {QPalette::ToolTipText, "ink"}, {QPalette::PlaceholderText, "muted"},
        {QPalette::Highlight, "accent"}, {QPalette::HighlightedText, "accentText"},
        {QPalette::Link, "accent"}, {QPalette::LinkVisited, "accentHover"}
    };
    auto palette = QApplication::palette();
    for (const auto &[role, key] : roles) {
        const auto colour = colours.value(key).value<QColor>();
        if (!colour.isValid())
            return;
        palette.setColor(role, colour);
    }
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, palette.color(QPalette::Midlight));
    if (palette == QApplication::palette())
        return;
    QApplication::setPalette(palette);
    // Qt caches palette(...) values in widget stylesheets. Refresh existing
    // tray menus and notices as well as widgets opened after this change.
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (!widget->styleSheet().isEmpty())
            widget->setStyleSheet(widget->styleSheet());
    }
}

void Controller::startAppMonitoring() {
    appUpdate = {{"installed", WHAT_VERSION},
                 {"latest", QJsonValue::Null},
                 {"release_url", QJsonValue::Null},
                 {"checked_at", QJsonValue::Null},
                 {"channel", settings.value("updates/beta", false).toBool() ? "beta" : "stable"},
                 {"busy", false}};
    auto usage = std::make_shared<AppUsage>();
    auto sample = [this, usage] {
        if (appJob.isRunning() || appRestartPending)
            return;
        appJob.submit([usage] { return usage->sample(); },
                      [this](QJsonObject metrics) {
                          metrics["uptime_seconds"] = double(appClock.elapsed() / 1000);
                          appMetrics = metrics;
                          publish();
                      });
    };
    connect(&appStatusTimer, &QTimer::timeout, this, sample);
    appStatusTimer.start(5000);
    QTimer::singleShot(0, this, sample);
}

QJsonObject Controller::appState() const {
    auto state = appMetrics;
    state["version"] = 1;
    state["control_enabled"] = settings.value("controls/what", false).toBool();
    state["logging"] = settings.value("diagnostics/logging", false).toBool();
    const int normal = qBound(100, settings.value("publishingNormalMs", 1000).toInt(), 60000);
    state["publishing_interval_ms"] = normal;
    state["high_interval_ms"] = qBound(50, settings.value("publishingHighMs", 100).toInt(), normal);
    state["restart_available"] =
        !appRestartPending && bool(restartLauncher) && (!restartBlocked || !restartBlocked());
    state["update"] = appUpdate;
    return state;
}

void Controller::setAppControlEnabled(bool enabled) {
    ++*commandEpoch;
    settings.setValue("controls/what", enabled);
    scheduleSettingsFlush();
    publishPolicy();
}

void Controller::setDiagnosticLogging(bool enabled) {
    settings.setValue("diagnostics/logging", enabled);
    server.setDiagnostics(enabled);
    scheduleSettingsFlush();
    publish();
}

void Controller::setAppUpdate(const QJsonObject &update) {
    if (appUpdate == update)
        return;
    appUpdate = update;
    publish();
}

void Controller::handleAppCommand(const QString &session, const QJsonObject &command) {
    const auto id = command["id"].toString(), action = command["action"].toString();
    const auto args = command["args"].toObject();
    auto reject = [&](const QString &message) { server.replyCommand(session, id, false, message); };
    if (!settings.value("controls/what", false).toBool()) {
        reject("Enable Allow Home Assistant to Manage WHAT in the app's Pairing and Security settings");
        return;
    }
    auto lease = server.commandLease(session);
    if (!lease || !lease->load())
        return;
    if (appRestartPending) {
        reject("WHAT is already preparing to restart");
        return;
    }
    if (action == "app_logging") {
        if (args.size() != 1 || !args["value"].isBool()) {
            reject("Logging requires a boolean value");
            return;
        }
        setDiagnosticLogging(args["value"].toBool());
    } else if (action == "app_publish_interval") {
        const double value = args["value"].toDouble(-1);
        const int high = appState()["high_interval_ms"].toInt();
        if (args.size() != 1 || !args["value"].isDouble() || !std::isfinite(value) ||
            std::floor(value) != value || value < qMax(100, high) || value > 60000) {
            reject(
                "Publishing interval must be a whole number from the high-priority interval to 60000 ms (minimum 100 ms)");
            return;
        }
        setPublishingIntervals(int(value), high);
    } else if (action == "app_restart") {
        if (!args.isEmpty() || !appState()["restart_available"].toBool() || appJob.isRunning()) {
            reject("WHAT cannot restart while another app operation is running");
            return;
        }
        if (testMode) {
            server.replyCommand(session, id, true, "Restart accepted (test mode; no process was restarted)");
            return;
        }
        appRestartPending = true;
        const auto generation = commandEpoch->load();
        const auto launch = restartLauncher;
        // Launch off the GUI thread. The replacement waits for this process to exit,
        // so settings, the profile lock and worker shutdown finish before it starts.
        appJob.onStatus = [this, session, id](const QString &error) {
            if (!error.isEmpty()) {
                appRestartPending = false;
                server.replyCommand(session, id, false, error);
            }
        };
        appJob.submit(launch, [this, session, id, lease, generation](const QString &error) {
            appJob.onStatus = {};
            if (!error.isEmpty() || !lease->load() || commandEpoch->load() != generation) {
                appRestartPending = false;
                server.replyCommand(session, id, false, error.isEmpty() ? "Restart cancelled" : error);
                return;
            }
            server.replyCommand(session, id, true, "WHAT is restarting");
            // Allow the network worker to send the acknowledgement before shutdown.
            QTimer::singleShot(250, this, [this, lease, generation] {
                if (lease->load() && commandEpoch->load() == generation &&
                    (!restartBlocked || !restartBlocked()))
                    // An approved restart must not be vetoed by taskbar-icon windows.
                    QCoreApplication::exit(0);
                else
                    appRestartPending = false;
            });
        });
        return;
    } else {
        reject("Unknown WHAT app command");
        return;
    }
    server.replyCommand(session, id, true, "Setting applied");
}
