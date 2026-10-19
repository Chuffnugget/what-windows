#include "app/Controller.h"
#include "Displays.h"
#include <QJsonDocument>
#include <QUuid>

bool Controller::displaysEnabled() const {
    return moduleEnabled("windows") && settings.value("modules/windows/displays", false).toBool();
}

QJsonObject Controller::displaysState(bool forSharing) const {
    // Keep local recovery controls usable while withholding disabled metadata from HA.
    if (forSharing && !displaysEnabled())
        return {{"enabled", false},          {"control_enabled", false},
                {"available", false},        {"busy", false},
                {"pending", false},          {"status", "Disabled"},
                {"monitors", QJsonArray{}},  {"layouts", QJsonArray{}},
                {"active_layout", QString()}};
    const QVariantList flags{forSharing,
                             displaysEnabled(),
                             settings.value("controls/displays", false),
                             displayJob.isRunning() && !displayQuerying,
                             displayCommandQueued,
                             !displayRollback.isEmpty(),
                             displayMessage};
    if (displayCacheInput[forSharing ? 1 : 0] == displayInfo &&
        displayCacheLayouts[forSharing ? 1 : 0] == displayLayouts &&
        displayCacheFlags[forSharing ? 1 : 0] == flags)
        return displayCacheResult[forSharing ? 1 : 0];
    auto state = displayInfo;
    state["enabled"] = displaysEnabled();
    state["control_enabled"] = displaysEnabled() && settings.value("controls/displays", false).toBool();
    state["available"] = displaysEnabled() && displayInfo["available"].toBool();
    state["busy"] = (displayJob.isRunning() && !displayQuerying) || displayCommandQueued;
    state["pending"] = !displayRollback.isEmpty();
    state["status"] = displayMessage.isEmpty() ? displayInfo["status"].toString() : displayMessage;
    state["monitors"] = displaysEnabled() ? displayInfo["monitors"].toArray() : QJsonArray{};
    QJsonArray layouts;
    for (auto v : displayLayouts) {
        auto l = v.toObject();
        bool available = true;
        for (auto r : l["layout"].toArray()) {
            bool found = false;
            for (auto m : displayInfo["monitors"].toArray())
                if (m.toObject()["id"] == r.toObject()["id"])
                    found = true;
            available &= found;
        }
        layouts.append(QJsonObject{{"id", l["id"]}, {"name", l["name"]}, {"available", available}});
    }
    state["layout_editor"] = true;
    state["layouts"] = layouts;
    state["active_layout"] = QString();
    if (state["available"].toBool())
        for (auto v : displayLayouts)
            if (Displays::matches(v.toObject()["layout"].toArray(), Displays::layout(displayInfo))) {
                state["active_layout"] = v.toObject()["id"];
                break;
            }
    displayCacheInput[forSharing ? 1 : 0] = displayInfo;
    displayCacheLayouts[forSharing ? 1 : 0] = displayLayouts;
    displayCacheFlags[forSharing ? 1 : 0] = flags;
    displayCacheResult[forSharing ? 1 : 0] = state;
    return state;
}

void Controller::setupDisplays() {
    QSet<QString> names, ids;
    const auto stored = QJsonDocument::fromJson(settings.value("displays/layouts").toByteArray()).array();
    for (auto v : stored) {
        const auto l = v.toObject();
        const auto id = l["id"].toString(), name = l["name"].toString();
        if (displayLayouts.size() >= 32)
            break;
        if (QUuid(id).isNull() || name.trimmed().isEmpty() || name.size() > 64 ||
            names.contains(name.toCaseFolded()) || ids.contains(id) ||
            !Displays::validateLayout(l["layout"].toArray()).isEmpty())
            continue;
        displayLayouts.append(l);
        names.insert(name.toCaseFolded());
        ids.insert(id);
    }
    displayRollback = QJsonDocument::fromJson(settings.value("displays/recovery").toByteArray()).array();
    if (!displayRollback.isEmpty() && !Displays::validateLayout(displayRollback).isEmpty())
        displayRollback = {};
    displayJob.onStatus = [this](QString error) {
        workerStatus("Displays", error);
        if (!error.isEmpty()) {
            displayQuerying = false;
            displayMessage = error;
            emit changed();
            drainDisplayCommand();
        }
    };
    if (!displayRollback.isEmpty()) {
        displayMessage =
            "An interrupted display change has recovery data. Restore the previous layout or keep the current layout.";
    }
}

void Controller::setDisplaysEnabled(bool enabled) {
    ++*commandEpoch;
    ++collectionGeneration;
    settings.setValue("modules/windows/displays", enabled);
    scheduleSettingsFlush();
    displayClock.invalidate();
    if (!enabled)
        displayInfo["available"] = false;
    pollDisplays();
    publishPolicy();
    emit changed();
    emit sensorsChanged();
    log(QString("Displays submodule %1").arg(enabled ? "enabled" : "disabled"));
}

void Controller::setDisplayControl(bool enabled) {
    ++*commandEpoch;
    settings.setValue("controls/displays", enabled);
    scheduleSettingsFlush();
    publishPolicy();
    emit changed();
    log(enabled ? "Home Assistant display control enabled" : "Home Assistant display control disabled");
}

void Controller::pollDisplays() {
    if (!displaysEnabled() || displayJob.isRunning() ||
        (displayClock.isValid() && displayClock.elapsed() < 10000))
        return;
    displayClock.restart();
    displayQuerying = true;
    const auto generation = collectionGeneration;
    const bool testing = testMode;
    displayJob.submit(
        [testing] {
            return testing
                       ? QJsonObject{{"available", true}, {"status", "Test mode"}, {"monitors", QJsonArray{}}}
                       : Displays::query();
        },
        [this, generation](QJsonObject state) {
            displayQuerying = false;
            // Dispatch only after the query's result has been incorporated.
            QTimer::singleShot(0, this, &Controller::drainDisplayCommand);
            if (generation != collectionGeneration || !displaysEnabled())
                return;
            if (displayInfo["status"] != state["status"])
                log("Displays: " + state["status"].toString());
            displayInfo = state;
            mergeReadings("windows", Displays::readings(state), 6);
            publish();
            emit changed();
            emit sensorsChanged();
        });
}

void Controller::localDisplayCommand(const QVariantMap &arguments) {
    runDisplayCommand({}, QJsonObject::fromVariantMap(arguments));
}

void Controller::handleDisplayCommand(const QString &session, const QJsonObject &command) {
    runDisplayCommand(session, command["args"].toObject(), command["id"].toString());
}

void Controller::drainDisplayCommand() {
    if (!displayCommandQueued || displayJob.isRunning())
        return;
    displayCommandQueued = false;
    const auto session = queuedDisplaySession, id = queuedDisplayId;
    const auto args = queuedDisplayArgs;
    queuedDisplayArgs = {};
    if (commandEpoch->load() != queuedDisplayEpoch ||
        (!session.isEmpty() && (!server.commandLease(session) || !server.commandLease(session)->load()))) {
        if (!session.isEmpty())
            server.replyCommand(session, id, false,
                                "Display command cancelled: permissions or connection changed");
        emit changed();
        publish();
        return;
    }
    runDisplayCommand(session, args, id);
}

void Controller::runDisplayCommand(const QString &session, const QJsonObject &args,
                                   const QString &requestId) {
    const bool local = session.isEmpty();
    const auto operation = args["operation"].toString();
    auto reply = [this, session, requestId, local](bool ok, QString message) {
        displayMessage = message;
        log("Displays: " + message);
        if (!local)
            server.replyCommand(session, requestId, ok, message);
        emit changed();
        publish();
    };
    const bool recovery = operation == "revert" || operation == "keep";
    if (!displaysEnabled() && !(local && recovery)) {
        reply(false, "Enable the Displays submodule first");
        return;
    }
    if (!local && !settings.value("controls/displays", false).toBool()) {
        reply(false, "Enable Home Assistant display control on the PC first");
        return;
    }
    if (displayQuerying && !displayCommandQueued) {
        // Bounded one-command queue; never run OS queries and mutations concurrently.
        if (QJsonDocument(args).toJson(QJsonDocument::Compact).size() > 32768) {
            reply(false, "Display command too large");
            return;
        }
        queuedDisplayArgs = args;
        queuedDisplaySession = session;
        queuedDisplayId = requestId;
        queuedDisplayEpoch = commandEpoch->load();
        displayCommandQueued = true;
        emit changed();
        publish();
        return;
    }
    if (displayJob.isRunning() || displayCommandQueued) {
        reply(false, "A display operation is still running; try again shortly");
        return;
    }
    if (operation == "keep") {
        if (displayRollback.isEmpty()) {
            reply(false, "No display change awaits confirmation");
            return;
        }
        displayRollback = {};
        settings.remove("displays/recovery");
        settings.sync();
        reply(true, "Display layout kept for this session. Save it by name to restore it later.");
        return;
    }
    if (args.size() > 4) {
        reply(false, "Too many display command arguments");
        return;
    }
    if (operation == "apply") {
        if (args.size() != 3 || !args["layout"].isArray() || !args["expected"].isArray() ||
            !Displays::validateLayout(args["layout"].toArray()).isEmpty() ||
            !Displays::validateLayout(args["expected"].toArray()).isEmpty()) {
            reply(false, "Invalid display layout or expected arrangement");
            return;
        }
    } else {
        for (auto it = args.begin(); it != args.end(); ++it)
            if (!QStringList{"operation", "monitor", "source", "layout_id", "name"}.contains(it.key()) ||
                !it.value().isString() || it.value().toString().size() > 128) {
                reply(false, "Invalid display command arguments");
                return;
            }
    }
    if (operation == "save" || operation == "delete") {
        if (!local) {
            reply(false, "Create and delete saved layouts in the Windows app");
            return;
        }
        if (!displayRollback.isEmpty()) {
            reply(false, "Keep or revert the pending change before managing layouts");
            return;
        }
        if (operation == "delete") {
            QJsonArray next;
            for (auto v : displayLayouts)
                if (v.toObject()["id"] != args["layout_id"])
                    next.append(v);
            displayLayouts = next;
        } else {
            const auto name = args["name"].toString().trimmed();
            if (name.isEmpty() || name.size() > 64 || name.contains(QChar('\n')) ||
                displayLayouts.size() >= 32) {
                reply(false, "Use a name of 1–64 characters; at most 32 layouts can be saved");
                return;
            }
            for (auto v : displayLayouts)
                if (v.toObject()["name"].toString().compare(name, Qt::CaseInsensitive) == 0) {
                    reply(false, "A layout already has this name; delete it or use a different name");
                    return;
                }
            // Save a fresh read, so changes made in Windows Settings are included.
            const bool testing = testMode;
            displayJob.submit(
                [testing] { return testing ? QJsonObject{} : Displays::query(); },
                [this, name, reply](QJsonObject state) {
                    const auto layout = Displays::layout(state);
                    const auto error = Displays::validateLayout(layout);
                    if (!state["available"].toBool() || !error.isEmpty()) {
                        reply(false,
                              "Could not capture current layout: " + state["status"].toString() + error);
                        return;
                    }
                    displayInfo = state;
                    displayLayouts.append(
                        QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                    {"name", name},
                                    {"layout", layout}});
                    settings.setValue("displays/layouts",
                                      QJsonDocument(displayLayouts).toJson(QJsonDocument::Compact));
                    scheduleSettingsFlush();
                    reply(true, "Saved display layout: " + name);
                });
            emit changed();
            return;
        }
        settings.setValue("displays/layouts", QJsonDocument(displayLayouts).toJson(QJsonDocument::Compact));
        scheduleSettingsFlush();
        reply(true, "Saved layout removed");
        return;
    }
    if (!recovery && !displayRollback.isEmpty()) {
        reply(false, "Keep or revert the pending display change first");
        return;
    }
    QJsonArray saved;
    if (operation == "restore") {
        for (auto v : displayLayouts)
            if (v.toObject()["id"] == args["layout_id"])
                saved = v.toObject()["layout"].toArray();
        if (saved.isEmpty()) {
            reply(false, "Saved layout no longer exists");
            return;
        }
    } else if (operation == "revert") {
        saved = displayRollback;
        if (saved.isEmpty()) {
            reply(false, "No previous layout to restore");
            return;
        }
    } else if (!QStringList{"extend", "disconnect", "mirror", "primary", "apply"}.contains(operation)) {
        reply(false, "Unknown display operation");
        return;
    }
    const auto lease = local ? std::shared_ptr<std::atomic_bool>{} : server.commandLease(session);
    auto epoch = commandEpoch;
    const auto generation = epoch->load();
    const bool testing = testMode;
    const auto rollback = displayRollback;
    displayMessage = "Reading and validating display arrangement…";
    emit changed();
    // Read/plan first; persist recovery on the GUI thread before any OS mutation.
    displayJob.submit(
        [local, lease, operation, epoch, generation, testing, saved, args] {
            if ((!local && (!lease || !lease->load())) ||
                (operation != "revert" && epoch->load() != generation))
                return QJsonObject{{"error", "Display command cancelled"}};
            if (testing)
                return QJsonObject{{"error", "Display changes are disabled in test mode"}};
            auto before = Displays::query();
            if (!before["available"].toBool())
                return QJsonObject{{"error", before["status"]}};
            auto plan = saved.isEmpty() ? Displays::edit(before, args) : QJsonObject{{"layout", saved}};
            plan["before"] = Displays::layout(before);
            return plan;
        },
        [=](QJsonObject plan) {
            if (plan.contains("error")) {
                reply(false, plan["error"].toString());
                return;
            }
            const auto before = plan["before"].toArray(), wanted = plan["layout"].toArray();
            if (operation != "revert") {
                displayRollback = before;
                settings.setValue("displays/recovery", QJsonDocument(before).toJson(QJsonDocument::Compact));
                settings.sync();
                if (settings.status() != QSettings::NoError) {
                    displayRollback = {};
                    reply(false, "Cannot save display recovery data; no display change was made");
                    return;
                }
            }
            displayJob.submit(
                [local, lease, operation, epoch, generation, wanted] {
                    if ((!local && (!lease || !lease->load())) ||
                        (operation != "revert" && epoch->load() != generation))
                        return QJsonObject{{"error", "Display command cancelled"}};
                    const auto error = Displays::apply(wanted);
                    auto result = Displays::query();
                    result["error"] = error;
                    return result;
                },
                [=](QJsonObject result) {
                    if (result.contains("monitors"))
                        displayInfo = result;
                    auto error = result["error"].toString();
                    if (operation == "revert") {
                        if (error.isEmpty() && !Displays::matches(wanted, Displays::layout(result)))
                            error = "Windows restored a different arrangement; check Display settings";
                        if (error.isEmpty()) {
                            displayRollback = {};
                            settings.remove("displays/recovery");
                            settings.sync();
                        }
                        reply(
                            error.isEmpty(),
                            error.isEmpty()
                                ? "Previous display layout restored"
                                : error +
                                      ". Recovery data retained; reconnect missing displays and try Restore previous layout again.");
                    } else {
                        const bool exact = Displays::matches(wanted, Displays::layout(result));
                        if (error.isEmpty()) {
                            displayRollback = {};
                            settings.remove("displays/recovery");
                            settings.sync();
                            reply(
                                true,
                                exact
                                    ? "Display layout applied"
                                    : "Display layout applied; Windows adjusted the requested modes. Check Windows Display settings.");
                        } else
                            reply(
                                false,
                                error +
                                    ". Previous layout recovery is available; no automatic rollback will occur.");
                    }
                    mergeReadings("windows", Displays::readings(displayInfo), 6);
                    emit sensorsChanged();
                    emit changed();
                    publish();
                });
            emit changed();
        });
}
