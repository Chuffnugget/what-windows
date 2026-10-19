#pragma once
#include <QObject>
#include <QWebSocket>
#include <functional>

// Shared framed-message interface; entity protocol and command permissions live above it.
class WirePeer : public QObject {
    Q_OBJECT
public:
    QWebSocket *socket = nullptr;
    bool cloud = false;
    bool mqtt = false;
    bool encrypted = true;
    bool closed = false;
    std::function<qint64(QString)> send;
    std::function<qint64()> pendingWrites;

    explicit WirePeer(QWebSocket *ws, bool remote, QObject *parent = nullptr)
        : QObject(parent), socket(ws), cloud(remote) {
        socket->setParent(this);
        connect(ws, &QWebSocket::textMessageReceived, this, &WirePeer::textMessageReceived);
        connect(ws, &QWebSocket::binaryMessageReceived, this, &WirePeer::binaryMessageReceived);
        connect(ws, &QWebSocket::bytesWritten, this, &WirePeer::bytesWritten);
        connect(ws, &QWebSocket::disconnected, this, [this] {
            if (!closed) {
                closed = true;
                emit disconnected();
            }
        });
    }

    explicit WirePeer(bool remote, QObject *parent = nullptr) : QObject(parent), cloud(remote), mqtt(true) {}

    void setMaxAllowedIncomingFrameSize(quint64 n) {
        if (socket)
            socket->setMaxAllowedIncomingFrameSize(n);
    }

    void setMaxAllowedIncomingMessageSize(quint64 n) {
        if (socket)
            socket->setMaxAllowedIncomingMessageSize(n);
    }

    qint64 bytesToWrite() const {
        return socket ? socket->bytesToWrite() : (pendingWrites ? qMax<qint64>(0, pendingWrites()) : 0);
    }

    QSslConfiguration sslConfiguration() const {
        return socket ? socket->sslConfiguration() : QSslConfiguration();
    }

    qint64 sendTextMessage(const QString &text) {
        if (closed)
            return -1;
        if (socket)
            return socket->sendTextMessage(text);
        auto n = send ? send(text) : -1;
        // MQTT completion is signalled by the broker client after its write drains.
        return n;
    }

    void close(QWebSocketProtocol::CloseCode code = QWebSocketProtocol::CloseCodeNormal,
               const QString &reason = {}) {
        if (socket) {
            socket->close(code, reason);
            return;
        }
        if (!closed) {
            closed = true;
            emit disconnected();
            deleteLater();
        }
    }

    void abort() {
        if (socket)
            socket->abort();
        else
            close();
    }

signals:
    void textMessageReceived(QString text);
    void binaryMessageReceived(QByteArray data);
    void bytesWritten(qint64 bytes);
    void disconnected();
};
