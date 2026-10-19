#include "modules/icue/IcueAutomation.h"
#include "modules/icue/LightingLink.h"
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUuid>
#include <QCryptographicHash>
#include <QtEndian>
#include <QThread>
#include <QSet>
#include <QColor>
#include <cmath>
#include <stdexcept>

namespace {
constexpr qsizetype MaxFrame = 4 * 1024 * 1024;

void fail(const QString &message) {
    throw std::runtime_error(message.toStdString());
}

class Session {
    QLocalSocket socket;
    QByteArray pending;
    QElapsedTimer budget;

    int remaining() const {
        const int ms = 6000 - int(budget.elapsed());
        if (ms <= 0)
            fail("iCUE automation timed out; check its SDK approval prompt. The command was not retried.");
        return qMin(1500, ms);
    }

    QByteArray read(qsizetype size) {
        while (pending.size() < size) {
            pending += socket.readAll();
            if (pending.size() >= size)
                break;
            if (!socket.waitForReadyRead(remaining()))
                fail(
                    "iCUE automation reply timed out or disconnected. Check 'SDK: New Client Access Confirmation' in iCUE; command not retried.");
        }
        auto bytes = pending.left(size);
        pending.remove(0, size);
        return bytes;
    }

public:
    QString version;

    explicit Session(const QString &endpoint) {
        budget.start();
        socket.setReadBufferSize(MaxFrame + 4);
        socket.connectToServer(endpoint);
        if (!socket.waitForConnected(remaining()))
            fail("Cannot connect to iCUE Automation API: " + socket.errorString() +
                 ". Start iCUE and enable Settings > SDK.");
        const auto data = call("handshake", {{"clientId", "WHAT"}, {"version", 1}});
        version = data["serverVersion"].toString();
        if (version.isEmpty())
            fail("Unrecognised iCUE automation handshake reply");
    }

    QJsonObject call(const QString &method, const QJsonObject &params = {}, bool allowNoEffect = false) {
        const auto context = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto payload = QJsonDocument(QJsonObject{{"context", context},
                                                 {"method", "com.corsair.icue." + method},
                                                 {"params", params}})
                           .toJson(QJsonDocument::Compact);
        QByteArray frame(4, 0);
        qToBigEndian<quint32>(quint32(payload.size()), frame.data());
        frame += payload;
        if (socket.write(frame) != frame.size())
            fail("Could not send iCUE automation request");
        while (socket.bytesToWrite() > 0)
            if (!socket.waitForBytesWritten(remaining()))
                fail("iCUE request timed out; outcome may be unknown. Not retried.");
        for (int messages = 0; messages < 128; ++messages) {
            const auto header = read(4);
            const auto length = qFromBigEndian<quint32>(header.constData());
            if (!length || length > MaxFrame)
                fail("Invalid iCUE automation frame length");
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(read(length), &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject())
                fail("Invalid iCUE automation JSON response");
            const auto reply = doc.object();
            if (reply["context"] != context)
                continue;
            if (allowNoEffect && method == "lighting-link-effects.get-active" &&
                reply["error"].toObject()["code"].toInt() == 4 &&
                reply["error"].toObject()["message"] == "ResourceNotFound")
                return {{"_noActiveEffect", true}};
            if (reply.contains("error") && !reply["error"].isNull())
                fail("iCUE rejected " + method + ": " +
                     QString::fromUtf8(
                         QJsonDocument(reply["error"].toObject()).toJson(QJsonDocument::Compact)) +
                     ". Check SDK access approval in iCUE.");
            if (!reply["data"].isObject() && !reply["data"].isNull())
                fail("Invalid iCUE automation response data");
            return reply["data"].toObject();
        }
        fail("Too many unrelated iCUE automation messages");
        return {};
    }
};

QJsonArray items(const QJsonObject &data) {
    if (!data["items"].isArray() || data["items"].toArray().size() > 256)
        fail("Invalid or oversized iCUE automation inventory");
    return data["items"].toArray();
}

QJsonArray options(const QJsonArray &values) {
    QJsonArray result;
    QSet<QString> seen;
    for (auto value : values) {
        auto row = value.toObject();
        const auto id = row["id"].toString(), name = row["name"].toString();
        if (id.isEmpty() || id.size() > 256 || name.isEmpty() || name.size() > 256 || seen.contains(id))
            fail("Invalid iCUE option identity");
        seen.insert(id);
        result.append(QJsonObject{{"id", id}, {"name", name}});
    }
    return result;
}
} // namespace

QJsonObject IcueAutomation::poll(bool enabled, bool force, bool readTelemetry) {
    if (!enabled) {
        cache = {};
        routes.clear();
        inventoryAge.invalidate();
        lightingInventoryAge.invalidate();
        lightingEffects = {};
        return {{"enabled", false}, {"connected", false}, {"status", "Disabled"}, {"controls", QJsonArray{}}};
    }
    try {
        Session session(endpoint);
        if (force || !inventoryAge.isValid() || inventoryAge.elapsed() > 15000) {
            QJsonArray controls;
            QMap<QString, QJsonObject> nextRoutes;
            auto add = [&](QString id, QString name, QString device, QJsonArray choices, QString method,
                           QJsonObject params, QString parameter, bool confirmed) {
                if (choices.isEmpty())
                    return;
                controls.append(QJsonObject{{"id", id},
                                            {"name", name},
                                            {"device", device},
                                            {"options", choices},
                                            {"current", QJsonValue::Null},
                                            {"confirmed", confirmed}});
                nextRoutes[id] = {{"method", method}, {"params", params}, {"parameter", parameter}};
            };
            add("profile", "iCUE profile", "iCUE", options(items(session.call("profiles.list"))),
                "profiles.activate", {}, "profileId", true);
            QStringList warnings;
            for (const auto &module : {QString("cooling"), QString("eq"), QString("dpi")}) {
                try {
                    const auto devices = items(session.call(module + ".devices-list"));
                    if (devices.size() > 64)
                        fail("Too many iCUE automation devices");
                    for (auto value : devices) {
                        auto device = value.toObject();
                        const auto deviceId = device["id"].toString(), name = device["name"].toString();
                        if (deviceId.isEmpty() || deviceId.size() > 256 || name.isEmpty() ||
                            name.size() > 256)
                            fail("Invalid iCUE device identity");
                        const auto choices =
                            options(items(session.call(module + ".presets-list", {{"deviceId", deviceId}})));
                        const auto id = module + "_" +
                                        QString::fromLatin1(QCryptographicHash::hash(
                                                                deviceId.toUtf8(), QCryptographicHash::Sha256)
                                                                .toHex()
                                                                .left(24));
                        add(id,
                            module == "cooling" ? "Apply cooling preset"
                            : module == "eq"    ? "Apply EQ preset"
                                                : "Apply DPI preset",
                            name, choices, module + ".activate-preset", {{"deviceId", deviceId}},
                            module == "eq" ? "equalizerId" : "presetId", false);
                    }
                } catch (const std::exception &e) {
                    warnings.append(module + ": " + QString::fromUtf8(e.what()));
                }
            }
            cache = {{"enabled", true},
                     {"connected", true},
                     {"status", warnings.isEmpty() ? "Connected" : "Connected; " + warnings.join("; ")},
                     {"version", session.version},
                     {"controls", controls}};
            routes = nextRoutes;
            inventoryAge.start();
        }
        const auto active = session.call("profiles.get-active");
        const auto activeId = active["id"].toString();
        if (activeId.isEmpty())
            fail("iCUE returned no active profile identity");
        auto controls = cache["controls"].toArray();
        bool known = false;
        for (qsizetype i = 0; i < controls.size(); ++i) {
            auto row = controls[i].toObject();
            if (row["id"] != "profile")
                continue;
            for (auto option : row["options"].toArray())
                if (option.toObject()["id"] == activeId)
                    known = true;
            row["current"] = known ? QJsonValue(activeId) : QJsonValue(QJsonValue::Null);
            controls[i] = row;
        }
        if (!known)
            inventoryAge.invalidate();
        cache["controls"] = controls;
        cache["connected"] = true;
        if (!readTelemetry)
            return cache;
        // Optional telemetry must not turn a working profile connection into an error.
        QString telemetryStatus;
        try {
            Session telemetry(endpoint);
            if (!lightingInventoryAge.isValid() || lightingInventoryAge.elapsed() > 15000) {
                lightingEffects = LightingLink::effects(telemetry.call("lighting-link-effects.list"));
                lightingInventoryAge.start();
            }
            const auto lighting = telemetry.call("lighting-link-effects.get-active", {}, true);
            cache["lighting"] = LightingLink::state(lighting, lightingEffects);
            cache["readings"] = LightingLink::readings(cache["lighting"].toObject());
            telemetryStatus = lighting["_noActiveEffect"].toBool()
                                  ? "No active Automation Lighting Link effect"
                              : cache["readings"].toArray()[0].toObject()["available"].toBool()
                                  ? "Lighting Link values read from iCUE"
                                  : "Unrecognised Lighting Link response; current values unavailable";
        } catch (const std::exception &e) {
            telemetryStatus = QString::fromUtf8(e.what()).left(1200);
            lightingInventoryAge.invalidate();
            cache["lighting"] = LightingLink::state({}, lightingEffects, telemetryStatus);
            cache["readings"] = LightingLink::readings(cache["lighting"].toObject());
        }
        cache["telemetry_status"] =
            telemetryStatus +
            ". Current DPI stage/sensitivity are not exposed by the verified API; stages-list describes available stages only.";
        return cache;
    } catch (const std::exception &e) {
        inventoryAge.invalidate();
        return {{"enabled", true},
                {"connected", false},
                {"status", QString::fromUtf8(e.what()).left(2000)},
                {"controls", QJsonArray{}}};
    }
}

QString IcueAutomation::validate(const QJsonObject &args, const QJsonObject &state) {
    if (args.size() != 2 || !args["control"].isString() || !args["option"].isString())
        return "Expected an iCUE control and option ID";
    if (!state["connected"].toBool())
        return "iCUE Automation API is not connected";
    for (auto value : state["controls"].toArray()) {
        const auto row = value.toObject();
        if (row["id"] != args["control"])
            continue;
        for (auto option : row["options"].toArray())
            if (option.toObject()["id"] == args["option"])
                return {};
        return "iCUE option is no longer available";
    }
    return "iCUE control is no longer available";
}

QString IcueAutomation::command(const QJsonObject &args, const std::function<void()> &clearRgb,
                                const std::function<bool()> &cancelled) {
    if (cancelled())
        return "iCUE command cancelled";
    const auto fresh = poll(true, true, false);
    const auto invalid = validate(args, fresh);
    if (!invalid.isEmpty())
        return invalid;
    if (cancelled())
        return "iCUE command cancelled";
    try {
        Session session(endpoint);
        const auto route = routes.value(args["control"].toString());
        auto params = route["params"].toObject();
        params[route["parameter"].toString()] = args["option"];
        if (cancelled())
            return "iCUE command cancelled";
        if (args["control"] == "profile")
            clearRgb();
        if (cancelled())
            return "iCUE command cancelled";
        session.call(route["method"].toString(), params);
        inventoryAge.invalidate();
        if (args["control"] == "profile") {
            for (int i = 0; i < 5; ++i) {
                if (session.call("profiles.get-active")["id"] == args["option"])
                    return {};
                QThread::msleep(100);
            }
            return "iCUE accepted the request but the active profile did not match; not retried";
        }
        return {};
    } catch (const std::exception &e) {
        inventoryAge.invalidate();
        return QString::fromUtf8(e.what());
    }
}

QJsonArray IcueAutomation::lightingReadings(const QJsonObject &reply, const QString &error) {
    return LightingLink::readings(LightingLink::state(reply, {}, error));
}

QString IcueAutomation::validateLighting(const QJsonObject &args, const QJsonObject &state) {
    if (!state["connected"].toBool())
        return "iCUE Automation API is not connected";
    return LightingLink::validate(args, state["lighting"].toObject());
}

QString IcueAutomation::lightingCommand(const QJsonObject &args, const std::function<void()> &clearRgb,
                                        const std::function<bool()> &cancelled) {
    if (cancelled())
        return "iCUE command cancelled";
    try {
        Session session(endpoint);
        const auto effects = LightingLink::effects(session.call("lighting-link-effects.list"));
        const auto current =
            LightingLink::state(session.call("lighting-link-effects.get-active", {}, true), effects);
        auto error = LightingLink::validate(args, current);
        if (!error.isEmpty())
            return error;
        if (cancelled())
            return "iCUE command cancelled";
        const bool stop = args["stop"].toBool();
        const auto payload = stop ? QJsonObject{} : LightingLink::activation(args, current);
        clearRgb();
        if (cancelled())
            return "iCUE command cancelled";
        session.call(stop ? "lighting-link-effects.deactivate" : "lighting-link-effects.activate", payload);
        for (int attempt = 0; attempt < 5; ++attempt) {
            const auto actual =
                LightingLink::state(session.call("lighting-link-effects.get-active", {}, true), effects);
            if (stop ? actual["available"].toBool() && actual["effect"].isNull()
                     : LightingLink::matches(payload, actual))
                return {};
            QThread::msleep(100);
        }
        return "iCUE accepted the request but Lighting Link readback did not match; not retried";
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
}
