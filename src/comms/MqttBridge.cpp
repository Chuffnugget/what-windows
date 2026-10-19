#include "MqttBridge.h"
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QUuid>
#include <QDateTime>
#include <openssl/crypto.h>
#include <cstring>
#include <cmath>
#include <QFileInfo>

namespace {
QByteArray signature(const QString &key, const QString &direction, const QString &epoch,
                     const QString &session, qint64 seq, const QString &body) {
    return QMessageAuthenticationCode::hash(
               (direction + "\n" + epoch + "\n" + session + "\n" + QString::number(seq) + "\n" + body)
                   .toUtf8(),
               key.toUtf8(), QCryptographicHash::Sha256)
        .toHex();
}
} // namespace

MqttBridge::MqttBridge(QObject *parent) : QObject(parent) {
    announce.setInterval(5000);
    retry.setInterval(5000);
    retry.setSingleShot(true);
    connect(&announce, &QTimer::timeout, this, [this] {
        advertise();
        if (diagnosticsEnabled && trafficClock.elapsed() >= 15000)
            reportTraffic();
        if (peer && QDateTime::currentMSecsSinceEpoch() - lastReceived > 45000) {
            debug("Home Assistant session timed out: no valid inbound message for 45 s");
            peer->close();
        }
    });
    connect(&retry, &QTimer::timeout, this, &MqttBridge::start);
}

MqttBridge::~MqttBridge() {
    stop();
}

void MqttBridge::debug(const QString &message) {
    if (diagnosticsEnabled)
        emit diagnostic("MQTT debug: " + message);
}

void MqttBridge::setDiagnostics(bool enabled) {
    if (diagnosticsEnabled == enabled)
        return;
    diagnosticsEnabled = enabled;
    traffic = {};
    completed = callbackDrops = 0;
    trafficClock.start();
    if (enabled)
        debug(QString("trace enabled; broker=%1; session=%2; preferred=%3; scope=%4; TLS=%5; port=%6; "
                      "credentials=%7; trust=%8; client_certificate=%9; QoS=0; retained=no; "
                      "announce=5 s; receive_timeout=45 s; retry=5 s; summaries=15 s")
                  .arg(online ? "connected" : "disconnected", peer ? "open" : "none")
                  .arg(preferred())
                  .arg(config["scope"] == "local" ? "local" : "cloud")
                  .arg(config["tls"].toBool(true))
                  .arg(config["port"].toInt(8883))
                  .arg(!config["username"].toString().isEmpty())
                  .arg(config["ca"].toString().isEmpty() ? "Windows" : "custom")
                  .arg(!config["certificate"].toString().isEmpty()));
}

void MqttBridge::reject(const QString &reason) {
    if (!diagnosticsEnabled)
        return;
    // Reasons are fixed strings. Log the first occurrence, then aggregate repeats.
    if (++traffic.rejected[reason] == 1)
        debug("discarded message: " + reason);
}

void MqttBridge::recordFrame(bool delta, int readings) {
    if (!diagnosticsEnabled)
        return;
    if (traffic.full + traffic.delta == 0)
        debug(QString("first snapshot submitted; delta=%1; readings=%2").arg(delta).arg(readings));
    ++(delta ? traffic.delta : traffic.full);
    traffic.lastReadings = readings;
}

void MqttBridge::recordFeedback(qint64 elapsedMs) {
    if (!diagnosticsEnabled)
        return;
    if (traffic.feedback++ == 0)
        debug(QString("first Home Assistant application reply; round_trip_ms=%1").arg(elapsedMs));
    traffic.lastRoundTripMs = elapsedMs;
    traffic.maxRoundTripMs = qMax(traffic.maxRoundTripMs, elapsedMs);
}

void MqttBridge::recordResult(bool ok, bool submitted) {
    if (!diagnosticsEnabled)
        return;
    ++(ok ? traffic.resultsOk : traffic.resultsFailed);
    if (!submitted)
        reject("result write not accepted");
}

void MqttBridge::reportTraffic() {
    if (!diagnosticsEnabled)
        return;
    QStringList reasons;
    for (auto it = traffic.rejected.cbegin(); it != traffic.rejected.cend(); ++it)
        reasons.append(it.key() + "=" + QString::number(it.value()));
    // Cumulative totals for this connection attempt. QoS 0 completion only means
    // a local write drained; application feedback is separate evidence from HA.
    debug(QString("totals: submitted=%1; write_completions=%2; bytes=%3; inflight=%4; "
                  "valid_inbound=%5; ping=%6; requests=%7; full=%8; delta=%9; last_frame_readings=%10; "
                  "app_replies=%11; round_trip_ms=%12; max_round_trip_ms=%13; result_ok=%14; result_no=%15; "
                  "last_inbound_age_ms=%16; tx_seq=%17; rx_seq=%18; callback_drops=%19; discarded={%20}")
              .arg(traffic.submitted).arg(completed.load()).arg(traffic.bytes).arg(outgoing.load())
              .arg(traffic.accepted).arg(traffic.pings).arg(traffic.requests)
              .arg(traffic.full).arg(traffic.delta).arg(traffic.lastReadings)
              .arg(traffic.feedback).arg(traffic.lastRoundTripMs).arg(traffic.maxRoundTripMs)
              .arg(traffic.resultsOk).arg(traffic.resultsFailed)
              .arg(lastReceived ? QDateTime::currentMSecsSinceEpoch() - lastReceived : -1)
              .arg(sent).arg(received).arg(callbackDrops.load()).arg(reasons.join("; ")));
    trafficClock.restart();
}

void MqttBridge::configure(QJsonObject options, QString id, QString token) {
    stop();
    emit progress("disabled");
    config = options;
    device = id;
    secret = token;
    base = "what/" + device;
    // Refresh the safe configuration snapshot without copying any broker secrets.
    if (diagnosticsEnabled) {
        diagnosticsEnabled = false;
        setDiagnostics(true);
    }
    if (!config["enabled"].toBool())
        emit status("MQTT disabled");
    start();
}

void MqttBridge::stop() {
    if (client) {
        reportTraffic();
        debug("stopping broker client; pending writes discarded; direct transport remains available");
    }
    ++generation;
    retry.stop();
    announce.stop();
    online = false;
    if (peer)
        peer->close();
    if (client) {
        MQTTAsync_destroy(&client);
        client = nullptr;
    }
    outgoing = 0;
}

void MqttBridge::setPreferred(bool value) {
    config["preferred"] = value;
    debug(QString("transport preference changed: %1; announcing handover")
              .arg(value ? "MQTT" : "Home Assistant API"));
    advertise();
}

void MqttBridge::start() {
    if (!config["enabled"].toBool() || client)
        return;
    traffic = {};
    completed = callbackDrops = 0;
    trafficClock.start();
    attemptClock.start();
    lastReceived = 0;
    debug("connection attempt started; MQTT 3.1.1; clean session; keepalive=15 s; connect_timeout=10 s");
    for (const auto &name : {"ca", "certificate", "key"}) {
        const auto path = config[name].toString();
        if (!path.isEmpty() && (!QFileInfo(path).isFile() || !QFileInfo(path).isReadable())) {
            emit status("MQTT certificate/key file is not readable; check the configured paths");
            emit progress("failed");
            return;
        }
    }
    usedSessions.clear();
    epoch = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.clear();
    received = sent = 0;
    auto host = config["host"].toString();
    if (host.contains(':') && !host.startsWith('['))
        host = "[" + host + "]";
    const auto uri = (config["tls"].toBool(true) ? "ssl://" : "tcp://") + host + ":" +
                     QString::number(config["port"].toInt(8883));
    const auto clientId = ("what-" + device + "-" + epoch.left(8)).toUtf8();
    MQTTAsync_createOptions create = MQTTAsync_createOptions_initializer;
    create.sendWhileDisconnected = 0;
    create.maxBufferedMessages = 1;
    const int created = MQTTAsync_createWithOptions(&client, uri.toUtf8().constData(), clientId.constData(),
                                                    MQTTCLIENT_PERSISTENCE_NONE, nullptr, &create);
    if (created != MQTTASYNC_SUCCESS) {
        debug(QString("client setup failed; code=%1").arg(created));
        emit status("MQTT client setup failed");
        emit progress("failed");
        client = nullptr;
        return;
    }
    MQTTAsync_setCallbacks(client, this, disconnected, message, nullptr);
    username = config["username"].toString().toUtf8();
    password = config["password"].toString().toUtf8();
    ca = config["ca"].toString().toUtf8();
    certificate = config["certificate"].toString().toUtf8();
    key = config["key"].toString().toUtf8();
    if (config["tls"].toBool(true) && ca.isEmpty()) {
        // Paho/OpenSSL does not load the Windows certificate store itself.
        defaultCa = std::make_unique<QTemporaryFile>();
        if (!defaultCa->open()) {
            lost();
            emit status("MQTT could not prepare the Windows CA trust store");
            return;
        }
        for (const auto &cert : QSslConfiguration::systemCaCertificates())
            defaultCa->write(cert.toPem());
        defaultCa->flush();
        ca = defaultCa->fileName().toUtf8();
    }
    MQTTAsync_connectOptions options = MQTTAsync_connectOptions_initializer;
    options.context = this;
    options.onSuccess = success;
    options.onFailure = failure;
    options.keepAliveInterval = 15;
    options.cleansession = 1;
    options.connectTimeout = 10;
    options.MQTTVersion = MQTTVERSION_3_1_1;
    options.maxInflight = 8;
    options.username = username.isEmpty() ? nullptr : username.constData();
    options.password = password.isEmpty() ? nullptr : password.constData();
    MQTTAsync_SSLOptions ssl = MQTTAsync_SSLOptions_initializer;
    ssl.enableServerCertAuth = 1;
    ssl.verify = 1;
    ssl.sslVersion = MQTT_SSL_VERSION_TLS_1_2;
    ssl.enabledCipherSuites = "ECDHE+AESGCM:ECDHE+CHACHA20:DHE+AESGCM";
    ssl.trustStore = ca.isEmpty() ? nullptr : ca.constData();
    ssl.keyStore = certificate.isEmpty() ? nullptr : certificate.constData();
    ssl.privateKey = key.isEmpty() ? nullptr : key.constData();
    if (config["tls"].toBool(true))
        options.ssl = &ssl;
    emit progress("connecting");
    const int rc = MQTTAsync_connect(client, &options);
    if (rc != MQTTASYNC_SUCCESS) {
        debug(QString("connect could not start; code=%1").arg(rc));
        stop();
        retry.start();
        emit status("MQTT connect could not start");
        emit progress("retrying", retry.interval());
    } else
        emit status("Connecting to MQTT broker");
}

void MqttBridge::success(void *context, MQTTAsync_successData *) {
    auto *self = static_cast<MqttBridge *>(context);
    const auto gen = self->generation.load();
    QMetaObject::invokeMethod(
        self,
        [self, gen] {
            if (gen == self->generation)
                self->connected();
        },
        Qt::QueuedConnection);
}

void MqttBridge::callbackFailure(const char *operation, int code) {
    const auto gen = generation.load();
    QMetaObject::invokeMethod(
        this,
        [this, gen, operation, code] {
            if (gen != generation)
                return;
            // Do not copy Paho's raw error text: it may contain endpoint details.
            debug(QString("%1 failed; code=%2; elapsed_ms=%3; retry=5 s")
                      .arg(operation).arg(code).arg(attemptClock.elapsed()));
            if (qstrcmp(operation, "connect") == 0 && (code == 4 || code == 5))
                debug("broker refused authentication; check credentials and broker access rules");
            lost();
        },
        Qt::QueuedConnection);
}

void MqttBridge::failure(void *context, MQTTAsync_failureData *data) {
    static_cast<MqttBridge *>(context)->callbackFailure("connect", data ? data->code : MQTTASYNC_FAILURE);
}

void MqttBridge::disconnected(void *context, char *) {
    static_cast<MqttBridge *>(context)->callbackFailure("connection lost", MQTTASYNC_DISCONNECTED);
}

int MqttBridge::message(void *context, char *topic, int length, MQTTAsync_message *message) {
    auto *self = static_cast<MqttBridge *>(context);
    if (message->payloadlen >= 0 && message->payloadlen <= 65536 && self->queued.load() < 32) {
        ++self->queued;
        const auto gen = self->generation.load();
        const auto name = QString::fromUtf8(topic, length ? length : int(strlen(topic)));
        const auto data = QByteArray(static_cast<char *>(message->payload), message->payloadlen);
        const bool retained = message->retained;
        QMetaObject::invokeMethod(
            self,
            [self, name, data, retained, gen] {
                --self->queued;
                if (gen == self->generation)
                    self->receive(name, data, retained);
            },
            Qt::QueuedConnection);
    } else
        ++self->callbackDrops;
    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topic);
    return 1;
}

void MqttBridge::connected() {
    if (!client)
        return;
    online = true;
    debug(QString("broker connected; elapsed_ms=%1; subscribing to what/<device>/in")
              .arg(attemptClock.elapsed()));
    MQTTAsync_responseOptions options = MQTTAsync_responseOptions_initializer;
    options.context = this;
    options.onSuccess = subscribed;
    options.onFailure = subscribeFailed;
    const int rc = MQTTAsync_subscribe(client, (base + "/in").toUtf8().constData(), 0, &options);
    if (rc != MQTTASYNC_SUCCESS) {
        callbackFailure("subscribe", rc);
        return;
    }
    announce.start();
    advertise();
    emit status("MQTT broker connected; waiting for authenticated Home Assistant handover");
    emit progress("waiting");
}

void MqttBridge::subscribed(void *context, MQTTAsync_successData *data) {
    auto *self = static_cast<MqttBridge *>(context);
    const auto gen = self->generation.load();
    const int qos = data ? data->alt.qos : 128;
    QMetaObject::invokeMethod(self, [self, gen, qos] {
        if (gen != self->generation)
            return;
        if (qos == 128) {
            self->callbackFailure("subscribe", qos);
            return;
        }
        self->debug(QString("subscription confirmed; granted_qos=%1; signed announcements on "
                            "what/<device>/announce; awaiting WHAT integration handover").arg(qos));
    }, Qt::QueuedConnection);
}

void MqttBridge::subscribeFailed(void *context, MQTTAsync_failureData *data) {
    static_cast<MqttBridge *>(context)->callbackFailure("subscribe", data ? data->code : MQTTASYNC_FAILURE);
}

void MqttBridge::lost() {
    stop();
    retry.start();
    emit status("MQTT disconnected; direct transport remains available");
    emit progress("retrying", retry.interval());
}

qint64 MqttBridge::publish(QString topic, QByteArray payload) {
    if (!online || !client || payload.size() > 12 * 1024 * 1024 || outgoing.load() >= 8) {
        reject(!online || !client ? "write while disconnected" :
               payload.size() > 12 * 1024 * 1024 ? "oversized write" : "outbound queue full");
        return -1;
    }
    MQTTAsync_message message = MQTTAsync_message_initializer;
    message.payload = payload.data();
    message.payloadlen = payload.size();
    message.qos = 0;
    message.retained = 0;
    MQTTAsync_responseOptions options = MQTTAsync_responseOptions_initializer;
    options.context = this;
    options.onSuccess = published;
    options.onFailure = publishFailed;
    ++outgoing;
    const int result = MQTTAsync_sendMessage(client, topic.toUtf8().constData(), &message, &options);
    if (result != MQTTASYNC_SUCCESS) {
        --outgoing;
        reject("write submission failed code=" + QString::number(result));
    } else if (diagnosticsEnabled) {
        ++traffic.submitted;
        traffic.bytes += payload.size();
    }
    return result == MQTTASYNC_SUCCESS ? payload.size() : -1;
}

QByteArray MqttBridge::envelope(QString text, QString sid, qint64 seq, QString direction) const {
    const auto body = QString::fromLatin1(text.toUtf8().toBase64());
    return QJsonDocument(
               QJsonObject{{"epoch", epoch},
                           {"session", sid},
                           {"seq", double(seq)},
                           {"body", body},
                           {"mac", QString::fromLatin1(signature(secret, direction, epoch, sid, seq, body))}})
        .toJson(QJsonDocument::Compact);
}

void MqttBridge::advertise() {
    if (!online)
        return;
    if (!peer && usedSessions.size() >= 128) {
        stop();
        start();
        return;
    }
    const auto body =
        QString::fromUtf8(QJsonDocument(QJsonObject{{"preferred", preferred()}, {"discovery_probe_v1", true},
                                                    {"time", double(QDateTime::currentSecsSinceEpoch())},
                                                    {"scope", config["scope"].toString("cloud")}})
                              .toJson(QJsonDocument::Compact));
    publish(base + "/announce", envelope(body, "", 0, "pc"));
}

void MqttBridge::receive(QString topic, QByteArray payload, bool retained) {
    if (!online || retained || topic != base + "/in") {
        reject(!online ? "inbound while disconnected" : retained ? "retained inbound" : "unexpected topic");
        return;
    }
    const auto obj = QJsonDocument::fromJson(payload).object();
    const auto sid = obj["session"].toString(), body = obj["body"].toString();
    const auto number = obj["seq"].toDouble(-1);
    if (obj.size() != 5 || !obj["seq"].isDouble() || !std::isfinite(number) || number < 1 ||
        number > 9007199254740991.0 || std::floor(number) != number || !obj["session"].isString() ||
        !obj["body"].isString()) {
        reject("invalid envelope or sequence");
        return;
    }
    const auto sequence = qint64(number);
    if (obj["epoch"] != epoch || QUuid(sid).isNull() || sequence < 1 || sequence > 9007199254740991LL ||
        double(sequence) != obj["seq"].toDouble()) {
        reject("stale epoch or invalid session");
        return;
    }
    const auto mac = obj["mac"].toString().toLatin1(),
               expected = signature(secret, "ha", epoch, sid, sequence, body);
    if (mac.size() != expected.size() || CRYPTO_memcmp(mac.data(), expected.data(), mac.size())) {
        reject("signature mismatch; check pairing on both ends");
        return;
    }
    const auto decoded = QByteArray::fromBase64(body.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.isEmpty() || decoded.size() > 32768) {
        reject("invalid or oversized body");
        return;
    }
    auto inner = QJsonDocument::fromJson(decoded).object();
    if (inner["type"] == "discovery_probe") {
        // The random reply topic is the challenge nonce. This cannot create or
        // replace a telemetry session, and old replies cannot answer a new probe.
        if (sequence != 1 || sid == session ||
            (discoveryProbeClock.isValid() && discoveryProbeClock.elapsed() < 1000))
            return;
        discoveryProbeClock.start();
        const auto reply = QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "discovery_reply"}, {"preferred", preferred()}}).toJson(QJsonDocument::Compact));
        publish(base + "/out/" + sid, envelope(reply, sid, 1, "pc"));
        return;
    }
    if (!peer) {
        if (!preferred() || inner["type"] != "auth" || sequence != 1 || usedSessions.contains(sid) ||
            usedSessions.size() >= 128) {
            reject(!preferred() ? "MQTT is not the preferred transport" : "invalid or reused handover");
            return;
        }
        usedSessions.insert(sid);
        session = sid;
        received = 0;
        sent = 0;
        auto *created = new WirePeer(config["scope"].toString("cloud") == "cloud", this);
        peer = created;
        created->encrypted = config["tls"].toBool(true);
        created->pendingWrites = [this] { return outgoing.load(); };
        created->send = [this, sid](QString text) {
            return publish(base + "/out/" + sid, envelope(text, sid, ++sent, "pc"));
        };
        connect(created, &WirePeer::disconnected, this, [this, sid] {
            debug("Home Assistant MQTT session closed; awaiting handover or reconnect");
            peer = nullptr;
            if (!online)
                return;
            if (outgoing.load() > 0) {
                stop();
                start();
                return;
            } // Discard unsent data from the old sharing policy.
            publish(base + "/out/" + sid, envelope("{\"type\":\"transport_close\"}", sid, ++sent, "pc"));
            emit status("MQTT broker connected; waiting for authenticated Home Assistant handover");
            emit progress("waiting");
        });
        emit peerReady(created);
        debug("signed Home Assistant session opened; checking protocol authentication");
        emit status("MQTT authenticating Home Assistant session");
        emit progress("authenticating");
    }
    if (sid != session || sequence <= received) {
        reject("wrong session or replayed sequence");
        return;
    }
    received = sequence;
    lastReceived = QDateTime::currentMSecsSinceEpoch();
    if (diagnosticsEnabled) {
        ++traffic.accepted;
        if (inner["type"] == "ping")
            ++traffic.pings;
        if (inner["type"] == "command")
            ++traffic.requests;
    }
    if (inner["type"] == "ping")
        return;
    if (inner["type"] == "transport_close") {
        debug("Home Assistant requested transport handover");
        peer->close();
        return;
    }
    if (inner["type"] == "auth")
        inner["token"] = secret;
    emit peer->textMessageReceived(QString::fromUtf8(QJsonDocument(inner).toJson(QJsonDocument::Compact)));
}

void MqttBridge::releaseOutgoing() {
    // Runs on Paho's thread. A completion from a client that stop() already
    // replaced must not take the new counter below zero.
    int pending = outgoing.load();
    while (pending > 0 && !outgoing.compare_exchange_weak(pending, pending - 1)) {
    }
}

void MqttBridge::published(void *context, MQTTAsync_successData *) {
    auto *self = static_cast<MqttBridge *>(context);
    self->releaseOutgoing();
    ++self->completed;
    const auto gen = self->generation.load();
    QMetaObject::invokeMethod(
        self,
        [self, gen] {
            if (gen == self->generation && self->peer)
                emit self->peer->bytesWritten(0);
        },
        Qt::QueuedConnection);
}

void MqttBridge::publishFailed(void *context, MQTTAsync_failureData *data) {
    auto *self = static_cast<MqttBridge *>(context);
    self->releaseOutgoing();
    self->callbackFailure("publish", data ? data->code : MQTTASYNC_FAILURE);
}
