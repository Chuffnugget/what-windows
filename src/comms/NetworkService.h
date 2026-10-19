#pragma once
#include <QObject>
#include "PublishingPolicy.h"
#include "WirePeer.h"
#include "MqttBridge.h"
#include <QThread>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QMutex>
#include <QQueue>
#include <QElapsedTimer>
#include <memory>
#include <atomic>

struct PendingSnapshot {
    QJsonObject value;
    bool event = false;
    bool reset = false;
    quint64 generation = 0;
};

struct SnapshotMailbox {
    QMutex mutex;
    QQueue<PendingSnapshot> values;
    bool queued = false;
    quint64 ticket = 0;
};

struct NetworkControl {
    QMutex mutex;
    QHash<QString, std::shared_ptr<std::atomic_bool>> leases;
    QHash<QString, quint64> leaseIdentities;
    QHash<QString, std::shared_ptr<std::atomic_bool>> installationLeases;
    std::atomic_uint64_t identityGeneration{0};
    std::atomic_uint64_t generation{0};
};

class NetworkWorker : public QObject {
    Q_OBJECT
public:
    explicit NetworkWorker(QObject *parent = nullptr) : QObject(parent) {}
    void setInstallations(QJsonArray definitions,
                         QHash<QString, std::shared_ptr<std::atomic_bool>> guards);
    std::shared_ptr<NetworkControl> control = std::make_shared<NetworkControl>();
    quint64 appliedGeneration = 0;
    quint64 appliedIdentity = 0;
    void initialize();
    void setRemote(QString url);
    void setMqtt(QJsonObject options);
    void setDiagnostics(bool enabled);
    void connectRemote();
    bool listen(QHostAddress address, quint16 port);
    void stop();
    void configure(QString device, QString token, QSslConfiguration ssl);
    void setSnapshot(QJsonObject value);
    void replaceSnapshot(QJsonObject value);
    void setInterval(int ms);
    void setEntityLookup(QString reading, QStringList keys);
    void eventSnapshot(QJsonObject value);
    void replyCommand(const QString &session, const QString &id, bool ok, const QString &message,
                      QJsonObject result = {});
    std::shared_ptr<std::atomic_bool> commandLease(const QString &session);
    QString error;
signals:
    void installationState(QString id, QVariantMap state);
    void installationTransportFailed(QString id, QString requested, QString retained, QString message);
    void listenFinished(quint64 request, bool ok, quint16 port, QString error);
    void clientCount(int count, quint64 total);
    void remoteState(QString state);
    void mqttState(QString state);
    void connectionProgress(QString transport, QString phase, int retryMs = 0);
    void transportState(QString transport, QString tls);
    void transmitted();
    void haCommunication(qint64 timestamp);
    void feedbackSupport(bool supported);
    void entityLookupResult(QString reading, QStringList entities, QString message);
    void installationEntityLookup(QString installation, QString reading, QStringList entities, QString message);
    void diagnostic(QString text);
    void commandRequested(QString session, QJsonObject command);

private:
    friend class ControllerTests;
    friend class NetworkService;
    bool managedInstallations = false;
    QString installationId;
    QJsonObject installationOptions;
    QString activeInstallationTransport;
    QString transportRevision;
    QTimer *transportDeadline = nullptr;
    QSet<WirePeer *> transportPeers;
    void applyInstallationOptions(QJsonObject options);
    void activateInstallationTransport(WirePeer *peer, const QJsonObject &request);
    bool isActiveInstallationPeer(WirePeer *peer) const;
    QJsonObject installationConnectionConfig(WirePeer *peer) const;
    std::shared_ptr<std::atomic_bool> installationAlive;
    QHash<QString, NetworkWorker *> installations;
    QHash<QString, QVariantMap> installationStates;
    QVariantMap aggregateState;
    QJsonArray installationDiagnostics;
    QList<QWebSocket *> pendingPairings;
    void startInstallation();
    void acceptInstallation(QWebSocket *socket);
    void updateInstallationState(QVariantMap changes);
    void aggregateInstallationState();
    QJsonObject installationSnapshot(QJsonObject value) const;
    void accept();
    void attachSocket(QWebSocket *s);
    void attachPeer(WirePeer *s);
    void updateTransportState();
    void probeHomeAssistant();
    void sendPendingProbes();
    struct FeedbackPeer {
        QString nonce;
        QString reading;
        QElapsedTimer sent;
        bool due = false;
    };
    QHash<WirePeer *, FeedbackPeer> feedbackPeers;
    QTimer *feedbackTimer = nullptr, *lookupTimer = nullptr;
    QString lookupReading;
    QStringList lookupKeys;
    MqttBridge *mqtt = nullptr;
    QJsonObject mqttOptions;
    QWebSocket *outbound = nullptr;
    QTimer *remoteRetry = nullptr;
    QString remoteUrl;
    int remoteDelay = 1000;
    bool stopped = true;
    bool diagnosticsEnabled = false;
    void publish();
    QWebSocketServer *server = nullptr;
    QTimer *timer = nullptr, *flush = nullptr;
    QHash<WirePeer *, PublishingPolicy> publishingPolicies;
    QHash<WirePeer *, qint64> lastFrames;
    QHash<WirePeer *, QJsonObject> lastMetadata;
    QSet<WirePeer *> deltaClients;
    QHash<WirePeer *, QJsonObject> baselines;
    QHash<WirePeer *, int> sequences;
    QHash<WirePeer *, QQueue<QJsonObject>> pendingEvents;
    QElapsedTimer statsClock, publishClock;
    quint64 frameCount = 0, byteCount = 0, deltaCount = 0;
    QString device, token;
    QJsonObject latest;
    QSet<WirePeer *> clients, authenticated;
    QHash<WirePeer *, QElapsedTimer> blockedSince;
    quint64 connections = 0;
    QElapsedTimer receivedAt;

    struct CommandClient {
        QString session;
        std::shared_ptr<std::atomic_bool> alive;
        // Recent command IDs, for rejecting duplicates. IDs older than two minutes
        // are forgotten; commands expire after one minute anyway.
        QSet<QString> seen;
        QQueue<QPair<qint64, QString>> seenOrder;
        QElapsedTimer rate, clock;
        bool relativeDeadlines = false;
        int count = 0;
    };

    QHash<WirePeer *, CommandClient> commandClients;
    QHash<WirePeer *, QJsonObject> icueBaselines, automationBaselines;
};

class NetworkService : public QObject {
    Q_OBJECT
public:
    explicit NetworkService(QObject *parent = nullptr);
    void listenAsync(QHostAddress address, quint16 port);
    ~NetworkService() override;
    bool listen(QHostAddress address, quint16 port);
    void close();
    void setRemote(QString url);
    void setMqtt(QJsonObject options);
    void setDiagnostics(bool enabled);
    void setInstallations(QJsonArray definitions);
    QString installationForSession(const QString &session) const;

    bool isListening() const {
        return running;
    }

    bool isStarting() const {
        return starting;
    }

    quint64 policyGeneration() const {
        return worker->control->generation.load();
    }

    QString errorString() const {
        return error;
    }

    void configure(QString device, QString token, QSslConfiguration ssl);
    void setSnapshot(QJsonObject value);
    void replaceSnapshot(QJsonObject value);
    void setInterval(int ms);
    void setEntityLookup(QString reading, QStringList keys);
    void setEventSnapshot(QJsonObject value);
    void replyCommand(const QString &session, const QString &id, bool ok, const QString &message,
                      QJsonObject result = {});
    std::shared_ptr<std::atomic_bool> commandLease(const QString &session);
signals:
    void installationState(QString id, QVariantMap state);
    void installationTransportFailed(QString id, QString requested, QString retained, QString message);
    void listenFinished(quint64 request, bool ok, quint16 port, QString error);
    void clientCount(int count, quint64 total);
    void remoteState(QString state);
    void mqttState(QString state);
    void connectionProgress(QString transport, QString phase, int retryMs = 0);
    void transportState(QString transport, QString tls);
    void transmitted();
    void haCommunication(qint64 timestamp);
    void feedbackSupport(bool supported);
    void entityLookupResult(QString reading, QStringList entities, QString message);
    void installationEntityLookup(QString installation, QString reading, QStringList entities, QString message);
    void diagnostic(QString text);
    void commandRequested(QString session, QJsonObject command);

private:
    friend class ControllerTests;
    void enqueue(QJsonObject value, bool event);
    QThread *thread = new QThread;
    quint64 lifecycle = 0;
    void revokeLeases();
    NetworkWorker *worker;
    std::shared_ptr<SnapshotMailbox> mailbox = std::make_shared<SnapshotMailbox>();
    bool running = false, starting = false;
    QHash<QString, QJsonObject> installationDefinitions;
    QHash<QString, std::shared_ptr<std::atomic_bool>> installationGuards;
    QString error;
};
