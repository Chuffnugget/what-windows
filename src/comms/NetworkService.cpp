#include "comms/NetworkService.h"
#include <QCoreApplication>
#include <QHostInfo>
#include <QJsonDocument>
#include <QDateTime>
#include <QJsonArray>
#include <QMutexLocker>
#include <openssl/crypto.h>
#include <QUuid>
#include <QRegularExpression>
#include <QUrl>
#include <cmath>
#include <algorithm>
#include <utility>
#include <QRandomGenerator>

void NetworkWorker::initialize() {
    mqtt = new MqttBridge(this);
    connect(mqtt, &MqttBridge::peerReady, this, &NetworkWorker::attachPeer);
    connect(mqtt, &MqttBridge::status, this, &NetworkWorker::diagnostic);
    connect(mqtt, &MqttBridge::diagnostic, this, &NetworkWorker::diagnostic);
    connect(mqtt, &MqttBridge::status, this, &NetworkWorker::mqttState);
    connect(mqtt, &MqttBridge::progress, this, [this](QString phase, int retryMs) {
        emit connectionProgress("mqtt", phase, retryMs);
    });
    remoteRetry = new QTimer(this);
    remoteRetry->setSingleShot(true);
    connect(remoteRetry, &QTimer::timeout, this, &NetworkWorker::connectRemote);
    statsClock.start();
    publishClock.start();
    server = new QWebSocketServer("WHAT", QWebSocketServer::SecureMode, this);
    server->setMaxPendingConnections(8);
    server->setHandshakeTimeout(5000);
    timer = new QTimer(this);
    timer->setInterval(5000);
    connect(timer, &QTimer::timeout, this, &NetworkWorker::publish);
    flush = new QTimer(this);
    flush->setSingleShot(true);
    flush->setInterval(50);
    connect(flush, &QTimer::timeout, this, &NetworkWorker::publish);
    connect(server, &QWebSocketServer::newConnection, this, &NetworkWorker::accept);
    feedbackTimer = new QTimer(this);
    feedbackTimer->setInterval(5000);
    connect(feedbackTimer, &QTimer::timeout, this, &NetworkWorker::probeHomeAssistant);
    feedbackTimer->start();
    lookupTimer = new QTimer(this);
    lookupTimer->setSingleShot(true);
    lookupTimer->setInterval(100);
    connect(lookupTimer, &QTimer::timeout, this, &NetworkWorker::probeHomeAssistant);
}

void NetworkWorker::setEntityLookup(QString reading, QStringList keys) {
    for (auto *installation : installations)
        installation->setEntityLookup(reading, keys);
    lookupReading = reading;
    lookupKeys = keys;
    // Coalesce rapid row changes and discard responses for the previous row.
    for (auto &peer : feedbackPeers) {
        peer.nonce.clear();
        peer.due = false;
    }
    lookupTimer->start();
}

void NetworkWorker::probeHomeAssistant() {
    for (auto &probe : feedbackPeers)
        probe.due = true;
    sendPendingProbes();
}

void NetworkWorker::sendPendingProbes() {
    if (stopped)
        return;
    // A failed send may synchronously disconnect a peer and remove its state.
    const auto peers = feedbackPeers.keys();
    for (auto *peer : peers) {
        if (!feedbackPeers.contains(peer))
            continue;
        auto &probe = feedbackPeers[peer];
        if (!probe.due || !authenticated.contains(peer) || !isActiveInstallationPeer(peer)
            || peer->closed || peer->bytesToWrite())
            continue;
        if (!probe.nonce.isEmpty()) {
            if (probe.sent.elapsed() < 12000)
                continue;
            emit entityLookupResult(probe.reading, {}, "Home Assistant did not reply. Retrying…");
            if (diagnosticsEnabled)
                emit diagnostic(QString("%1 debug: application reply timeout; elapsed_ms=%2")
                                    .arg(peer->mqtt ? "MQTT" : "WebSocket").arg(probe.sent.elapsed()));
        }
        probe.nonce = QUuid::createUuid().toString(QUuid::Id128);
        probe.reading = lookupReading;
        probe.sent.restart();
        probe.due = false;
        const QJsonObject request{{"type", "ui_probe"}, {"id", probe.nonce},
                                  {"keys", QJsonArray::fromStringList(lookupKeys)}};
        peer->sendTextMessage(QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
    }
}

void NetworkWorker::configure(QString id, QString secret, QSslConfiguration ssl) {
    appliedIdentity = control->identityGeneration.load();
    if (token != secret)
        for (auto &client : commandClients)
            *client.alive = false;
    if (token != secret) {
        const auto old = authenticated;
        for (auto *s : old)
            s->close(QWebSocketProtocol::CloseCodePolicyViolated, "Pairing identity changed");
    }
    device = id;
    token = secret;
    server->setSslConfiguration(ssl);
    for (auto *installation : installations)
        installation->configure(device, installation->installationOptions["token"].toString(), ssl);
    if (!stopped)
        mqtt->configure(mqttOptions, device, token);
}

bool NetworkWorker::listen(QHostAddress address, quint16 port) {
    const bool ok = server->listen(address, port);
    error = server->errorString();
    if (ok) {
        stopped = false;
        timer->start();
        for (auto *installation : installations)
            installation->startInstallation();
        mqtt->configure(mqttOptions, device, token);
        if (!remoteUrl.isEmpty()) {
            remoteRetry->start(1500);
            emit connectionProgress("remote", "retrying", remoteRetry->interval());
        }
    }
    return ok;
}

void NetworkWorker::stop() {
    stopped = true;
    for (auto *installation : installations)
        installation->stop();
    const auto pending = pendingPairings;
    for (auto *socket : pending)
        socket->abort();
    mqtt->stop();
    emit connectionProgress("mqtt", "disabled");
    emit connectionProgress("remote", "disabled");
    remoteRetry->stop();
    if (outbound)
        outbound->abort();
    for (auto &client : commandClients)
        *client.alive = false;
    timer->stop();
    if (transportDeadline)
        transportDeadline->stop();
    flush->stop();
    server->close();
    const auto old = clients;
    for (auto *s : old)
        s->abort();
}

void NetworkWorker::setSnapshot(QJsonObject value) {
    for (auto *installation : installations)
        installation->setSnapshot(value);
    latest = installationSnapshot(value);
    receivedAt.restart();
    // Batch samples for up to 50 ms. Restarting the timer on every sample would
    // delay delivery indefinitely while readings keep arriving.
    if (!stopped && (!flush->isActive() || flush->remainingTime() > 50))
        flush->start(50);
}

void NetworkWorker::replaceSnapshot(QJsonObject value) {
    for (auto *installation : installations) {
        installation->replaceSnapshot(value);
        installation->appliedGeneration = appliedGeneration;
    }
    if (managedInstallations) {
        latest = value;
        receivedAt.restart();
        return;
    }
    pendingEvents.clear();
    publishingPolicies.clear();
    // Discard data still buffered by Qt under the previous sharing policy.
    const auto old = authenticated;
    for (auto *s : old)
        if (s->bytesToWrite() > 0)
            s->abort();
    setSnapshot(value);
}

void NetworkWorker::eventSnapshot(QJsonObject value) {
    for (auto *installation : installations)
        installation->eventSnapshot(value);
    if (managedInstallations) {
        latest = value;
        receivedAt.restart();
        return;
    }
    setSnapshot(value);
    const auto old = authenticated;
    for (auto *s : old) {
        if (pendingEvents[s].size() >= 32) {
            emit diagnostic("Slow Home Assistant client exceeded event queue; reconnecting for a full resync");
            s->abort();
            continue;
        }
        pendingEvents[s].enqueue(installationSnapshot(value));
    }
    publish();
}

void NetworkWorker::setInterval(int ms) {
    for (auto *installation : installations)
        installation->setInterval(ms);
    if (timer->interval() != ms)
        timer->setInterval(ms);
}

void NetworkWorker::publish() {
    if (stopped || (installationAlive && !installationAlive->load()))
        return;
    if (control->generation.load() != appliedGeneration || latest.isEmpty() || authenticated.isEmpty())
        return;
    auto data = latest;
    const auto elapsed = receivedAt.isValid() ? receivedAt.elapsed() / 1000 : 0;
    data["sample_age"] = latest["sample_age"].toDouble() + elapsed;
    // Do not re-publish stale snapshots indefinitely if the producer/UI stalls.
    if (elapsed > qMax(15, latest["publish_seconds"].toInt() * 3)) {
        const auto old = authenticated;
        for (auto *s : old)
            s->abort();
        return;
    }
    const auto old = authenticated;
    for (auto *s : old) {
        if (!isActiveInstallationPeer(s) && lastFrames.contains(s))
            continue; // The candidate gets one validation snapshot, not a second live stream.
        if (s->bytesToWrite() > 0) {
            if (!blockedSince.contains(s))
                blockedSince[s].start();
            if (blockedSince[s].elapsed() > 30000)
                s->abort();
            continue; // One outstanding frame; later ticks use the newest snapshot.
        }
        blockedSince.remove(s);
        const bool event = !pendingEvents[s].isEmpty();
        auto frame = publishingPolicies[s].apply(event ? pendingEvents[s].head() : data, s->cloud,
                                                 publishClock.elapsed(), event);
        // Event frames also need current diagnostics; they may have been queued
        // before another installation changed state.
        if (!installationId.isEmpty()) {
            const auto *owner = qobject_cast<NetworkWorker *>(parent());
            frame["connections"] = owner->installationDiagnostics;
            frame["connection_options"] = installationSnapshot({{"_metadata", true}})["connection_options"];
            if (transportPeers.contains(s))
                frame["connection_config"] = installationConnectionConfig(s);
        }
        const int due = publishingPolicies[s].nextWake();
        if (due && (!flush->isActive() || flush->remainingTime() > due))
            flush->start(due);
        auto metadata = frame;
        for (auto key : {"sensors", "sample_time", "sample_age"})
            metadata.remove(key);
        const auto icue = frame.value("icue").toObject();
        const auto automation = frame.value("icue_automation").toObject();
        QJsonObject current;
        for (const auto &row : frame["sensors"].toArray())
            current.insert(row.toObject()["id"].toString(), row);
        if (deltaClients.contains(s)) {
            const int sequence = sequences.value(s) + 1;
            frame["sequence"] = sequence;
            if (baselines.contains(s)) {
                frame["type"] = "delta";
                frame["base_sequence"] = sequences.value(s);
                QJsonArray changed, removed;
                const auto previous = baselines.value(s);
                for (auto it = current.begin(); it != current.end(); ++it)
                    if (!previous.contains(it.key()) || previous.value(it.key()) != it.value())
                        changed.append(it.value());
                for (auto it = previous.begin(); it != previous.end(); ++it)
                    if (!current.contains(it.key()))
                        removed.append(it.key());
                frame["sensors"] = changed;
                frame["removed"] = removed;
                if (icueBaselines.contains(s) && icueBaselines[s] == icue)
                    frame.remove("icue");
                if (automationBaselines.contains(s) && automationBaselines[s] == automation)
                    frame.remove("icue_automation");
            }
        }
        if (frame["type"] == "delta" && frame["sensors"].toArray().isEmpty() &&
            frame["removed"].toArray().isEmpty() && !frame.contains("icue") &&
            !frame.contains("icue_automation") && !event && lastMetadata.value(s) == metadata &&
            publishClock.elapsed() - lastFrames.value(s) >= 0 &&
            publishClock.elapsed() - lastFrames.value(s) < timer->interval())
            continue;
        // Fresh timing belongs to this send, not the cached sensor snapshot.
        // Receivers re-anchor deadlines so independent monotonic clocks cannot
        // accumulate drift over a connection lasting days or weeks.
        frame["sent_at"] = QDateTime::currentMSecsSinceEpoch() / 1000.0;
        if (commandClients.contains(s) && commandClients[s].relativeDeadlines)
            frame["session_ms"] = double(commandClients[s].clock.elapsed());
        const auto wireBytes = QJsonDocument(frame).toJson(QJsonDocument::Compact);
        if (wireBytes.size() > 8 * 1024 * 1024) {
            emit diagnostic("Snapshot exceeds protocol limit; reduce shared readings.");
            s->abort();
            continue;
        }
        if (control->generation.load() != appliedGeneration ||
            (installationAlive && !installationAlive->load()))
            return;
        if (s->sendTextMessage(QString::fromUtf8(wireBytes)) < 0) {
            s->abort();
            continue;
        }
        if (s->mqtt)
            mqtt->recordFrame(frame["type"] == "delta", frame["sensors"].toArray().size());
        icueBaselines[s] = icue;
        automationBaselines[s] = automation;
        if (deltaClients.contains(s)) {
            baselines[s] = current;
            sequences[s] = frame["sequence"].toInt();
        }
        if (event)
            pendingEvents[s].dequeue();
        lastMetadata[s] = metadata;
        lastFrames[s] = publishClock.elapsed();
        ++frameCount;
        byteCount += wireBytes.size();
        if (frame["type"] == "delta")
            ++deltaCount;
        emit transmitted();
    }
    if (statsClock.elapsed() >= 60000) {
        emit diagnostic(
            QString(
                "Stream totals: %1 frames (%2 delta), %3 KiB sent; %4 authenticated clients; heartbeat %5 s")
                .arg(frameCount)
                .arg(deltaCount)
                .arg(byteCount / 1024)
                .arg(authenticated.size())
                .arg(timer->interval() / 1000));
        statsClock.restart();
    }
}

void NetworkWorker::accept() {
    while (server->hasPendingConnections()) {
        auto *s = server->nextPendingConnection();
        if (managedInstallations) {
            acceptInstallation(s);
            continue;
        }
        if (clients.size() >= 8) {
            s->close();
            s->deleteLater();
            continue;
        }
        attachSocket(s);
    }
}

void NetworkWorker::updateTransportState() {
    bool local = false, cloud = false;
    QStringList versions;
    int mqttPeers = 0, apiPeers = 0;
    for (auto *socket : authenticated) {
        if (!isActiveInstallationPeer(socket))
            continue;
        socket->mqtt ? ++mqttPeers : ++apiPeers;
        if (socket->cloud)
            cloud = true;
        else
            local = true;
        QString version;
        if (socket->mqtt)
            version = socket->encrypted ? "MQTT TLS encrypted (version unavailable)" : "MQTT unencrypted";
        else
            switch (socket->sslConfiguration().sessionProtocol()) {
            case QSsl::TlsV1_2:
                version = "TLS 1.2";
                break;
            case QSsl::TlsV1_3:
                version = "TLS 1.3";
                break;
            default:
                version = "TLS (version unavailable)";
                break;
            }
        if (!socket->mqtt)
            version += " encrypted";
        if (!versions.contains(version))
            versions.append(version);
    }
    versions.sort();
    if (diagnosticsEnabled)
        emit diagnostic(QString("MQTT debug: active authenticated routes: MQTT=%1; Home Assistant API=%2; "
                                "local=%3; cloud=%4; preferred=%5")
                            .arg(mqttPeers).arg(apiPeers).arg(local).arg(cloud)
                            .arg(mqtt->preferred() ? "MQTT" : "Home Assistant API"));
    emit transportState(local && cloud ? "hybrid"
                        : cloud        ? "cloud"
                        : local        ? "local"
                                       : "none",
                        versions.join(" + "));
}

void NetworkWorker::attachSocket(QWebSocket *s) {
    attachPeer(new WirePeer(s, s == outbound, this));
}

void NetworkWorker::attachPeer(WirePeer *s) {
    clients.insert(s);
    s->setMaxAllowedIncomingFrameSize(32768);
    s->setMaxAllowedIncomingMessageSize(32768);
    QTimer::singleShot(5000, s, [this, s] {
        if (!authenticated.contains(s)) {
            if (diagnosticsEnabled)
                emit diagnostic(QString("%1 debug: protocol authentication timeout (5 s)")
                                    .arg(s->mqtt ? "MQTT" : "WebSocket"));
            s->close(QWebSocketProtocol::CloseCodePolicyViolated, "Authentication timeout");
        }
    });
    connect(s, &WirePeer::disconnected, this, [this, s] {
        if (commandClients.contains(s)) {
            *commandClients[s].alive = false;
            {
                QMutexLocker lock(&control->mutex);
                control->leases.remove(commandClients[s].session);
                control->leaseIdentities.remove(commandClients[s].session);
                control->installationLeases.remove(commandClients[s].session);
            }
            commandClients.remove(s);
        }
        publishingPolicies.remove(s);
        lastFrames.remove(s);
        lastMetadata.remove(s);
        pendingEvents.remove(s);
        deltaClients.remove(s);
        baselines.remove(s);
        icueBaselines.remove(s);
        automationBaselines.remove(s);
        sequences.remove(s);
        authenticated.remove(s);
        feedbackPeers.remove(s);
        transportPeers.remove(s);
        emit feedbackSupport(!feedbackPeers.isEmpty());
        clients.remove(s);
        blockedSince.remove(s);
        s->deleteLater();
        emit clientCount(std::count_if(authenticated.begin(), authenticated.end(),
                                      [this](WirePeer *peer) { return isActiveInstallationPeer(peer); }), connections);
        updateTransportState();
    });
    connect(s, &WirePeer::bytesWritten, this, [this, s](qint64) {
        // A busy telemetry write must not postpone the check by another five seconds.
        // The due flag and outstanding nonce keep this bounded to one probe per peer.
        if (!s->bytesToWrite())
            sendPendingProbes();
        if (!s->bytesToWrite() && (!pendingEvents.value(s).isEmpty() || blockedSince.contains(s)))
            QTimer::singleShot(0, this, [this] { publish(); });
    });
    connect(s, &WirePeer::binaryMessageReceived, s,
            [s] { s->close(QWebSocketProtocol::CloseCodeDatatypeNotSupported); });
    connect(s, &WirePeer::textMessageReceived, this, [this, s](QString message) {
        if (installationAlive && !installationAlive->load()) {
            s->abort();
            return;
        }
        auto object = QJsonDocument::fromJson(message.toUtf8()).object();
        if (authenticated.contains(s)) {
            if (control->generation.load() != appliedGeneration ||
                control->identityGeneration.load() != appliedIdentity)
                return;
            if (object["type"] == "transport_activate" && !installationId.isEmpty()) {
                activateInstallationTransport(s, object);
                return;
            }
            if (!isActiveInstallationPeer(s)) {
                s->close(QWebSocketProtocol::CloseCodePolicyViolated, "Connection is not active");
                return;
            }
            if (object["type"] == "ui_feedback" && feedbackPeers.contains(s)) {
                auto &probe = feedbackPeers[s];
                if (probe.nonce.isEmpty() || object["id"].toString() != probe.nonce)
                    return;
                const auto entries = object["entities"].toArray();
                QStringList entities;
                static const QRegularExpression entityId("^[a-z_]{1,64}\\.[a-z0-9_]{1,255}$");
                if (!object["entities"].isArray() || entries.size() > 32) {
                    s->close(QWebSocketProtocol::CloseCodePolicyViolated, "Invalid UI feedback");
                    return;
                }
                for (const auto &entry : entries) {
                    if (!entry.isString() || !entityId.match(entry.toString()).hasMatch()) {
                        s->close(QWebSocketProtocol::CloseCodePolicyViolated, "Invalid entity ID");
                        return;
                    }
                    entities.append(entry.toString());
                }
                if (s->mqtt)
                    mqtt->recordFeedback(probe.sent.elapsed());
                probe.nonce.clear();
                emit haCommunication(QDateTime::currentMSecsSinceEpoch());
                emit entityLookupResult(probe.reading, entities, entities.isEmpty()
                    ? "No Home Assistant entity is registered for this entry. It may be unshared or a local-only control."
                    : QString());
                sendPendingProbes();
                return;
            }
            if (!commandClients.contains(s)) {
                s->close(QWebSocketProtocol::CloseCodePolicyViolated, "Commands were not negotiated");
                return;
            }
            auto &client = commandClients[s];
            const auto id = object["id"].toString();
            static const QRegularExpression commandId("^[a-zA-Z0-9_-]{1,64}$");
            static const QRegularExpression commandAction("^[a-z_]{1,32}$");
            if (!client.alive->load() || object["type"] != "command" ||
                !commandId.match(id).hasMatch() ||
                !commandAction.match(object["action"].toString()).hasMatch() ||
                !object["args"].isObject()) {
                s->close(QWebSocketProtocol::CloseCodePolicyViolated, "Invalid command envelope");
                return;
            }
            if (!client.rate.isValid() || client.rate.elapsed() >= 10000) {
                client.rate.restart();
                client.count = 0;
            }
            const auto receivedAt = client.clock.elapsed();
            while (!client.seenOrder.isEmpty() && receivedAt - client.seenOrder.head().first > 120000)
                client.seen.remove(client.seenOrder.dequeue().second);
            // Reject the command, but keep the connection: dropping it would make every
            // entity unavailable just because a script or slider sent too much at once.
            if (++client.count > 30 || client.seen.size() >= 4096) {
                replyCommand(client.session, id, false, "Too many commands; wait a few seconds and try again");
                return;
            }
            if (client.relativeDeadlines || s->mqtt || (s->socket == outbound && outbound)) {
                const auto key = client.relativeDeadlines ? "deadline_ms" : "expires_at";
                const auto expires = object[key].toDouble();
                const auto now = client.relativeDeadlines ? receivedAt : QDateTime::currentMSecsSinceEpoch();
                if (!object[key].isDouble() || !std::isfinite(expires) || expires < now ||
                    expires > now + 60000) {
                    replyCommand(client.session, id, false, client.relativeDeadlines ? "Command deadline expired"
                        : "Expired command or clock mismatch; synchronise the PC and Home Assistant clocks or update both to support session-relative deadlines");
                    return;
                }
            }
            if (client.seen.contains(id)) {
                replyCommand(client.session, id, false, "Duplicate command rejected");
                return;
            }
            if (!installationId.isEmpty() && !installationOptions["controls"].toBool(true) &&
                object["action"] != "entity_cleanup_result") {
                replyCommand(client.session, id, false, "Controls are disabled for this installation");
                return;
            }
            if (object["action"] == "dynamic_lighting" && s->cloud &&
                !latest["dynamic_lighting"].toObject()["cloud"].toBool()) {
                replyCommand(client.session, id, false, "Dynamic Lighting cloud control is disabled.");
                return;
            }
            if (object["action"] == "wallpaper" && s->cloud &&
                !latest["wallpaper"].toObject()["cloud"].toBool()) {
                replyCommand(client.session, id, false, "Wallpaper Engine cloud control is disabled", {});
                return;
            }
            if (object["action"] == "beacn" && s->cloud && !latest["beacn"].toObject()["cloud"].toBool()) {
                replyCommand(client.session, id, false, "BEACN cloud control is disabled");
                return;
            }
            object["_policy_generation"] = double(appliedGeneration);
            if (object["action"] == "custom_action") {
                // Overwrite any caller-supplied value with the authenticated transport scope.
                object["_custom_cloud"] = s->cloud;
                const auto custom = latest["custom"].toObject();
                if (s->cloud && (!custom["cloud"].toBool() || !custom["_cloud_actions"].toArray().contains(
                                                                  object["args"].toObject()["id"]))) {
                    replyCommand(client.session, id, false,
                                 "This saved command is not allowed over cloud connections");
                    return;
                }
            }
            client.seen.insert(id);
            client.seenOrder.enqueue({receivedAt, id});
            emit commandRequested(client.session, object);
            return;
        }
        const auto provided = object["token"].toString().toLatin1(), expected = token.toLatin1();
        if (object["type"] != "auth" || object["protocol"].toInt() != 2 ||
            provided.size() != expected.size() || expected.isEmpty() ||
            CRYPTO_memcmp(provided.data(), expected.data(), expected.size()) != 0) {
            if (diagnosticsEnabled)
                emit diagnostic(QString("%1 debug: protocol authentication rejected; expected protocol=2")
                                    .arg(s->mqtt ? "MQTT" : "WebSocket"));
            s->close(QWebSocketProtocol::CloseCodePolicyViolated, "Authentication failed");
            return;
        }
        if (!installationId.isEmpty()) {
            const bool desired = s->mqtt == (installationOptions["type"] == "mqtt");
            if ((!isActiveInstallationPeer(s) && !desired)
                || (!isActiveInstallationPeer(s) && !object["transport_switch_v1"].toBool())) {
                s->close(QWebSocketProtocol::CloseCodePolicyViolated, "Use the selected connection and updated WHAT integration");
                return;
            }
            if (object["transport_switch_v1"].toBool())
                transportPeers.insert(s);
        }
        if (!s->cloud && !s->mqtt && outbound && isActiveInstallationPeer(s))
            outbound->abort();
        authenticated.insert(s);
        if (object["ui_feedback_v1"].toBool())
            feedbackPeers.insert(s, {});
        emit feedbackSupport(!feedbackPeers.isEmpty());
        ++connections;
        if (s->socket == outbound && outbound) {
            remoteDelay = 1000;
            emit remoteState("Connected remotely to Home Assistant");
        }
        updateTransportState();
        if (object["commands_v1"].toBool()) {
            CommandClient created;
            created.session = QUuid::createUuid().toString(QUuid::WithoutBraces);
            created.clock.start();
            created.relativeDeadlines = object["command_deadline_v1"].toBool();
            created.alive = std::make_shared<std::atomic_bool>(true);
            commandClients.insert(s, created);
            QMutexLocker lock(&control->mutex);
            const auto &client = commandClients[s];
            control->leases.insert(client.session, client.alive);
            control->leaseIdentities.insert(client.session, appliedIdentity);
            if (installationAlive)
                control->installationLeases.insert(client.session, installationAlive);
        }
        if (object["delta_v1"].toBool())
            deltaClients.insert(s);
        if (s->mqtt)
            emit mqttState("MQTT authenticated Home Assistant session active");
        if (diagnosticsEnabled)
            emit diagnostic(QString("%1 debug: protocol authentication accepted; protocol=2; scope=%2; "
                                    "delta=%3; controls=%4; ui_feedback=%5")
                                .arg(s->mqtt ? "MQTT" : "WebSocket", s->cloud ? "cloud" : "local")
                                .arg(deltaClients.contains(s)).arg(commandClients.contains(s))
                                .arg(feedbackPeers.contains(s)));
        emit diagnostic(deltaClients.contains(s)
                            ? "Home Assistant connected: changed-reading stream (50 ms batch; separate heartbeat)"
                            : "Home Assistant connected: compatible full-snapshot stream");
        const QJsonObject hello{{"type", "hello"},
                                {"protocol", 2},
                                {"device_id", device},
                                {"name", QHostInfo::localHostName()},
                                {"app_version", QCoreApplication::applicationVersion()},
                                {"delta_v1", deltaClients.contains(s)},
                                {"commands_v1", commandClients.contains(s)},
                                {"command_deadline_v1", commandClients.contains(s) && commandClients[s].relativeDeadlines},
                                {"ui_feedback_v1", feedbackPeers.contains(s)},
                                {"transport_switch_v1", transportPeers.contains(s)}};
        s->sendTextMessage(QString::fromUtf8(QJsonDocument(hello).toJson(QJsonDocument::Compact)));
        // Send the initial snapshot once hello has drained, without building
        // a queue for slow clients. The periodic timer handles later frames.
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(s, &WirePeer::bytesWritten, this, [this, s, connection](qint64) {
            if (!s->bytesToWrite()) {
                disconnect(*connection);
                publish();
            }
        });
        emit clientCount(std::count_if(authenticated.begin(), authenticated.end(),
                                      [this](WirePeer *peer) { return isActiveInstallationPeer(peer); }), connections);
        lookupTimer->start();
    });
}

NetworkService::NetworkService(QObject *parent) : QObject(parent), worker(new NetworkWorker) {
    thread->setObjectName("Home Assistant WebSocket communications");
    worker->moveToThread(thread);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(worker, &NetworkWorker::listenFinished, this,
            [this](quint64 request, bool ok, quint16 port, QString message) {
                if (request != lifecycle)
                    return;
                starting = false;
                running = ok;
                error = message;
                emit listenFinished(request, ok, port, message);
            });
    connect(worker, &NetworkWorker::clientCount, this, &NetworkService::clientCount);
    connect(worker, &NetworkWorker::installationTransportFailed, this, &NetworkService::installationTransportFailed);
    connect(worker, &NetworkWorker::transmitted, this, &NetworkService::transmitted);
    connect(worker, &NetworkWorker::haCommunication, this, &NetworkService::haCommunication);
    connect(worker, &NetworkWorker::feedbackSupport, this, &NetworkService::feedbackSupport);
    connect(worker, &NetworkWorker::entityLookupResult, this, &NetworkService::entityLookupResult);
    connect(worker, &NetworkWorker::remoteState, this, &NetworkService::remoteState);
    connect(worker, &NetworkWorker::mqttState, this, &NetworkService::mqttState);
    connect(worker, &NetworkWorker::connectionProgress, this, &NetworkService::connectionProgress);
    connect(worker, &NetworkWorker::transportState, this, &NetworkService::transportState);
    connect(worker, &NetworkWorker::diagnostic, this, &NetworkService::diagnostic);
    connect(worker, &NetworkWorker::commandRequested, this, &NetworkService::commandRequested);
    connect(worker, &NetworkWorker::installationState, this, &NetworkService::installationState);
    connect(worker, &NetworkWorker::installationEntityLookup, this, &NetworkService::installationEntityLookup);
    thread->start();
    QMetaObject::invokeMethod(worker, &NetworkWorker::initialize, Qt::QueuedConnection);
}

void NetworkService::setEntityLookup(QString reading, QStringList keys) {
    QMetaObject::invokeMethod(worker, [target = worker, reading, keys] {
        target->setEntityLookup(reading, keys);
    });
}

NetworkService::~NetworkService() {
    revokeLeases();
    ++worker->control->generation;
    QMetaObject::invokeMethod(
        worker,
        [target = worker, t = thread] {
            target->stop();
            t->quit();
        },
        Qt::QueuedConnection);
    // Give the worker a moment to stop. If an OS or MQTT call is stuck, leave the
    // thread running; it deletes itself (and the worker) when it finishes.
    thread->wait(250);
}

void NetworkService::revokeLeases() {
    QMutexLocker lock(&worker->control->mutex);
    for (auto lease : worker->control->leases)
        *lease = false;
}

void NetworkService::listenAsync(QHostAddress address, quint16 port) {
    starting = true;
    const auto request = ++lifecycle;
    QMetaObject::invokeMethod(
        worker,
        [target = worker, address, port, request] {
            const bool ok = target->listen(address, port);
            emit target->listenFinished(request, ok, port, target->error);
        },
        Qt::QueuedConnection);
}

bool NetworkService::listen(QHostAddress address, quint16 port) {
    QMetaObject::invokeMethod(
        worker,
        [this, address, port] {
            running = worker->listen(address, port);
            error = worker->error;
        },
        Qt::BlockingQueuedConnection);
    return running;
}

void NetworkService::close() {
    ++lifecycle;
    revokeLeases();
    QMetaObject::invokeMethod(worker, &NetworkWorker::stop, Qt::QueuedConnection);
    running = false;
    starting = false;
}

void NetworkService::configure(QString device, QString token, QSslConfiguration ssl) {
    const auto generation = ++worker->control->identityGeneration;
    revokeLeases();
    QMetaObject::invokeMethod(
        worker,
        [=, target = worker] {
            target->configure(device, token, ssl);
            target->appliedIdentity = generation;
        },
        Qt::QueuedConnection);
}

void NetworkService::setInterval(int ms) {
    QMetaObject::invokeMethod(worker, [=, target = worker] { target->setInterval(ms); });
}

void NetworkService::setSnapshot(QJsonObject value) {
    enqueue(value, false);
}

std::shared_ptr<std::atomic_bool> NetworkWorker::commandLease(const QString &session) {
    for (const auto &client : commandClients)
        if (client.session == session)
            return client.alive;
    return {};
}

void NetworkWorker::replyCommand(const QString &session, const QString &id, bool ok, const QString &message,
                                 QJsonObject result) {
    for (auto *installation : installations)
        installation->replyCommand(session, id, ok, message, result);
    for (auto it = commandClients.begin(); it != commandClients.end(); ++it)
        if (it->session == session) {
            QJsonObject reply{
                {"type", "command_result"}, {"id", id}, {"ok", ok}, {"message", message.left(512)}};
            if (!result.isEmpty())
                reply["result"] = result;
            const auto submitted = it.key()->sendTextMessage(
                QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact)));
            if (it.key()->mqtt)
                mqtt->recordResult(ok, submitted >= 0);
            return;
        }
}

std::shared_ptr<std::atomic_bool> NetworkService::commandLease(const QString &session) {
    if (!running)
        return {};
    QMutexLocker lock(&worker->control->mutex);
    if (worker->control->leaseIdentities.value(session) != worker->control->identityGeneration.load())
        return {};
    const auto installation = worker->control->installationLeases.value(session);
    if (installation && !installation->load())
        return {};
    return worker->control->leases.value(session);
}

void NetworkService::replyCommand(const QString &session, const QString &id, bool ok, const QString &message,
                                  QJsonObject result) {
    QMetaObject::invokeMethod(
        worker, [=, target = worker] { target->replyCommand(session, id, ok, message, result); });
}

void NetworkService::setEventSnapshot(QJsonObject value) {
    enqueue(value, true);
}

void NetworkService::replaceSnapshot(QJsonObject value) {
    {
        QMutexLocker lock(&mailbox->mutex);
        mailbox->values.clear();
        mailbox->queued = false;
        ++mailbox->ticket;
    }
    // Immediately suppress old-policy sends, then discard queued data on the worker.
    // The worker resumes only after the newest policy generation has been applied.
    const auto generation = ++worker->control->generation;
    QMetaObject::invokeMethod(
        worker,
        [target = worker, value, generation] {
            if (generation != target->control->generation.load())
                return;
            target->appliedGeneration = generation;
            target->replaceSnapshot(value);
        },
        Qt::QueuedConnection);
}

void NetworkService::enqueue(QJsonObject value, bool event) {
    QMutexLocker lock(&mailbox->mutex);
    auto &values = mailbox->values;
    // Coalesce only adjacent regular samples, never across an event boundary.
    if (!event && !values.isEmpty() && !values.back().event) {
        values.back().value = value;
        values.back().generation = worker->control->generation.load();
    } else {
        bool reset = values.size() >= 64;
        if (reset)
            values.clear();
        values.enqueue({value, event, reset, worker->control->generation.load()});
    }
    if (mailbox->queued)
        return;
    mailbox->queued = true;
    auto box = mailbox;
    auto target = worker;
    const auto ticket = ++mailbox->ticket;
    QMetaObject::invokeMethod(worker, [box, target, ticket] {
        QQueue<PendingSnapshot> batch;
        {
            QMutexLocker guard(&box->mutex);
            if (ticket != box->ticket)
                return;
            batch.swap(box->values);
            box->queued = false;
        }
        for (const auto &entry : batch) {
            if (entry.generation != target->control->generation.load())
                continue;
            if (entry.reset) {
                emit target->diagnostic(
                    "Producer queue exceeded limit; discarded backlog and resynchronised latest state");
                target->replaceSnapshot(entry.value);
            } else if (entry.event)
                target->eventSnapshot(entry.value);
            else
                target->setSnapshot(entry.value);
        }
    });
}

void NetworkWorker::setRemote(QString url) {
    if (managedInstallations)
        return;
    remoteUrl = url;
    remoteDelay = 1000;
    remoteRetry->stop();
    if (outbound)
        outbound->abort();
    emit connectionProgress("remote", "disabled");
    if (!stopped && !remoteUrl.isEmpty()) {
        remoteRetry->start(1000);
        emit connectionProgress("remote", "retrying", remoteRetry->interval());
    }
}

void NetworkWorker::connectRemote() {
    if (stopped || remoteUrl.isEmpty() || outbound)
        return;
    for (auto *peer : authenticated)
        if (!peer->mqtt || mqtt->preferred()) {
            emit remoteState("Existing connection active; remote standby");
            emit connectionProgress("remote", "standby");
            remoteRetry->start(5000);
            return;
        }
    const QUrl url(remoteUrl);
    if (url.scheme() != "wss" || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() ||
        url.hasFragment()) {
        emit diagnostic("Remote URL rejected");
        emit remoteState("Remote URL rejected; check the Home Assistant address");
        emit connectionProgress("remote", "failed");
        return;
    }
    auto *socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    outbound = socket;
    socket->setMaxAllowedIncomingFrameSize(32768);
    socket->setMaxAllowedIncomingMessageSize(32768);
    auto ssl = QSslConfiguration::defaultConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    socket->setSslConfiguration(ssl);
    auto connected = std::make_shared<bool>(false);
    // A failed handshake can report both errorOccurred and disconnected. Only the
    // first one schedules the retry, so the backoff doubles once per failure.
    auto retryScheduled = std::make_shared<bool>(false);
    connect(socket, &QWebSocket::connected, this, [this, socket, connected] {
        if (socket != outbound || stopped || socket->requestUrl() != QUrl(remoteUrl)) {
            socket->abort();
            return;
        }
        *connected = true;
        attachSocket(socket);
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "remote_auth"},
                                                                            {"protocol", 2},
                                                                            {"device_id", device},
                                                                            {"token", token}})
                                                      .toJson(QJsonDocument::Compact)));
        emit diagnostic("Remote TLS connected; authenticating with Home Assistant");
        emit connectionProgress("remote", "authenticating");
    });
    connect(socket, &QWebSocket::disconnected, this, [this, socket, connected, retryScheduled] {
        if (outbound == socket) {
            outbound = nullptr;
            const auto message =
                socket->closeCode() == QWebSocketProtocol::CloseCodePolicyViolated
                    ? QString(
                          "Home Assistant rejected remote authentication; check pairing and update the WHAT integration")
                    : QString("Remote disconnected; reconnect scheduled");
            emit remoteState(message);
            if (socket->closeCode() == QWebSocketProtocol::CloseCodePolicyViolated)
                emit diagnostic(message);
        }
        if (!*connected)
            socket->deleteLater();
        if (std::exchange(*retryScheduled, true))
            return;
        if (!stopped && !remoteUrl.isEmpty()) {
            remoteRetry->start(remoteDelay + QRandomGenerator::global()->bounded(500));
            emit connectionProgress("remote", "retrying", remoteRetry->interval());
            remoteDelay = qMin(remoteDelay * 2, 60000);
        }
    });
    connect(socket, &QWebSocket::errorOccurred, this,
            [this, socket, retryScheduled](QAbstractSocket::SocketError error) {
        // Extract only a status code: raw Qt errors can contain the private endpoint URL.
        const auto status = QRegularExpression("(?:status code|status):?\\s*([45][0-9]{2})",
                                               QRegularExpression::CaseInsensitiveOption)
                                .match(socket->errorString())
                                .captured(1);
        QString message;
        if (status == "404")
            message =
                "Remote HTTP 404: WHAT endpoint not found; install/update the WHAT Home Assistant integration and restart Home Assistant";
        else if (!status.isEmpty())
            message = "Remote HTTP " + status +
                      ": check Home Assistant remote access and reverse-proxy WebSocket settings";
        else if (error == QAbstractSocket::HostNotFoundError)
            message = "Remote hostname not found; check the cloud address and DNS";
        else if (error == QAbstractSocket::ConnectionRefusedError)
            message = "Remote connection refused; check Home Assistant remote access and port";
        else if (error == QAbstractSocket::SslHandshakeFailedError)
            message = "Remote TLS handshake failed; check certificate trust, expiry and the PC clock";
        else
            message = "Remote connection failed (network error " + QString::number(int(error)) +
                      "); check connectivity and Home Assistant remote access";
        emit diagnostic(message);
        emit remoteState(message);
        emit connectionProgress("remote", "failed");
        // Failed handshakes may never emit disconnected.
        if (socket == outbound && socket->state() == QAbstractSocket::UnconnectedState) {
            outbound = nullptr;
            socket->deleteLater();
            if (!std::exchange(*retryScheduled, true) && !stopped && !remoteUrl.isEmpty()) {
                remoteRetry->start(remoteDelay);
                emit connectionProgress("remote", "retrying", remoteRetry->interval());
                remoteDelay = qMin(remoteDelay * 2, 60000);
            }
        }
    });
    QTimer::singleShot(15000, socket, [this, socket] {
        bool found = false;
        for (auto *peer : authenticated)
            if (peer->socket == socket)
                found = true;
        if (!found)
            socket->abort();
    });
    emit remoteState("Connecting securely to Home Assistant");
    emit connectionProgress("remote", "connecting");
    emit diagnostic("Connecting to configured Home Assistant remote endpoint");
    socket->open(url);
}

void NetworkService::setRemote(QString url) {
    QMetaObject::invokeMethod(worker, [target = worker, url] { target->setRemote(url); });
}

void NetworkWorker::setMqtt(QJsonObject options) {
    if (managedInstallations)
        return;
    const bool onlyPreference = (options.keys() == mqttOptions.keys());
    auto a = options, b = mqttOptions;
    a.remove("preferred");
    b.remove("preferred");
    mqttOptions = options;
    if (!stopped) {
        if (onlyPreference && a == b)
            mqtt->setPreferred(options["preferred"].toBool());
        else
            mqtt->configure(options, device, token);
    }
}

void NetworkService::setMqtt(QJsonObject options) {
    QMetaObject::invokeMethod(worker, [target = worker, options] { target->setMqtt(options); });
}

void NetworkWorker::setDiagnostics(bool enabled) {
#ifndef QT_NO_DEBUG
    enabled = true;
#endif
    diagnosticsEnabled = enabled;
    for (auto *installation : installations)
        installation->setDiagnostics(enabled);
    mqtt->setDiagnostics(enabled);
    if (enabled)
        updateTransportState();
}

void NetworkService::setDiagnostics(bool enabled) {
    QMetaObject::invokeMethod(worker, [target = worker, enabled] { target->setDiagnostics(enabled); });
}
