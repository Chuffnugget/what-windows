#include "CustomModule.h"
#include "CustomSources.h"
#include <QDateTime>
#include <QSet>
#include <QThread>
#include <QUuid>

CustomModule::CustomModule(bool testing, QObject *parent) : QObject(parent), testMode(testing) {
    pollClock.start();
    timer.setInterval(500);
    connect(&timer, &QTimer::timeout, this, &CustomModule::schedule);
}

CustomModule::~CustomModule() {
    cancelTest();
    for (auto &state : states)
        state.lease->store(false);
}

QString CustomModule::newId() const {
    return QUuid::createUuid().toString(QUuid::Id128);
}

QVariantMap CustomModule::newDefinition(const QString &mode) const {
    const bool action = mode == "action";
    return QJsonObject{{"id", newId()},
                       {"name", ""},
                       {"description", ""},
                       {"mode", mode},
                       {"source", action ? "script" : "process"},
                       {"enabled", true},
                       {"ha_enabled", false},
                       {"cloud_enabled", false},
                       {"interval", 30},
                       {"timeout", 10},
                       {"shell", "powershell"},
                       {"command", ""},
                       {"format", "text"},
                       {"outputs", QJsonArray{QJsonObject{{"id", newId()},
                                                          {"name", "Reading"},
                                                          {"path", ""},
                                                          {"kind", action ? "text" : "binary"},
                                                          {"unit", ""},
                                                          {"state_class", ""}}}}}
        .toVariantMap();
}

QVariantMap CustomModule::definitionForReading(const QString &id) const {
    for (auto value : sources) {
        const auto definition = value.toObject();
        if ("custom_action_" + definition["id"].toString() == id)
            return definition.toVariantMap();
        for (auto output : definition["outputs"].toArray())
            if ("custom_" + output.toObject()["id"].toString() == id)
                return definition.toVariantMap();
    }
    return {};
}

void CustomModule::restore(QJsonArray definitions, bool enabled, bool control, bool allowCloud) {
    active = enabled;
    controls = control;
    cloud = allowCloud;
    if (active && !testMode)
        timer.start();
    QSet<QString> ids, outputIds;
    for (const auto value : definitions) {
        const auto definition = value.toObject();
        auto error = CustomSources::validate(definition);
        const auto id = definition["id"].toString();
        if (ids.contains(id))
            error = "Duplicate custom source ID";
        for (auto output : definition["outputs"].toArray())
            if (outputIds.contains(output.toObject()["id"].toString()))
                error = "Duplicate custom output ID";
        if (!error.isEmpty() || sources.size() >= 128) {
            emit diagnostic("Custom source was not loaded: " + error);
            continue;
        }
        ids.insert(id);
        for (auto output : definition["outputs"].toArray())
            outputIds.insert(output.toObject()["id"].toString());
        sources.append(definition);
        states.insert(id, State{});
    }
    emit changed();
    emit commandsChanged();
    emit readingsChanged();
}

QVariantList CustomModule::commands() const {
    return entriesForMode("action");
}

QVariantList CustomModule::sensors() const {
    return entriesForMode("sensor");
}

QStringList CustomModule::issues() const {
    QStringList result;
    if (!active)
        return result;
    for (const auto &value : sources) {
        const auto definition = value.toObject();
        const auto state = states.constFind(definition["id"].toString());
        if (!definition["enabled"].toBool() || state == states.cend())
            continue;
        const auto error = state->result["error"].toString();
        if (!error.isEmpty())
            result.append(definition["name"].toString() + ": " + error);
    }
    return result;
}

QVariantList CustomModule::entriesForMode(const QString &mode) const {
    QVariantList entries;
    for (auto value : sources) {
        auto definition = value.toObject();
        if (definition["mode"] != mode)
            continue;
        const auto state = states.value(definition["id"].toString());
        definition["status"] = state.result["error"].toString();
        definition["last_attempt"] = state.result["attempt"];
        definition["last_success"] = state.lastSuccess;
        definition["duration_ms"] = state.result["duration_ms"];
        definition["running"] = state.running;
        if (definition["mode"] == "action")
            definition["result"] = state.result;
        entries.append(definition.toVariantMap());
    }
    return entries;
}

QJsonArray CustomModule::readings() const {
    return captureReadings()();
}

std::function<QJsonArray()> CustomModule::captureReadings() const {
    // Qt containers share their storage until modified. Conversion and mapping
    // happen on the catalogue worker, including when many outputs are configured.
    return [definitions = sources, results = states, enabled = active] {
        QJsonArray rows;
        for (auto value : definitions) {
            auto definition = value.toObject();
            definition["enabled"] = enabled && definition["enabled"].toBool();
            for (auto row :
                 CustomSources::readings(definition, results.value(definition["id"].toString()).result))
                rows.append(row);
        }
        return rows;
    };
}

QJsonObject CustomModule::shared(bool forHomeAssistant) const {
    QJsonArray buttons, cloudActions;
    for (auto value : sources) {
        const auto definition = value.toObject();
        if (active && definition["mode"] == "action" &&
            (!forHomeAssistant || definition["ha_enabled"].toBool())) {
            const auto id = "custom_action_" + definition["id"].toString();
            if (definition["ha_enabled"].toBool() && definition["cloud_enabled"].toBool())
                cloudActions.append(id);
            buttons.append(QJsonObject{{"id", "custom_action_" + definition["id"].toString()},
                                       {"name", definition["name"]},
                                       {"local_available", definition["enabled"].toBool()},
                                       {"available", controls && definition["ha_enabled"].toBool() &&
                                                         definition["enabled"].toBool()}});
        }
    }
    return {{"enabled", active},
            {"control_enabled", active && controls},
            {"cloud", cloud},
            {"actions", buttons},
            {"_cloud_actions", cloudActions}};
}

void CustomModule::invalidate(const QString &id) {
    states[id].lease->store(false);
    states[id] = State{};
}

void CustomModule::notifyConfiguration() {
    emit configurationChanged();
    emit commandsChanged();
    emit changed();
    emit readingsChanged();
}

void CustomModule::setEnabled(bool enabled) {
    if (active == enabled)
        return;
    active = enabled;
    if (active && !testMode)
        timer.start();
    else
        timer.stop();
    for (const auto &id : states.keys())
        invalidate(id);
    notifyConfiguration();
}

void CustomModule::setControlEnabled(bool enabled) {
    if (controls == enabled)
        return;
    controls = enabled;
    // Revoke already accepted actions when permission changes.
    for (auto value : sources)
        if (value.toObject()["mode"] == "action")
            invalidate(value.toObject()["id"].toString());
    notifyConfiguration();
}

void CustomModule::setCloudEnabled(bool enabled) {
    if (cloud == enabled)
        return;
    cloud = enabled;
    for (auto value : sources)
        if (value.toObject()["mode"] == "action")
            invalidate(value.toObject()["id"].toString());
    notifyConfiguration();
}

QVariantMap CustomModule::save(const QVariantMap &input) {
    auto definition = QJsonObject::fromVariantMap(input);
    // Do not persist diagnostics or values copied from an expanded editor.
    for (const auto &key : {"status", "last_attempt", "last_success", "duration_ms", "running", "result"})
        definition.remove(key);
    const auto invalid = CustomSources::validate(definition);
    if (!invalid.isEmpty())
        return {{"error", invalid}};
    const auto id = definition["id"].toString();
    int position = -1;
    QSet<QString> used;
    for (int i = 0; i < sources.size(); ++i) {
        const auto previous = sources[i].toObject();
        if (previous["id"] == id) {
            position = i;
            if (previous["mode"] != definition["mode"])
                return {{"error", "Create a new entry to change between a sensor and an action"}};
            for (auto oldValue : previous["outputs"].toArray())
                for (auto newValue : definition["outputs"].toArray()) {
                    const auto old = oldValue.toObject(), next = newValue.toObject();
                    if (old["id"] == next["id"] &&
                        (old["kind"] != next["kind"] || old["unit"] != next["unit"] ||
                         old["state_class"] != next["state_class"]))
                        return {
                            {"error",
                             "Add a new output to change its type, unit or statistics. Existing Home Assistant history keeps its original meaning."}};
                }
        } else
            for (auto output : previous["outputs"].toArray())
                used.insert(output.toObject()["id"].toString());
    }
    for (auto output : definition["outputs"].toArray())
        if (used.contains(output.toObject()["id"].toString()))
            return {{"error", "An output ID belongs to another source"}};
    if (position < 0 && sources.size() >= 128)
        return {{"error", "Maximum 128 custom sources"}};
    if (position < 0)
        sources.append(definition);
    else
        sources[position] = definition;
    invalidate(id);
    notifyConfiguration();
    return {{"id", id}};
}

void CustomModule::remove(const QString &id) {
    for (int i = 0; i < sources.size(); ++i)
        if (sources[i].toObject()["id"] == id) {
            states[id].lease->store(false);
            states.remove(id);
            sources.removeAt(i);
            notifyConfiguration();
            return;
        }
}

void CustomModule::test(const QVariantMap &input) {
    if (testing())
        return;
    if (!previewJob)
        previewJob = std::make_unique<BackgroundWorker>("Custom sensor preview");
    auto definition = QJsonObject::fromVariantMap(input);
    definition["enabled"] = true;
    const auto invalid = CustomSources::validate(definition);
    if (!invalid.isEmpty()) {
        lastPreview = {{"ok", false}, {"error", invalid}, {"definition_id", definition["id"]}};
        emit previewChanged();
        return;
    }
    const auto lease = previewLease = std::make_shared<std::atomic_bool>(true);
    lastPreview = {};
    previewJob->submit(
        [definition, lease] {
            return CustomSources::run(definition, [lease] {
                return !lease->load() || QThread::currentThread()->isInterruptionRequested();
            });
        },
        [this, lease, definition](QJsonObject result) {
            if (previewLease == lease) {
                result["readings"] = CustomSources::readings(definition, result);
                result["definition_id"] = definition["id"];
                lastPreview = result;
                emit previewChanged();
            }
        });
    emit previewChanged();
}

void CustomModule::cancelTest() {
    if (previewLease)
        previewLease->store(false);
}

void CustomModule::schedule() {
    if (!active || testMode)
        return;
    const auto now = pollClock.elapsed();
    const auto start = nextSource;
    for (qsizetype offset = 0; offset < sources.size(); ++offset) {
        const auto index = (start + offset) % sources.size();
        const auto definition = sources[index].toObject();
        const auto id = definition["id"].toString();
        auto &state = states[id];
        if (definition["mode"] != "sensor" || !definition["enabled"].toBool() || state.running ||
            state.next > now)
            continue;
        if (!readerA)
            readerA = std::make_unique<BackgroundWorker>("Custom sensors 1");
        auto *worker = readerA.get();
        if (worker->isRunning()) {
            if (!readerB)
                readerB = std::make_unique<BackgroundWorker>("Custom sensors 2");
            worker = readerB->isRunning() ? nullptr : readerB.get();
        }
        if (!worker)
            break;
        state.running = true;
        // Resume after the last admitted source when a worker becomes free.
        nextSource = (index + 1) % sources.size();
        const auto lease = state.lease;
        worker->submit(
            [definition, lease] {
                return CustomSources::run(definition, [lease] {
                    return !lease->load() || QThread::currentThread()->isInterruptionRequested();
                });
            },
            [this, id, lease, definition](QJsonObject result) {
                if (!states.contains(id) || states[id].lease != lease)
                    return;
                auto &state = states[id];
                state.running = false;
                const auto oldError = state.result["error"].toString();
                state.result = result;
                state.failures = result["ok"].toBool() ? 0 : qMin(6, state.failures + 1);
                if (result["ok"].toBool())
                    state.lastSuccess = result["attempt"].toString();
                // Failed probes back off independently; other sensors keep their schedules.
                const auto delay = qMax(definition["interval"].toInt() * 1000,
                                        state.failures ? qMin(60000, 1000 << state.failures) : 0);
                state.next = pollClock.elapsed() + delay;
                if (oldError != result["error"].toString())
                    emit diagnostic(QString("Custom source %1: %2")
                                        .arg(id, result["ok"].toBool()
                                                     ? "recovered"
                                                     : "collection failed; inspect its status"));
                emit readingsChanged();
                emit changed();
            });
    }
}

void CustomModule::runAction(const QString &identifier, const QString &request,
                             std::shared_ptr<std::atomic_bool> remote, bool viaCloud) {
    const auto id = identifier.startsWith("custom_action_") ? identifier.mid(14) : identifier;
    for (auto value : sources) {
        const auto definition = value.toObject();
        if (definition["id"] != id || definition["mode"] != "action")
            continue;
        if (!active || !definition["enabled"].toBool() ||
            (remote && (!controls || !definition["ha_enabled"].toBool() ||
                        (viaCloud && (!cloud || !definition["cloud_enabled"].toBool())))) ||
            (actions && actions->isRunning())) {
            emit actionFinished(request, false,
                                "Custom action is disabled, not permitted or another action is running");
            return;
        }
        const auto lease = states[id].lease;
        if (!actions)
            actions = std::make_unique<BackgroundWorker>("Custom actions");
        states[id].running = true;
        actions->submit(
            [definition, lease, remote] {
                return CustomSources::run(definition, [lease, remote] {
                    return !lease->load() || (remote && !remote->load()) ||
                           QThread::currentThread()->isInterruptionRequested();
                });
            },
            [this, id, lease, request, remote](QJsonObject result) {
                if (states.contains(id) && states[id].lease == lease) {
                    states[id].running = false;
                    states[id].result = result;
                    if (result["ok"].toBool())
                        states[id].lastSuccess = result["attempt"].toString();
                }
                emit changed();
                emit commandsChanged();
                emit actionFinished(request, result["ok"].toBool(),
                                    result["ok"].toBool() ? "Action completed"
                                    : remote ? "Command failed; inspect its result on WHAT's Commands page"
                                             : result["error"].toString());
            });
        emit changed();
        emit commandsChanged();
        return;
    }
    emit actionFinished(request, false, "Custom action no longer exists");
}
