#include "app/Controller.h"
#include "modules/windows/commands/WindowsCommands.h"
#include "modules/windows/commands/ShellCommand.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <QRegularExpression>

QJsonObject Controller::controlPermissions() const {
    QJsonObject result;
    for (const auto &key :
         {"media", "power", "power_plan", "urls", "monitor", "shell", "notifications", "input", "announcements"})
        result[key] = moduleEnabled("windows") && settings.value(QString("controls/") + key, false).toBool();
    if (!sourceOptions()["media"].toBool())
        result["media"] = false;
    return result;
}

void Controller::setControlPermission(const QString &permission, bool enabled) {
    if (!controlPermissions().contains(permission))
        return;
    ++*commandEpoch;
    settings.setValue("controls/" + permission, enabled);
    if (announcements)
        announcements->setPermitted(controlPermissions()["announcements"].toBool());
    if (richNotifications)
        richNotifications->setPermitted(controlPermissions()["notifications"].toBool());
    if (permission == "media" && enabled) {
        settings.setValue("sources/media", true);
        ++collectionGeneration;
        refreshSlow = true;
        slowSchedule.invalidate();
    }
    scheduleSettingsFlush();
    log(QString("Remote %1 commands %2").arg(permission, enabled ? "enabled" : "disabled"));
    sampleWindows();
    publishPolicy();
    emit sensorsChanged();
}

QJsonObject Controller::controlState() const {
    auto permissions = controlPermissions();
    const bool shell = permissions.take("shell").toBool();
    const bool notifications = permissions.take("notifications").toBool();
    const bool input = permissions.take("input").toBool();
    permissions.remove("announcements");
    QJsonObject values;
    QSet<QString> mediaIds;
    if (permissions["media"].toBool())
        for (auto it = catalog.cbegin(); it != catalog.cend(); ++it)
            if (it.key().startsWith("win_media_") || it.key() == "win_audio_volume" ||
                it.key() == "win_audio_muted")
                mediaIds.insert(it.key());
    if (permissions["media"].toBool())
        for (const auto &entry : allReadings(&mediaIds)) {
            const auto row = entry.toObject();
            const auto id = row["id"].toString();
            if (row["available"].toBool() &&
                (id.startsWith("win_media_") || id == "win_audio_volume" || id == "win_audio_muted"))
                values[id] = row["value"];
        }
    return {{"version", 1},
            {"permissions", permissions},
            {"shell_enabled", shell},
            {"notifications_enabled", notifications},
            {"input_enabled", input},
            {"values", values},
            {"power", permissions["power_plan"].toBool() ? powerInfo : QJsonObject{}}};
}

void Controller::handleCommand(const QString &session, const QJsonObject &command) {
    if (command.contains("_policy_generation") &&
        quint64(command["_policy_generation"].toDouble()) != server.policyGeneration()) {
        server.replyCommand(session, command["id"].toString(), false,
                            "Command cancelled: sharing policy changed");
        return;
    }
    if (command["action"].toString().startsWith("app_")) {
        handleAppCommand(session, command);
        return;
    }
    if (command["action"] == "entity_cleanup_result") {
        handleEntityCleanupResult(session, command);
        return;
    }
    if (command["action"] == "dynamic_lighting") {
        handleDynamicLightingCommand(session, command);
        return;
    }
    if (command["action"] == "beacn") {
        handleBeacnCommand(session, command);
        return;
    }
    if (command["action"] == "wallpaper") {
        handleWallpaperCommand(session, command);
        return;
    }
    if (command["action"] == "notify") {
        handleNotificationCommand(session, command);
        return;
    }
    if (command["action"] == "display") {
        handleDisplayCommand(session, command);
        return;
    }
    if (command["action"] == "custom_action") {
        handleCustomAction(session, command);
        return;
    }
    if (QStringList{"speak", "play_audio", "audio_control"}.contains(command["action"].toString())) {
        handleAnnouncementCommand(session, command);
        return;
    }
    if (command["action"].toString().startsWith("icue_")) {
        handleIcueCommand(session, command);
        return;
    }
    const auto id = command["id"].toString(), action = command["action"].toString();
    const auto args = command["args"].toObject();
    appendCommandStream(QString("\n[%1] Home Assistant → %2 %3\n")
                            .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), id.left(40), action));
    if (action == "run_command")
        appendCommandStream(args["shell"].toString() + "> " + args["command"].toString().left(2048) + "\n");
    else if (action == "keyboard")
        appendCommandStream("Keyboard input (contents omitted)\n");
    else
        appendCommandStream(QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)).left(2048) +
                            "\n");
    auto reject = [&](const QString &error) {
        appendCommandStream("Rejected: " + error + "\n");
        log("Remote command rejected: " + action.left(40) + " - " + error);
        server.replyCommand(session, id, false, error);
    };
    const auto permission = WindowsCommands::permission(action);
    const auto invalid = WindowsCommands::validate(action, args);
    if (!invalid.isEmpty()) {
        reject(invalid);
        return;
    }
    if (!controlPermissions()[permission].toBool()) {
        reject("Enable this remote-control permission on the Windows Modules page first");
        return;
    }
    auto &job = action == "run_command" ? shellJob : commandJob;
    if (job.isRunning()) {
        reject(action == "run_command" ? "Another shell command is still running"
                                       : "Another Windows command is still running");
        return;
    }
    auto lease = server.commandLease(session);
    if (!lease || !lease->load())
        return;
    const bool disruptive = QStringList{"sleep", "hibernate", "shutdown", "reboot"}.contains(action);
    if (disruptive)
        server.replyCommand(
            session, id, true,
            "Accepted; Windows may disconnect before completion. Check local diagnostics if it does not proceed.");
    const bool testing = testMode;
    auto epoch = commandEpoch;
    const auto generation = epoch->load();
    log("Remote command accepted: " + action + (testing ? " (test mode; no OS action)" : ""));
    job.onStatus = [this, session, id, action, disruptive](QString error) {
        if (action == "run_command" && !error.isEmpty()) {
            commandStreamTimer.stop();
            flushCommandStream();
        }
        if (!error.isEmpty()) {
            log("Remote command failed: " + action + " - " + error);
            if (!disruptive)
                server.replyCommand(session, id, false, error);
        }
    };
    QElapsedTimer queued;
    queued.start();
    if (action == "run_command") {
        auto streamBuffer = commandStreamBuffer;
        commandStreamTimer.start();
        shellJob.submit(
            [lease, epoch, generation, queued, testing, args, streamBuffer] {
                auto cancelled = [lease, epoch, generation] { return !lease->load() || epoch->load() != generation; };
                if (cancelled() || queued.elapsed() > 5000)
                    return QJsonObject{{"error", "Command cancelled before launch"}};
                if (testing)
                    return QJsonObject{{"stdout", ""},       {"stderr", ""},       {"exit_code", 0},
                                       {"timed_out", false}, {"cancelled", false}, {"truncated", false}};
                return ShellCommand::run(args, cancelled, [streamBuffer](bool isError, const QString &text) {
                    if (text.isEmpty())
                        return;
                    QMutexLocker lock(&streamBuffer->mutex);
                    streamBuffer->pending =
                        (streamBuffer->pending + (isError ? "[stderr] " : "") + text).right(65536);
                });
            },
            [this, session, id](QJsonObject result) {
                commandStreamTimer.stop();
                flushCommandStream();
                const auto error = result.value("error").toString();
                appendCommandStream(error.isEmpty()
                                        ? QString("\n[exit %1; timeout=%2; cancelled=%3; truncated=%4]\n")
                                              .arg(result["exit_code"].toDouble())
                                              .arg(result["timed_out"].toBool())
                                              .arg(result["cancelled"].toBool())
                                              .arg(result["truncated"].toBool())
                                        : "Error: " + error + "\n");
                log(error.isEmpty()
                        ? QString(
                              "Shell command finished: exit %1; timeout=%2; cancelled=%3; output truncated=%4")
                              .arg(result["exit_code"].toDouble())
                              .arg(result["timed_out"].toBool())
                              .arg(result["cancelled"].toBool())
                              .arg(result["truncated"].toBool())
                        : "Shell command failed: " + error);
                server.replyCommand(session, id, error.isEmpty(), error.isEmpty() ? "Completed" : error,
                                    result);
            });
        return;
    }
    commandJob.submit(
        [lease, epoch, generation, queued, testing, disruptive, action, args] {
            if (!lease->load() || epoch->load() != generation || queued.elapsed() > 5000)
                return QString("Command cancelled: session, permission or deadline changed");
            if (testing)
                return QString();
            // Give the accepted reply a short opportunity to drain before suspend/shutdown.
            if (disruptive) {
                QThread::msleep(250);
                if (!lease->load() || epoch->load() != generation)
                    return QString("Command cancelled");
            }
            return WindowsCommands::execute(action, args);
        },
        [this, session, id, action, disruptive](QString error) {
            appendCommandStream(action + (error.isEmpty() ? " completed\n" : " failed: " + error + "\n"));
            log("Remote command " + action + (error.isEmpty() ? " completed" : " failed: " + error));
            if (!disruptive)
                server.replyCommand(session, id, error.isEmpty(), error.isEmpty() ? "Completed" : error);
            refreshSlow = true;
            slowSchedule.invalidate();
            powerClock.invalidate();
            sampleWindows();
        });
}

void Controller::appendCommandStream(const QString &text) {
    commandStreamText = (commandStreamText + text).right(65536);
    emit commandStreamChanged();
}

void Controller::flushCommandStream() {
    QString text;
    {
        QMutexLocker lock(&commandStreamBuffer->mutex);
        text.swap(commandStreamBuffer->pending);
    }
    if (!text.isEmpty())
        appendCommandStream(text);
}

void Controller::clearCommandStream() {
    {
        QMutexLocker lock(&commandStreamBuffer->mutex);
        commandStreamBuffer->pending.clear();
    }
    commandStreamText.clear();
    emit commandStreamChanged();
}

void Controller::previewEntityCleanup(const QString &installation) {
    if (entityCleanupState.value("busy").toBool())
        return;
    if (!installation.isEmpty())
        entityCleanupInstallation = installation;
    else if (installations().size() == 1)
        entityCleanupInstallation = installations().first().toMap().value("id").toString();
    entityCleanupToken.clear();
    entityCleanupSession.clear();
    entityCleanupPreviewClock.invalidate();
    sendEntityCleanup("preview");
}

void Controller::confirmEntityCleanup() {
    if (entityCleanupState.value("busy").toBool() || entityCleanupToken.isEmpty() ||
        !entityCleanupPreviewClock.isValid() || entityCleanupPreviewClock.elapsed() > 110000) {
        entityCleanupState = {{"message", "Request a fresh preview before removing entities."}};
        emit changed();
        return;
    }
    sendEntityCleanup("remove");
}

void Controller::sendEntityCleanup(const QString &operation) {
    const auto state = installationStates.value(entityCleanupInstallation);
    const auto scope = state.value("scope").toString();
    if (entityCleanupInstallation.isEmpty() || state.value("clients").toInt() == 0 ||
        (scope != "local" && scope != "hybrid")) {
        entityCleanupToken.clear();
        entityCleanupSession.clear();
        entityCleanupPreviewClock.invalidate();
        entityCleanupState = {{"message",
                              "Choose a connected installation using local sharing before cleaning up. "
                              "Cloud-only sharing does not include the complete sensor selection."}};
        emit changed();
        return;
    }
    const auto id = QUuid::createUuid().toString(QUuid::Id128);
    entityCleanupRequest = {{"id", id}, {"operation", operation}, {"token", entityCleanupToken}};
    entityCleanupState = {{"busy", true}, {"message", "Waiting for Home Assistant…"}};
    emit changed();
    publish();
    QTimer::singleShot(20000, this, [this, id] {
        if (entityCleanupRequest["id"] != id || !entityCleanupState.value("busy").toBool())
            return;
        entityCleanupRequest = {};
        entityCleanupToken.clear();
        entityCleanupState = {
            {"message",
             "No reply from Home Assistant. Use the updated integration with local sharing. If removal was requested, check Home Assistant before retrying."}};
        emit changed();
        publish();
    });
}

void Controller::handleEntityCleanupResult(const QString &session, const QJsonObject &command) {
    const auto args = command["args"].toObject();
    const auto status = args["status"].toString();
    const auto operation = entityCleanupRequest["operation"].toString();
    bool valid = !session.isEmpty() && server.installationForSession(session) == entityCleanupInstallation &&
                 entityCleanupState.value("busy").toBool() &&
                 args["request_id"].isString() && args["request_id"] == entityCleanupRequest["id"] &&
                 (status == "error" || (operation == "preview" && status == "preview") ||
                  (operation == "remove" && status == "removed")) &&
                 (operation != "remove" || entityCleanupSession == session);
    const auto count = args["count"].toInt(-1);
    valid = valid && count >= 0 && count <= 100000;
    QStringList entities;
    if (status == "preview") {
        valid = valid && QRegularExpression("^[a-f0-9]{32}$").match(args["token"].toString()).hasMatch() &&
                args["entities"].isArray() && args["entities"].toArray().size() <= 20;
        for (auto item : args["entities"].toArray()) {
            const auto name = item.toString();
            valid = valid && name.size() <= 255 &&
                    QRegularExpression("^[a-z_]+\\.[a-z0-9_]+$").match(name).hasMatch();
            entities << name;
        }
    }
    if (!valid) {
        server.replyCommand(session, command["id"].toString(), false, "Cleanup reply is no longer expected.");
        return;
    }
    entityCleanupRequest = {};
    entityCleanupState = {
        {"busy", false}, {"status", status}, {"count", count}, {"entities", entities.join('\n')}};
    if (status == "preview") {
        entityCleanupToken = args["token"].toString();
        entityCleanupSession = session;
        entityCleanupPreviewClock.start();
        entityCleanupState["message"] =
            QString("%1 stale or unshared entities can be removed. First 20:").arg(count);
    } else {
        entityCleanupToken.clear();
        entityCleanupState["message"] =
            status == "removed"
                ? QString("Removed %1 stale or unshared entities.").arg(count)
                : QString(
                      "Home Assistant could not complete cleanup. Request a fresh preview using local sharing; check Home Assistant logs if it persists.");
    }
    // The dialog renders plain text. Keep HA's reason instead of hiding it behind a generic error.
    if (status == "error" && !args["message"].toString().trimmed().isEmpty())
        entityCleanupState["message"] = args["message"].toString().left(512);
    server.replyCommand(session, command["id"].toString(), true, "Cleanup result received.");
    log(status == "removed" ? QString("Home Assistant entity cleanup removed %1 entries").arg(count)
        : status == "error" ? "Home Assistant entity cleanup failed"
                            : "Home Assistant entity cleanup preview received");
    emit changed();
    publish();
}
