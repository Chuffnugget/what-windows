#include "app/Controller.h"
#include "app/Architecture.h"
#include "WallpaperEngine.h"
#include <QFileDialog>

QVariantMap Controller::wallpaperState() const {
    auto state = wallpaperData.toVariantMap();
    state["enabled"] = moduleEnabled("wallpaper");
    state["busy"] = wallpaperJob.isRunning();
    state["control_enabled"] = settings.value("modules/wallpaper/control", false).toBool();
    state["share_path"] = settings.value("modules/wallpaper/share_path", false).toBool();
    state["cloud"] = settings.value("modules/wallpaper/cloud", false).toBool();
    state["override"] = settings.value("modules/wallpaper/path", "");
    return state;
}

QJsonObject Controller::wallpaperShared() const {
    auto state = QJsonObject::fromVariantMap(wallpaperState());
    state.remove("executable");
    state.remove("override");
    if (!state["enabled"].toBool()) {
        state = {{"enabled", false}, {"running", false}, {"control_enabled", false}, {"cloud", false}};
    }
    if (!state.value("share_path").toBool())
        state.remove("current_wallpaper");
    return state;
}

void Controller::saveWallpaper(bool enabled, bool control, bool sharePath, bool cloud, const QString &path) {
    if (!Architecture::moduleAvailable("wallpaper"))
        return;
    ++*commandEpoch;
    settings.setValue("modules/wallpaper/enabled", enabled);
    settings.setValue("modules/wallpaper/control", control);
    settings.setValue("modules/wallpaper/share_path", sharePath);
    settings.setValue("modules/wallpaper/cloud", cloud);
    settings.setValue("modules/wallpaper/path", path.trimmed());
    scheduleSettingsFlush();
    wallpaperData = {};
    publishPolicy();
    refreshWallpaper();
    emit changed();
}

QString Controller::browseWallpaper(bool executable) {
    return QFileDialog::getOpenFileName(
        nullptr, executable ? "Select Wallpaper Engine executable" : "Select wallpaper", {},
        executable ? "Wallpaper Engine (wallpaper32.exe wallpaper64.exe)" : "Wallpaper files (*.*)");
}

void Controller::refreshWallpaper() {
    if (!moduleEnabled("wallpaper") || wallpaperJob.isRunning())
        return;
    const auto path = settings.value("modules/wallpaper/path").toString();
    const auto epoch = commandEpoch->load();
    const bool testing = testMode;
    wallpaperJob.submit(
        [path, testing] {
            return testing ? QJsonObject{{"installed", false}, {"running", false}, {"status", "Test mode"}}
                           : WallpaperEngine::inspect(path, true);
        },
        [this, epoch](QJsonObject state) {
            if (commandEpoch->load() == epoch && moduleEnabled("wallpaper")) {
                wallpaperData = state;
                publish();
                emit changed();
            }
        });
    emit changed();
}

void Controller::wallpaperCommand(const QVariantMap &args) {
    handleWallpaperCommand({}, {{"args", QJsonObject::fromVariantMap(args)}});
}

void Controller::handleWallpaperCommand(const QString &session, const QJsonObject &command) {
    const auto args = command["args"].toObject();
    const auto id = command["id"].toString();
    const bool local = session.isEmpty();
    auto reply = [this, session, id, local](QJsonObject result) {
        const auto error = result["error"].toString();
        wallpaperData["status"] = error.isEmpty() ? result["message"].toString() : error;
        if (result.contains("output") && !result["output"].toString().isEmpty())
            wallpaperData["current_wallpaper"] = result["output"];
        if (!local) {
            result.remove("output");
            server.replyCommand(session, id, error.isEmpty(),
                                error.isEmpty() ? result["message"].toString() : error);
        }
        emit changed();
        publish();
    };
    if (!moduleEnabled("wallpaper") ||
        (!local && !settings.value("modules/wallpaper/control", false).toBool())) {
        reply({{"error", "Enable Wallpaper Engine and its Home Assistant control permission first"}});
        return;
    }
    const auto invalid = WallpaperEngine::validate(args, false);
    if (!invalid.isEmpty()) {
        reply({{"error", invalid}});
        return;
    }
    if (wallpaperJob.isRunning()) {
        reply({{"error", "Wallpaper Engine is busy; try again"}});
        return;
    }
    const auto path = settings.value("modules/wallpaper/path").toString();
    auto lease = local ? std::shared_ptr<std::atomic_bool>{} : server.commandLease(session);
    auto epoch = commandEpoch;
    const auto generation = epoch->load();
    const bool testing = testMode;
    wallpaperJob.submit(
        [local, lease, epoch, generation, testing, path, args] {
            if ((!local && (!lease || !lease->load())) || epoch->load() != generation)
                return QJsonObject{{"error", "Command cancelled"}};
            if (testing)
                return QJsonObject{{"error", "Wallpaper commands are disabled in test mode"}};
            const auto exe = WallpaperEngine::discover(path);
            if ((!local && !lease->load()) || epoch->load() != generation)
                return QJsonObject{{"error", "Command cancelled"}};
            return WallpaperEngine::execute(exe, args);
        },
        reply);
    emit changed();
}
