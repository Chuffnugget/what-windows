#pragma once
#include <QObject>
#include <QJsonObject>
#include <QTimer>
#include <QPointer>
#include <QSet>
#include <atomic>
#include <QTemporaryFile>
#include <QElapsedTimer>
#include <QMap>
#include <memory>
#include "WirePeer.h"
#include <MQTTAsync.h>

class MqttBridge : public QObject {
    Q_OBJECT
public:
    explicit MqttBridge(QObject *parent = nullptr);
    ~MqttBridge();
    void configure(QJsonObject options, QString device, QString token);
    void stop();
    void start();

    bool preferred() const {
        return config["preferred"].toBool();
    }

    void setPreferred(bool value);
    void setDiagnostics(bool enabled);
    void recordFrame(bool delta, int readings);
    void recordFeedback(qint64 elapsedMs);
    void recordResult(bool ok, bool submitted);
signals:
    void peerReady(WirePeer *peer);
    void status(QString message);
    void progress(QString phase, int retryMs = 0);
    void diagnostic(QString message);

private:
    friend class ControllerTests;
    MQTTAsync client = nullptr;
    QJsonObject config;
    QString device, secret, epoch, base;
    bool online = false;
    std::atomic<int> generation{0}, queued{0}, outgoing{0};
    QSet<QString> usedSessions;
    QTimer announce, retry;
    QPointer<WirePeer> peer;
    QString session;
    qint64 received = 0, sent = 0, lastReceived = 0;
    QByteArray username, password, ca, certificate, key;
    std::unique_ptr<QTemporaryFile> defaultCa;
    bool diagnosticsEnabled = false;
    struct Traffic {
        quint64 submitted = 0, bytes = 0, accepted = 0, pings = 0, requests = 0;
        quint64 full = 0, delta = 0, feedback = 0, resultsOk = 0, resultsFailed = 0;
        qint64 lastRoundTripMs = -1, maxRoundTripMs = -1;
        int lastReadings = 0;
        QMap<QString, quint64> rejected;
    } traffic;
    std::atomic<quint64> completed{0}, callbackDrops{0};
    QElapsedTimer trafficClock, attemptClock, discoveryProbeClock;
    void debug(const QString &message);
    void reject(const QString &reason);
    void reportTraffic();
    void callbackFailure(const char *operation, int code);
    void releaseOutgoing();
    void connected();
    void lost();
    void receive(QString topic, QByteArray payload, bool retained);
    void advertise();
    qint64 publish(QString topic, QByteArray payload);
    QByteArray envelope(QString body, QString session, qint64 sequence, QString direction) const;
    static int message(void *, char *, int, MQTTAsync_message *);
    static void success(void *, MQTTAsync_successData *);
    static void failure(void *, MQTTAsync_failureData *);
    static void disconnected(void *, char *);
    static void subscribed(void *, MQTTAsync_successData *);
    static void subscribeFailed(void *, MQTTAsync_failureData *);
    static void published(void *, MQTTAsync_successData *);
    static void publishFailed(void *, MQTTAsync_failureData *);
};
