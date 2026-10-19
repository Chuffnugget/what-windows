#pragma once
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QThread>
#include <QUuid>
#include <QTimer>
#include <QtEndian>
#include <atomic>

class AutomationFixture {
    QThread thread;
    QObject *context = new QObject;
    QLocalServer *server = nullptr;
    QString active = "a";
    QJsonObject lightingActive;

public:
    QString endpoint = "what-automation-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::atomic_bool removeB{false}, deny{false}, oversized{false}, ignoreLightingWrite{false};
    std::atomic_int commands{0};
    std::atomic_int lightingMode{0};
    bool listening = false;

    AutomationFixture() {
        context->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, context, &QObject::deleteLater);
        thread.start();
        QMetaObject::invokeMethod(
            context,
            [this] {
                server = new QLocalServer(context);
                listening = server->listen(endpoint);
                QObject::connect(server, &QLocalServer::newConnection, server, [this] {
                    while (server->hasPendingConnections()) {
                        auto *socket = server->nextPendingConnection();
                        auto pending = std::make_shared<QByteArray>();
                        QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                        QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket, pending] {
                            *pending += socket->readAll();
                            while (pending->size() >= 4) {
                                const auto n = qFromBigEndian<quint32>(pending->constData());
                                if (n > 4 * 1024 * 1024) {
                                    socket->abort();
                                    return;
                                }
                                if (pending->size() < 4 + n)
                                    return;
                                const auto request = QJsonDocument::fromJson(pending->mid(4, n)).object();
                                pending->remove(0, 4 + n);
                                const auto method =
                                    request["method"].toString().section("com.corsair.icue.", 1);
                                QJsonObject reply{{"context", request["context"]},
                                                  {"method", request["method"]}},
                                    data;
                                const auto args = request["params"].toObject();
                                if (method == "handshake")
                                    data = {{"serverVersion", "fixture"}};
                                else if (method == "profiles.list") {
                                    QJsonArray choices{QJsonObject{{"id", "a"}, {"name", "Default"}}};
                                    if (!removeB)
                                        choices.append(QJsonObject{{"id", "b"}, {"name", "Gaming"}});
                                    data = {{"items", choices}};
                                } else if (method == "profiles.get-active")
                                    data = {{"id", active}, {"name", active}};
                                else if (method == "lighting-link-effects.list") {
                                    data = {
                                        {"items",
                                         QJsonArray{
                                             QJsonObject{{"type", "StaticColor"},
                                                         {"parameters", QJsonArray{"Color1"}}},
                                             QJsonObject{{"type", "RainbowWave"},
                                                         {"parameters", QJsonArray{"Speed", "Direction"}}},
                                             QJsonObject{{"type", "ColorWave"},
                                                         {"parameters",
                                                          QJsonArray{"Speed", "Direction", "RandomColor",
                                                                     "Color1", "Color2"}}},
                                             QJsonObject{{"type", "SpiralRainbow"},
                                                         {"parameters", QJsonArray{"Speed", "Rotation"}}}}}};
                                } else if (method == "lighting-link-effects.activate" ||
                                           method == "lighting-link-effects.deactivate") {
                                    if (deny)
                                        reply["error"] = QJsonObject{{"code", 2}, {"message", "NotAllowed"}};
                                    else {
                                        ++commands;
                                        if (!ignoreLightingWrite) {
                                            if (method.endsWith(".deactivate"))
                                                lightingMode = 0;
                                            else {
                                                lightingActive = args["effect"].toObject();
                                                lightingMode = 3;
                                            }
                                        }
                                    }
                                } else if (method == "lighting-link-effects.get-active") {
                                    if (lightingMode == 0)
                                        reply["error"] =
                                            QJsonObject{{"code", 4}, {"message", "ResourceNotFound"}};
                                    else if (lightingMode == 2)
                                        reply["error"] = QJsonObject{{"code", 2}, {"message", "NotAllowed"}};
                                    else if (lightingMode == 3)
                                        data = {{"effect", lightingActive}};
                                    else
                                        data = {
                                            {"effect",
                                             QJsonObject{
                                                 {"type", "ColorWave"},
                                                 {"parameters",
                                                  QJsonArray{QJsonObject{{"type", "Speed"}, {"value", 2}},
                                                             QJsonObject{{"type", "Direction"}, {"value", 1}},
                                                             QJsonObject{
                                                                 {"type", "Color1"},
                                                                 {"value", double(qint32(0xff00aaffu))}}}}}}};
                                } else if (method.endsWith("devices-list"))
                                    data = {{"items",
                                             QJsonArray{QJsonObject{{"id", "dev"}, {"name", "Device"}}}}};
                                else if (method.endsWith("presets-list"))
                                    data = {{"items", QJsonArray{QJsonObject{{"id", "preset"},
                                                                             {"name", "Balanced"}}}}};
                                else if (method == "profiles.activate" ||
                                         method.endsWith("activate-preset")) {
                                    if (method.endsWith("activate-preset") &&
                                        (args["deviceId"] != "dev" ||
                                         args[method.startsWith("eq.") ? "equalizerId" : "presetId"] !=
                                             "preset"))
                                        reply["error"] = QJsonObject{
                                            {"code", 3}, {"message", "Invalid preset parameters"}};
                                    else if (deny)
                                        reply["error"] = QJsonObject{{"code", 2}, {"message", "NotAllowed"}};
                                    else {
                                        ++commands;
                                        if (method == "profiles.activate")
                                            active = args["profileId"].toString();
                                    }
                                } else
                                    reply["error"] = QJsonObject{{"code", 1}, {"message", "Unknown method"}};
                                reply["data"] = data;
                                const auto body = QJsonDocument(reply).toJson(QJsonDocument::Compact);
                                QByteArray frame(4, 0);
                                qToBigEndian<quint32>(oversized ? 5 * 1024 * 1024 : body.size(),
                                                      frame.data());
                                frame += body;
                                socket->write(frame.left(2));
                                QTimer::singleShot(2, socket,
                                                   [socket, frame] { socket->write(frame.mid(2)); });
                            }
                        });
                    }
                });
            },
            Qt::BlockingQueuedConnection);
    }

    ~AutomationFixture() {
        QMetaObject::invokeMethod(context, [this] { delete server; }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    }
};
