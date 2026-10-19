#include "NetworkService.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QUuid>
#include <openssl/crypto.h>

QString NetworkService::installationForSession(const QString &session) const {
    QMutexLocker lock(&worker->control->mutex);
    const auto guard = worker->control->installationLeases.value(session);
    if (guard && guard->load())
        for (auto it = installationGuards.cbegin(); it != installationGuards.cend(); ++it)
            if (it.value() == guard)
                return it.key();
    return {};
}

// Installation workers share the network thread and immutable sensor snapshots.
// Each owns its sockets, delivery baselines, retry timers and command sessions.
void NetworkService::setInstallations(QJsonArray definitions) {
    QHash<QString, QJsonObject> next;
    for (const auto &value : definitions) {
        auto options = value.toObject();
        const auto id = options["id"].toString();
        // Editing a route must leave the current route alive during handover.
        // Identity and permission changes still revoke commands synchronously.
        for (const auto *key : {"name", "type", "url", "endpoint", "mqtt", "addresses"})
            options.remove(key);
        next.insert(id, options);
        if (!installationGuards.contains(id) || installationDefinitions.value(id) != options) {
            if (auto guard = installationGuards.value(id))
                *guard = false;
            installationGuards[id] = std::make_shared<std::atomic_bool>(true);
        }
    }
    for (const auto &id : installationGuards.keys())
        if (!next.contains(id)) {
            *installationGuards.take(id) = false;
        }
    // Revoke accepted but not yet dispatched commands immediately on the GUI side.
    {
        QMutexLocker lock(&worker->control->mutex);
        for (auto it = worker->control->installationLeases.cbegin();
             it != worker->control->installationLeases.cend(); ++it)
            if (!it.value()->load())
                if (auto lease = worker->control->leases.value(it.key()))
                    *lease = false;
    }
    installationDefinitions = next;
    QMetaObject::invokeMethod(worker, [target = worker, definitions, guards = installationGuards] {
        target->setInstallations(definitions, guards);
    });
}

void NetworkWorker::setInstallations(QJsonArray definitions,
                                     QHash<QString, std::shared_ptr<std::atomic_bool>> guards) {
    if (!managedInstallations) {
        setRemote({});
        setMqtt({});
        const auto old = clients;
        for (auto *peer : old)
            peer->abort();
        managedInstallations = true;
    }
    QSet<QString> wanted;
    for (const auto &value : definitions)
        wanted.insert(value.toObject()["id"].toString());
    for (const auto &id : installations.keys()) {
        if (!wanted.contains(id) || installations[id]->installationAlive != guards.value(id)) {
            auto *old = installations.take(id);
            old->stop();
            old->disconnect(this);
            old->deleteLater();
            installationStates.remove(id);
        }
    }
    for (const auto &value : definitions) {
        const auto options = value.toObject();
        const auto id = options["id"].toString();
        if (installations.contains(id)) {
            installations[id]->applyInstallationOptions(options);
            continue;
        }
        auto *child = new NetworkWorker(this);
        child->control = control;
        child->installationId = id;
        child->installationOptions = options;
        child->activeInstallationTransport = options["type"].toString();
        child->transportRevision = QUuid::createUuid().toString(QUuid::Id128);
        child->installationAlive = guards.value(id);
        child->initialize();
        installations.insert(id, child);
        child->appliedGeneration = appliedGeneration;
        child->configure(device, options["token"].toString(), server->sslConfiguration());
        child->setInterval(timer->interval());
        child->setSnapshot(latest);
        child->setEntityLookup(lookupReading, lookupKeys);
        connect(child, &NetworkWorker::diagnostic, this, [this, child](const QString &text) {
            emit diagnostic("Installation [" + child->installationOptions["name"].toString() + "]: " + text);
        });
        connect(child, &NetworkWorker::commandRequested, this, &NetworkWorker::commandRequested);
        connect(child, &NetworkWorker::installationTransportFailed, this,
                &NetworkWorker::installationTransportFailed);
        connect(child, &NetworkWorker::transmitted, this, &NetworkWorker::transmitted);
        connect(child, &NetworkWorker::haCommunication, this, [this, child](qint64 time) {
            child->updateInstallationState({{"lastCommunication", time}});
            emit haCommunication(time);
        });
        connect(child, &NetworkWorker::entityLookupResult, this,
                [this, child](QString reading, QStringList entities, QString message) {
                    emit installationEntityLookup(child->installationId, reading, entities, message);
                });
        connect(child, &NetworkWorker::clientCount, this, [child](int count, quint64 total) {
            child->updateInstallationState({{"clients", count}, {"connections", qulonglong(total)}});
        });
        connect(child, &NetworkWorker::feedbackSupport, this,
                [child](bool supported) { child->updateInstallationState({{"feedback", supported}}); });
        connect(child, &NetworkWorker::transportState, this, [child](QString scope, QString tls) {
            child->updateInstallationState({{"scope", scope}, {"tls", tls}});
        });
        connect(child, &NetworkWorker::remoteState, this,
                [child](QString text) { child->updateInstallationState({{"status", text}}); });
        connect(child, &NetworkWorker::mqttState, this, [child](QString text) {
            if (child->installationOptions["type"] == "mqtt")
                child->updateInstallationState({{"status", text}});
        });
        connect(child, &NetworkWorker::connectionProgress, this,
                [child](const QString &transport, const QString &phase, int retryMs) {
                    if ((transport == "mqtt") != (child->installationOptions["type"] == "mqtt"))
                        return;
                    const auto now = QDateTime::currentMSecsSinceEpoch();
                    QVariantMap state{{"phase", phase}, {"retryAt", retryMs > 0 ? now + retryMs : 0}};
                    if (phase == "connecting")
                        state["attemptAt"] = now;
                    child->updateInstallationState(state);
                });
        child->setDiagnostics(diagnosticsEnabled);
        child->updateInstallationState({{"clients", 0}, {"status", "Sharing stopped"}});
        if (!stopped)
            child->startInstallation();
    }
    aggregateInstallationState();
}

void NetworkWorker::startInstallation() {
    if (!installationAlive || !installationAlive->load() || !installationOptions["enabled"].toBool()) {
        const auto failure = installationOptions["configurationError"].toString();
        updateInstallationState({{"status", failure.isEmpty() ? "Disabled" : failure},
                                 {"phase", failure.isEmpty() ? "disabled" : "failed"}});
        return;
    }
    stopped = false;
    activeInstallationTransport = installationOptions["type"].toString();
    timer->start();
    updateInstallationState({{"activeType", activeInstallationTransport}, {"switching", false}});
    if (installationOptions["type"] == "mqtt") {
        auto options = installationOptions["mqtt"].toObject();
        options["enabled"] = true;
        options["preferred"] = true;
        setMqtt(options);
    } else {
        setRemote(installationOptions["endpoint"].toString());
        if (remoteUrl.isEmpty())
            updateInstallationState({{"status", "Waiting for Home Assistant"}, {"phase", "waiting"}});
    }
}

void NetworkWorker::updateInstallationState(QVariantMap changes) {
    auto *owner = qobject_cast<NetworkWorker *>(parent());
    if (!owner || owner->installations.value(installationId) != this)
        return;
    auto &state = owner->installationStates[installationId];
    const auto before = state;
    if (changes.contains("clients")) {
        const bool connected = changes.value("clients").toInt() > 0;
        if (connected != (state.value("clients").toInt() > 0))
            changes["connectedSince"] = connected ? QDateTime::currentMSecsSinceEpoch() : qint64(0);
    }
    for (auto it = changes.cbegin(); it != changes.cend(); ++it)
        state[it.key()] = it.value();
    if (state == before)
        return;
    emit owner->installationState(installationId, state);
    owner->aggregateInstallationState();
}

void NetworkWorker::aggregateInstallationState() {
    int count = 0;
    quint64 total = 0;
    bool local = false, cloud = false, feedback = false;
    QStringList protocols;
    for (const auto &state : installationStates) {
        count += state.value("clients").toInt();
        total += state.value("connections").toULongLong();
        feedback |= state.value("feedback").toBool();
        const auto scope = state.value("scope").toString();
        local |= scope == "local" || scope == "hybrid";
        cloud |= scope == "cloud" || scope == "hybrid";
        for (const auto &protocol : state.value("tls").toString().split(" + ", Qt::SkipEmptyParts))
            if (!protocols.contains(protocol))
                protocols.append(protocol);
    }
    const QString scope = local && cloud ? "hybrid" : cloud ? "cloud" : local ? "local" : "none";
    protocols.sort();
    const auto tls = protocols.join(" + ");
    if (aggregateState.value("clients") != count || aggregateState.value("total") != total)
        emit clientCount(count, total);
    if (aggregateState.value("feedback") != feedback)
        emit feedbackSupport(feedback);
    if (aggregateState.value("scope") != scope || aggregateState.value("tls") != tls)
        emit transportState(scope, tls);
    aggregateState = {{"clients", count},
                      {"total", qulonglong(total)},
                      {"feedback", feedback},
                      {"scope", scope},
                      {"tls", tls}};
    // Build a safe shared report on the network worker. Every installation sees
    // the same named states, but never another destination's address or secrets.
    installationDiagnostics = {};
    for (auto *child : installations) {
        const auto &options = child->installationOptions;
        const auto state = installationStates.value(child->installationId);
        const bool connected = !child->stopped && state.value("clients").toInt() > 0;
        QString status = "waiting";
        if (!options["configurationError"].toString().isEmpty())
            status = "error";
        else if (!options["enabled"].toBool())
            status = "disabled";
        else if (connected)
            status = "connected";
        else if (child->stopped)
            status = "stopped";
        else if (state.value("phase") == "connecting")
            status = "connecting";
        else if (state.value("phase") == "retrying")
            status = "retrying";
        else if (state.value("phase") == "failed")
            status = "error";
        const auto since = connected ? state.value("connectedSince").toLongLong() : 0;
        installationDiagnostics.append(
            QJsonObject{{"id", child->installationId},
                        {"name", options["name"]},
                        {"type", child->activeInstallationTransport},
                        {"status", status},
                        {"scope", connected ? state.value("scope", "none").toString() : "none"},
                        {"connected_since", since},
                        {"last_reply", state.value("lastCommunication").toLongLong()}});
    }
}

void NetworkWorker::acceptInstallation(QWebSocket *socket) {
    // Limit unauthenticated handshakes, not the number of configured installations.
    if (stopped) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    // Do not let idle, unauthenticated sockets reserve every pairing slot.
    // Authenticated connections are never evicted by this admission limit.
    if (pendingPairings.size() >= 16) {
        auto *oldest = pendingPairings.takeFirst();
        oldest->abort();
        oldest->deleteLater();
    }
    pendingPairings.append(socket);
    socket->setMaxAllowedIncomingFrameSize(32768);
    socket->setMaxAllowedIncomingMessageSize(32768);
    auto connection = std::make_shared<QMetaObject::Connection>();
    auto disconnected = std::make_shared<QMetaObject::Connection>();
    *disconnected = connect(socket, &QWebSocket::disconnected, this, [this, socket] {
        if (pendingPairings.removeOne(socket))
            socket->deleteLater();
    });
    QTimer::singleShot(5000, socket, [this, socket] {
        if (pendingPairings.contains(socket))
            socket->abort();
    });
    *connection =
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket, connection, disconnected](const QString &message) {
                    const auto provided =
                        QJsonDocument::fromJson(message.toUtf8()).object()["token"].toString().toLatin1();
                    for (auto *child : installations) {
                        const auto expected = child->token.toLatin1();
                        if (child->stopped || !child->installationAlive->load() || expected.isEmpty() ||
                            provided.size() != expected.size() ||
                            CRYPTO_memcmp(provided.data(), expected.data(), expected.size()) != 0)
                            continue;
                        // Each installation can have its own local connection and outbound route.
                        if (child->clients.size() >= 4)
                            break;
                        disconnect(*connection);
                        disconnect(*disconnected);
                        pendingPairings.removeOne(socket);
                        child->attachSocket(socket);
                        emit socket->textMessageReceived(message); // Run the normal complete auth validation.
                        return;
                    }
                    socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                                  "Installation disabled or pairing token rejected");
                });
}

QJsonObject NetworkWorker::installationSnapshot(QJsonObject value) const {
    // Do not turn an absent startup snapshot into a metadata-only frame.
    // Authentication may finish before the first sensor snapshot arrives.
    if (installationId.isEmpty() || value.isEmpty())
        return value;
    value["connection_options"] =
        QJsonObject{{"remote_enabled", !installationOptions["endpoint"].toString().isEmpty()},
                    {"mqtt_enabled", !installationOptions["mqtt"].toObject()["host"].toString().isEmpty()}};
    if (!installationOptions["controls"].toBool(true)) {
        for (const auto *key : {"app", "beacn", "wallpaper", "custom", "icue", "icue_automation", "displays",
                                "dynamic_lighting"}) {
            if (!value.contains(key))
                continue;
            auto module = value[key].toObject();
            module["control_enabled"] = false;
            value[key] = module;
        }
        auto controls = value["controls"].toObject();
        auto permissions = controls["permissions"].toObject();
        for (auto it = permissions.begin(); it != permissions.end(); ++it)
            it.value() = false;
        controls["permissions"] = permissions;
        controls["notifications_enabled"] = false;
        controls["announcements_enabled"] = false;
        controls["shell_enabled"] = false;
        controls["input_enabled"] = false;
        value["controls"] = controls;
    }
    const auto cleanupTarget = value.take("_cleanup_installation").toString();
    if (!cleanupTarget.isEmpty() && cleanupTarget != installationId)
        value.remove("entity_cleanup");
    return value;
}

// One route carries the live stream. A replacement receives one initial snapshot
// for validation, then waits for Home Assistant to finish pending commands and
// explicitly activate it. A failed check leaves the original route untouched.
bool NetworkWorker::isActiveInstallationPeer(WirePeer *peer) const {
    return installationId.isEmpty() || peer->mqtt == (activeInstallationTransport == "mqtt");
}

QJsonObject NetworkWorker::installationConnectionConfig(WirePeer *peer) const {
    const auto *owner = qobject_cast<NetworkWorker *>(parent());
    const auto addresses = installationOptions["addresses"].toArray();
    auto address = addresses.isEmpty() ? QString() : addresses.first().toString();
    if (peer->socket && !peer->cloud)
        address = peer->socket->localAddress().toString();
    return {{"revision", transportRevision},
            {"preferred", installationOptions["type"]},
            {"remote_enabled", !installationOptions["endpoint"].toString().isEmpty()},
            {"mqtt_enabled", !installationOptions["mqtt"].toObject()["host"].toString().isEmpty()},
            {"direct_host", address},
            {"direct_port", owner->server->serverPort()},
            {"fingerprint",
             QString::fromLatin1(
                 server->sslConfiguration().localCertificate().digest(QCryptographicHash::Sha256).toHex())}};
}

void NetworkWorker::applyInstallationOptions(QJsonObject options) {
    auto before = installationOptions;
    installationOptions = options;
    before.remove("name");
    options.remove("name");
    if (before == options || stopped)
        return;
    // An acknowledgement from an earlier edit must not activate obsolete settings.
    transportRevision = QUuid::createUuid().toString(QUuid::Id128);
    const auto requested = installationOptions["type"].toString();
    const bool switching = requested != activeInstallationTransport;
    if (!transportDeadline) {
        transportDeadline = new QTimer(this);
        transportDeadline->setSingleShot(true);
        transportDeadline->setInterval(30000);
        connect(transportDeadline, &QTimer::timeout, this, [this] {
            const auto requested = installationOptions["type"].toString();
            if (requested == activeInstallationTransport)
                return;
            auto restored = installationOptions;
            restored["type"] = activeInstallationTransport;
            applyInstallationOptions(restored);
            const auto message =
                QString(
                    "Could not verify %1. Kept %2. Check the connection settings and update the WHAT integration if needed.")
                    .arg(requested == "mqtt" ? "MQTT" : "Home Assistant API",
                         activeInstallationTransport == "mqtt" ? "MQTT" : "Home Assistant API");
            updateInstallationState({{"switching", false}, {"switchError", message}, {"status", message}});
            emit diagnostic(message);
            emit installationTransportFailed(installationId, requested, activeInstallationTransport, message);
        });
    }
    transportDeadline->stop();
    if (switching)
        transportDeadline->start();
    updateInstallationState(
        {{"switching", switching},
         {"switchError", ""},
         {"status", switching ? "Checking the new connection; current connection remains active"
                              : "Connection settings saved"}});
    // Send route permissions over the existing authenticated stream before the
    // replacement authenticates. Home Assistant applies them without reloading.
    publish();
    if (requested == "mqtt") {
        auto mqttConfig = installationOptions["mqtt"].toObject();
        mqttConfig["enabled"] = true;
        mqttConfig["preferred"] = true;
        setMqtt(mqttConfig);
        if (!switching)
            setRemote({});
    } else {
        mqtt->setPreferred(false); // An active MQTT peer stays alive until activation.
        if (remoteUrl != installationOptions["endpoint"].toString())
            setRemote(installationOptions["endpoint"].toString());
        else if (!remoteUrl.isEmpty() && !outbound)
            remoteRetry->start(1);
        if (!switching)
            setMqtt({});
    }
}

void NetworkWorker::activateInstallationTransport(WirePeer *peer, const QJsonObject &request) {
    // Authentication alone does not select a route. Home Assistant must validate
    // the replacement snapshot and acknowledge the current settings revision.
    const auto requested = installationOptions["type"].toString();
    const bool valid = transportPeers.contains(peer) && request["revision"] == transportRevision &&
                       peer->mqtt == (requested == "mqtt") &&
                       QRegularExpression("^[a-f0-9]{32}$").match(request["id"].toString()).hasMatch();
    const auto result = QJsonObject{
        {"type", "command_result"},
        {"id", request["id"]},
        {"ok", valid},
        {"message", valid ? "Connection activated" : "Connection selection changed; replacement cancelled"}};
    if (valid) {
        activeInstallationTransport = requested;
        if (transportDeadline)
            transportDeadline->stop();
        // Revoke old command leases at the same point that publication changes.
        const auto previous = authenticated;
        for (auto *other : previous)
            if (!isActiveInstallationPeer(other))
                other->abort();
        if (requested == "mqtt")
            setRemote({});
        else
            setMqtt({});
        int count = 0;
        for (auto *active : authenticated)
            count += isActiveInstallationPeer(active);
        emit clientCount(count, connections);
        updateTransportState();
        updateInstallationState(
            {{"activeType", requested}, {"switching", false}, {"switchError", ""}, {"status", "Connected"}});
        emit diagnostic("Active connection: " +
                        (requested == "mqtt" ? QString("MQTT") : QString("Home Assistant API")));
    }
    peer->sendTextMessage(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    if (valid)
        publish();
}
