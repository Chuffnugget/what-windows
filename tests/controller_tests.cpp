#include <QQmlIncubationController>
#include "app/Architecture.h"
#include "app/SettingsStore.h"
#include "app/AppRuntime.h"
#include "modules/windows/media/MediaSessions.h"
#include "modules/beacn/InstalledFiles.h"
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <shldisp.h>
#include <propsys.h>
#include <propkey.h>
#include "app/WindowFrame.h"
#include "app/LocalDiagnostics.h"
#include "comms/PublishingPolicy.h"
#include "modules/windows/security/SecurityStatus.h"
#include "modules/wallpaper/WallpaperEngine.h"
#include "app/Controller.h"
#include "app/StartupRegistration.h"
#include "app/UiMotion.h"
#include <QtConcurrent>
#include <QQuickItemGrabResult>
#include "modules/beacn/BeacnBackend.h"
#include "modules/windows/specs/SpecsDisplays.h"
#include "modules/windows/specs/SpecsIssues.h"
#include "modules/windows/specs/SpecsHardware.h"
#include "app/DiagnosticReport.h"
#include "app/StartupNotice.h"
#include "app/TrayUi.h"
#include "AutomationFixture.h"
#include "modules/windows/displays/Displays.h"
#include "modules/icue/LightingLink.h"
#include "updater/UpdateManager.h"
#include "modules/windows/commands/WindowsCommands.h"
#include "modules/windows/commands/ShellCommand.h"
#include "modules/custom/CustomSources.h"
#include <QQmlApplicationEngine>
#include <QQmlExpression>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQuickItem>
#include <QScreen>
#include <QFontMetricsF>
#include <QUrlQuery>
#include <QtTest>
#include <QTemporaryDir>
#include "app/ConfigurationFile.h"
#include <QApplication>
#include <QDesktopServices>
#include <QClipboard>
#include <QColorDialog>
#include <QPushButton>
#include <QToolButton>
#include <QJsonDocument>
#include <QTcpServer>
#include <QSslConfiguration>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <QAbstractItemModelTester>
#include <atomic>
#include <QThread>
#include <QEventLoop>
#include <wtsapi32.h>
#include <QSemaphore>
#include <QLibrary>
#include <QScopeGuard>
#include <QWheelEvent>
#include <QHoverEvent>
#include <stdexcept>

class ReleaseUrlCapture : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:

    void open(const QUrl &url) {
        urls.append(url);
    }
};

// Stop incubation at a known binding, before componentComplete creates delegates.
class IncubationCheckpoint : public QObject {
    Q_OBJECT
public:
    std::atomic<bool> keepGoing{true};
    Q_INVOKABLE bool reached() {
        keepGoing.store(false);
        return true;
    }
};

class ControllerTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    std::unique_ptr<Controller> c;

    QJsonObject row(QString id, QJsonValue value, QString kind = "number") {
        return {{"id", id},
                {"source", "windows"},
                {"group", "Storage / Fixture"},
                {"name", "Fixture"},
                {"value", value},
                {"available", !value.isNull()},
                {"kind", kind},
                {"unit", kind == "number" ? "B" : ""},
                {"type", 0},
                {"state_class", kind == "number" ? "measurement" : ""}};
    }

    QJsonObject find(QString id) {
        for (const auto &v : c->allReadings())
            if (v.toObject()["id"] == id)
                return v.toObject();
        return {};
    }

    // Other desktop apps can briefly lock the shared Windows clipboard. Retry
    // writes with event processing between attempts; callers still verify the text.
    bool writeClipboardWithRetry(const std::function<void()> &write) {
        return QTest::qWaitFor([&] {
            write();
            return QApplication::clipboard()->ownsClipboard();
        }, 3000);
    }

    static QQuickItem *visualItem(QQuickItem *parent, const QString &name) {
        if (parent->objectName() == name)
            return parent;
        for (auto *child : parent->childItems())
            if (auto *result = visualItem(child, name))
                return result;
        return nullptr;
    }

    static bool openBrowserControl(QQuickWindow *window, Controller &controller, const QString &control,
                                   const QString &field = QString(), const QString &value = QString()) {
        controller.collapseCategories(false);
        auto *page = window->findChild<QObject *>("sensorsPage");
        if (!page)
            return false;
        const auto rows = controller.browserModel.snapshot();
        for (int index = 0; index < rows.size(); ++index) {
            const auto row = rows[index].toMap();
            if (row["control"] != control || (!field.isEmpty() && row[field] != value))
                continue;
            auto *list = window->findChild<QObject *>("sensorList");
            if (list)
                QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 0));
            page->setProperty("expandedId", row["id"]);
            auto *toggle = visualItem(window->contentItem(), "expand_" + row["id"].toString());
            auto *detail = toggle ? visualItem(toggle->parentItem(), "sensorDetailLoader") : nullptr;
            // Partly constructed controls are discoverable before the user can use them.
            return detail && detail->property("status").toInt() == 1; // Loader.Ready
        }
        return false;
    }

    // Helpers express fixture setup through the same installation API as the UI.
    bool setRemote(Controller &controller, bool enabled, const QString &url) {
        auto row = controller.installations().first().toMap();
        row["type"] = "api";
        row["url"] = enabled ? url : QString();
        return controller.saveInstallation(row);
    }

    bool saveMqtt(Controller &controller, const QVariantMap &input, const QString &password,
                  bool clearPassword = false) {
        auto row = controller.installations().first().toMap();
        row["type"] = "mqtt";
        row["enabled"] = input.value("enabled", false).toBool();
        QVariantMap options{{"host", ""}, {"port", 8883}, {"scope", "cloud"}, {"tls", true},
                            {"username", ""}, {"ca", ""}, {"certificate", ""}, {"key", ""}};
        for (auto it = input.cbegin(); it != input.cend(); ++it) options[it.key()] = it.value();
        row["mqtt"] = options;
        return controller.saveInstallation(row, password, clearPassword);
    }

private slots:
    void init() {
        // This teardown warning is not delivered through QQmlEngine::warnings.
        QTest::failOnWarning(QRegularExpression(
            ".*items in the process of being created at engine destruction.*"));
    }

    void beacnFileVerificationExplainsFailure();
    void webReadinessAndShellWarningColours();
    void applicationDialogsKeepModalOwnership();
    void moduleWarningsFollowHealth();
    void moduleWarningsOnBothPages();
    void nestedSettingsExpandTogether_data();
    void nestedSettingsExpandTogether();

    void failedInstallationSwitchKeepsCurrentPeer() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        auto installation = local.installations().first().toMap();
        const auto id = installation["id"].toString();
        QWebSocket peer;
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        peer.setSslConfiguration(ssl);
        QSignalSpy received(&peer, &QWebSocket::textMessageReceived);
        peer.open(QUrl(QString("wss://127.0.0.1:%1").arg(local.activePort)));
        QTRY_COMPARE(peer.state(), QAbstractSocket::ConnectedState);
        peer.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "auth"}, {"protocol", 2}, {"token", local.token}, {"commands_v1", true}}).toJson()));
        QTRY_VERIFY(!received.isEmpty());
        installation["mqtt"] = QVariantMap{{"host", "127.0.0.1"}, {"port", 1}, {"tls", false},
            {"scope", "local"}, {"username", ""}, {"ca", ""}, {"certificate", ""}, {"key", ""}};
        installation["type"] = "mqtt";
        QVERIFY(local.saveInstallation(installation));
        QTRY_VERIFY(local.installationStates[id]["switching"].toBool());
        QCOMPARE(peer.state(), QAbstractSocket::ConnectedState);
        // Cancel a pending switch, then start another. Its deadline must belong
        // only to the latest selection and must never tear down the retained API.
        installation["type"] = "api";
        QVERIFY(local.saveInstallation(installation));
        QTRY_VERIFY(!local.installationStates[id]["switching"].toBool());
        installation["type"] = "mqtt";
        QVERIFY(local.saveInstallation(installation));
        QTRY_VERIFY(local.installationStates[id]["switching"].toBool());
        QMetaObject::invokeMethod(local.server.worker, [&] {
            auto *worker = local.server.worker->installations[id];
            worker->transportDeadline->setInterval(100);
            worker->transportDeadline->start();
        }, Qt::BlockingQueuedConnection);
        QTRY_COMPARE(local.installations().first().toMap()["type"].toString(), QString("api"));
        QVERIFY(!local.installationStates[id]["switchError"].toString().isEmpty());
        QCOMPARE(peer.state(), QAbstractSocket::ConnectedState);
        QSignalSpy commands(&local.server, &NetworkService::commandRequested);
        peer.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "command"}, {"id", "still-live"}, {"action", "unsupported"}, {"args", QJsonObject{}}}).toJson()));
        QTRY_COMPARE(commands.count(), 1);
    }

    void installationTokenReplacementIsScoped() {
        QTemporaryDir dir;
        QString id;
        QVariantMap replacement;
        {
            Controller local(dir.path(), true);
            local.stopService();
            const auto deviceId = local.deviceId;
            const auto fingerprint = local.fingerprint;
            const auto firstId = local.installations().first().toMap()["id"].toString();
            const auto firstKey = "connections/credentials/" + firstId;
            const auto original = local.settings.value(firstKey).toMap();
            QVERIFY(local.saveInstallation({{"name", "Second House"}, {"type", "api"},
                                            {"enabled", false}, {"controls", true}, {"url", ""}}, "saved-broker-password"));
            id = local.installations().last().toMap()["id"].toString();
            const auto key = "connections/credentials/" + id;
            const auto before = local.settings.value(key).toMap();
            const auto definitions = local.settings.value("connections/installations");
            local.setSensorEnabled("pairing_fixture", true);
            const auto selected = local.selected;

            // A stale dialog must fail safely, never rotate all remaining tokens.
            QVERIFY(!local.rotateInstallationToken("removed-installation"));
            QVERIFY(!local.rotateInstallationToken(""));
            QCOMPARE(local.settings.value(firstKey).toMap(), original);
            QCOMPARE(local.settings.value(key).toMap(), before);
            QVERIFY(local.rotateInstallationToken(id));
            replacement = local.settings.value(key).toMap();
            QVERIFY(unprotectSecret(replacement["token"].toByteArray()) !=
                    unprotectSecret(before["token"].toByteArray()));
            QCOMPARE(replacement["password"], before["password"]);
            QCOMPARE(local.settings.value(firstKey).toMap(), original);
            QCOMPARE(local.settings.value("connections/installations"), definitions);
            QCOMPARE(local.deviceId, deviceId);
            QCOMPARE(local.fingerprint, fingerprint);
            QCOMPARE(local.selected, selected);
        }
        Controller restored(dir.path(), true);
        restored.stopService();
        QCOMPARE(restored.settings.value("connections/credentials/" + id).toMap(), replacement);
    }

    void installationCrudAndPersistence() {
        QTemporaryDir dir;
        QStringList ids;
        QByteArray originalToken;
        {
            Controller local(dir.path(), true);
            local.stopService();
            QCOMPARE(local.installations().size(), 1);
            auto original = local.installations().first().toMap();
            ids.append(original["id"].toString());
            const auto credential = "connections/credentials/" + ids.first();
            originalToken = unprotectSecret(local.settings.value(credential).toMap()["token"].toByteArray());
            original["name"] = "Main House";
            QVERIFY(local.saveInstallation(original));
            QCOMPARE(unprotectSecret(local.settings.value(credential).toMap()["token"].toByteArray()), originalToken);
            for (int n = 0; n < 3; ++n) {
                QVariantMap entry{{"name", QString("House %1").arg(n)}, {"type", "api"},
                                  {"url", ""}, {"enabled", false}, {"controls", false}};
                QVERIFY(local.saveInstallation(entry));
                ids.append(local.installations().last().toMap()["id"].toString());
            }
            QCOMPARE(local.installations().size(), 4);
            auto broker = local.installations().last().toMap();
            broker["type"] = "mqtt";
            broker["mqtt"] = QVariantMap{{"host", "localhost"}, {"port", 1883}, {"tls", false},
                {"scope", "local"}, {"username", "fixture"}, {"ca", ""}, {"certificate", ""}, {"key", ""}};
            QVERIFY(local.saveInstallation(broker, "PRIVATE-MQTT-PASSWORD"));
            QVERIFY(local.installations().last().toMap()["hasPassword"].toBool());
            auto invalid = broker;
            auto options = broker["mqtt"].toMap();
            options["scope"] = "cloud";
            invalid["mqtt"] = options;
            QVERIFY(!local.saveInstallation(invalid));
            QCOMPARE(local.installations().size(), 4);
            const auto backup = ConfigurationFile::encode(ConfigurationFile::snapshot(local.settings));
            QVERIFY(!backup.contains("PRIVATE-MQTT-PASSWORD"));
            QVERIFY(!backup.contains(originalToken));
            const auto imported = ConfigurationFile::decode(backup);
            QVERIFY2(imported.error.isEmpty(), qPrintable(imported.error));
            QCOMPARE(imported.values["connections/installations"].toArray().size(), 4);
            QVERIFY(local.setInstallationEnabled(ids[1], true));
            QVERIFY(local.deleteInstallation(ids[2]));
            QCOMPARE(local.installations().size(), 3);
        }
        {
            Controller restored(dir.path(), true);
            restored.stopService();
            QCOMPARE(restored.installations().size(), 3);
            QCOMPARE(restored.installations().first().toMap()["name"].toString(), QString("Main House"));
            QVERIFY(restored.installations()[1].toMap()["enabled"].toBool());
            const auto key = "connections/credentials/" + ids.first();
            QCOMPARE(unprotectSecret(restored.settings.value(key).toMap()["token"].toByteArray()), originalToken);
            for (const auto &value : restored.installations())
                QVERIFY(restored.deleteInstallation(value.toMap()["id"].toString()));
        }
        Controller empty(dir.path(), true);
        empty.stopService();
        QVERIFY(empty.installations().isEmpty()); // An empty list is deliberate, not a migration trigger.
    }

    void installationsStreamAndRevokeIndependently() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::renew(dir.path(), true);
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setLocalCertificate(identity.certificate);
        ssl.setPrivateKey(identity.key);
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        NetworkService service;
        service.configure("multiple-fixture", QString(64, 'z'), ssl);
        QJsonArray definitions;
        for (const char letter : {'a', 'b', 'c'})
            definitions.append(QJsonObject{{"id", QString(32, letter)}, {"name", QString(letter)},
                {"type", "api"}, {"enabled", true}, {"controls", letter != 'c'}, {"token", QString(64, letter)}});
        service.setInstallations(definitions);
        QVERIFY(service.listen(QHostAddress::LocalHost, 0));
        service.setInterval(1000);
        auto frame = QJsonObject{{"type", "snapshot"}, {"protocol", 2}, {"device_id", "multiple-fixture"},
            {"sensors", QJsonArray{row("fixture-reading", 1)}}, {"sample_age", 0}, {"publish_seconds", 1},
            {"controls", QJsonObject{{"permissions", QJsonObject{{"input", true}}}, {"values", QJsonObject{}}}}};
        quint16 port = 0;
        QMetaObject::invokeMethod(service.worker, [&] { port = service.worker->server->serverPort(); }, Qt::BlockingQueuedConnection);
        std::vector<std::unique_ptr<QWebSocket>> idlePeers;
        for (int i = 0; i < 16; ++i) {
            auto idle = std::make_unique<QWebSocket>();
            idle->setSslConfiguration(ssl);
            idle->open(QUrl(QString("wss://localhost:%1").arg(port)));
            idlePeers.push_back(std::move(idle));
        }
        QTRY_VERIFY(std::all_of(idlePeers.begin(), idlePeers.end(), [](const auto &peer) {
            return peer->state() == QAbstractSocket::ConnectedState;
        }));
        qsizetype pending = 0;
        QMetaObject::invokeMethod(service.worker, [&] { pending = service.worker->pendingPairings.size(); }, Qt::BlockingQueuedConnection);
        QCOMPARE(pending, 16);
        QWebSocket peers[3];
        QList<QJsonObject> received[3];
        for (int i = 0; i < 3; ++i) {
            peers[i].setSslConfiguration(ssl);
            connect(&peers[i], &QWebSocket::connected, this, [&, i] {
                peers[i].sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                    {"type", "auth"}, {"protocol", 2}, {"token", QString(64, QChar('a' + i))},
                    {"commands_v1", true}, {"delta_v1", true}, {"ui_feedback_v1", true}}).toJson()));
            });
            connect(&peers[i], &QWebSocket::textMessageReceived, this, [&, i](QString text) {
                received[i].append(QJsonDocument::fromJson(text.toUtf8()).object());
            });
            peers[i].open(QUrl(QString("wss://localhost:%1").arg(port)));
        }
        auto has = [&](int i, const QString &type) {
            return std::any_of(received[i].begin(), received[i].end(), [&](const auto &item) { return item["type"] == type; });
        };
        for (int i = 0; i < 3; ++i) QTRY_VERIFY_WITH_TIMEOUT(has(i, "hello"), 6000);
        QTRY_VERIFY(std::any_of(idlePeers.begin(), idlePeers.end(), [](const auto &peer) {
            return peer->state() == QAbstractSocket::UnconnectedState;
        }));
        idlePeers.clear();
        QTest::qWait(100); // Let the initial flush fire while no readings exist yet.
        for (int i = 0; i < 3; ++i)
            for (const auto &message : received[i])
                QVERIFY(message["type"] == "hello" || message["type"] == "ui_probe");
        service.setSnapshot(frame);
        for (int i = 0; i < 3; ++i) QTRY_VERIFY_WITH_TIMEOUT(has(i, "snapshot"), 6000);
        for (const auto &item : received[2])
            if (item["type"] == "snapshot")
                QVERIFY(!item["controls"].toObject()["permissions"].toObject()["input"].toBool());
        QSignalSpy commands(&service, &NetworkService::commandRequested);
        auto command = [&](int i, const QString &id) {
            peers[i].sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                {"type", "command"}, {"id", id}, {"action", "keyboard"},
                {"args", QJsonObject{{"keys", QJsonArray{"A"}}}}}).toJson()));
        };
        command(0, "allowed");
        QTRY_COMPARE(commands.count(), 1);
        const auto session = commands[0][0].toString();
        QCOMPARE(service.installationForSession(session), QString(32, 'a'));
        auto lease = service.commandLease(session);
        QVERIFY(lease && lease->load());
        command(2, "denied");
        QTRY_VERIFY(has(2, "command_result"));
        QCOMPARE(commands.count(), 1);
        // A label edit must not reconnect or revoke an accepted command.
        auto first = definitions[0].toObject();
        first["name"] = "Renamed";
        definitions[0] = first;
        service.setInstallations(definitions);
        QVERIFY(lease->load());
        first["enabled"] = false;
        definitions[0] = first;
        service.setInstallations(definitions);
        QVERIFY(!lease->load()); // Revocation is synchronous, before the worker consumes the edit.
        QTRY_COMPARE(peers[0].state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(peers[1].state(), QAbstractSocket::ConnectedState);
        QCOMPARE(peers[2].state(), QAbstractSocket::ConnectedState);
        received[1].clear(); received[2].clear();
        frame["sensors"] = QJsonArray{row("fixture-reading", 2)};
        frame["_cleanup_installation"] = QString(32, 'b');
        frame["entity_cleanup"] = QJsonObject{{"id", "preview"}, {"operation", "preview"}, {"token", ""}};
        service.replaceSnapshot(frame);
        // Revoking the first peer can leave an older frame in flight. Wait for
        // this reading revision before checking where its cleanup request went.
        auto hasUpdatedReading = [&](int peer) {
            for (const auto &message : received[peer])
                for (const auto &reading : message["sensors"].toArray())
                    if (reading.toObject()["id"] == "fixture-reading"
                        && reading.toObject()["value"] == 2)
                        return true;
            return false;
        };
        QTRY_VERIFY(hasUpdatedReading(1));
        QTRY_VERIFY(hasUpdatedReading(2));
        bool targeted = false;
        for (const auto &item : received[1]) {
            QVERIFY(!item.contains("_cleanup_installation"));
            targeted |= item.contains("entity_cleanup");
        }
        QVERIFY(targeted);
        for (const auto &item : received[2]) QVERIFY(!item.contains("entity_cleanup"));
        received[2].clear();
        service.setEventSnapshot(frame);
        QTRY_VERIFY(has(2, "delta") || has(2, "snapshot"));
        for (const auto &message : received[2]) {
            if (message["type"] != "snapshot" && message["type"] != "delta") continue;
            QCOMPARE(message["connections"].toArray().size(), 3);
            for (const auto &entry : message["connections"].toArray()) {
                const auto report = entry.toObject();
                QCOMPARE(report.size(), 7);
                QVERIFY(!report.contains("token") && !report.contains("endpoint") && !report.contains("mqtt"));
                if (report["id"] == QString(32, 'a')) {
                    QCOMPARE(report["name"].toString(), QString("Renamed"));
                    QCOMPARE(report["status"].toString(), QString("disabled"));
                } else {
                    QCOMPARE(report["status"].toString(), QString("connected"));
                    QVERIFY(report["connected_since"].toDouble() > 0);
                }
            }
        }

        definitions.removeAt(1);
        service.setInstallations(definitions);
        QTRY_COMPARE(peers[1].state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(peers[2].state(), QAbstractSocket::ConnectedState);
        service.setInstallations({});
        QTRY_COMPARE(peers[2].state(), QAbstractSocket::UnconnectedState);
        service.close();
    }

    void multipleOutboundApiConnections() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::renew(dir.path(), true);
        const auto original = QSslConfiguration::defaultConfiguration();
        auto trust = original;
        trust.addCaCertificate(identity.certificate);
        QSslConfiguration::setDefaultConfiguration(trust);
        auto restore = qScopeGuard([&] { QSslConfiguration::setDefaultConfiguration(original); });
        auto ssl = trust;
        ssl.setLocalCertificate(identity.certificate);
        ssl.setPrivateKey(identity.key);
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        NetworkService service;
        service.configure("outbound-installations", "unused", ssl);
        QJsonArray definitions;
        std::vector<std::unique_ptr<QWebSocketServer>> endpoints;
        QPointer<QWebSocket> peers[3];
        int snapshots[3] = {};
        for (int i = 0; i < 3; ++i) {
            auto endpoint = std::make_unique<QWebSocketServer>("fixture", QWebSocketServer::SecureMode);
            endpoint->setSslConfiguration(ssl);
            QVERIFY(endpoint->listen(QHostAddress::LocalHost, 0));
            const auto secret = QString(64, QChar('a' + i));
            definitions.append(QJsonObject{{"id", QString(32, QChar('a' + i))}, {"name", QString::number(i)},
                {"type", "api"}, {"enabled", true}, {"controls", true}, {"token", secret},
                {"endpoint", QString("wss://localhost:%1/api/what/connect").arg(endpoint->serverPort())}});
            connect(endpoint.get(), &QWebSocketServer::newConnection, this, [&, i, server = endpoint.get(), secret] {
                peers[i] = server->nextPendingConnection();
                connect(peers[i], &QWebSocket::textMessageReceived, this, [&, i, secret](QString text) {
                    const auto object = QJsonDocument::fromJson(text.toUtf8()).object();
                    if (object["type"] == "remote_auth") {
                        QCOMPARE(object["token"].toString(), secret);
                        peers[i]->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                            {"type", "auth"}, {"protocol", 2}, {"token", secret}}).toJson()));
                    } else if (object["type"] == "snapshot") ++snapshots[i];
                });
            });
            endpoints.push_back(std::move(endpoint));
        }
        service.setInstallations(definitions);
        QVERIFY(service.listen(QHostAddress::LocalHost, 0));
        service.setInterval(1000);
        const QJsonObject frame{{"type", "snapshot"}, {"protocol", 2}, {"device_id", "outbound-installations"},
                                {"sensors", QJsonArray{row("fixture", 1)}}, {"sample_age", 0}, {"publish_seconds", 1}};
        service.setSnapshot(frame);
        for (int i = 0; i < 3; ++i) QTRY_VERIFY_WITH_TIMEOUT(snapshots[i] > 0, 6000);
        endpoints[0]->close();
        peers[0]->abort();
        const int second = snapshots[1], third = snapshots[2];
        service.setSnapshot(frame);
        QTRY_VERIFY(snapshots[1] > second && snapshots[2] > third);
        QCOMPARE(peers[1]->state(), QAbstractSocket::ConnectedState);
        QCOMPARE(peers[2]->state(), QAbstractSocket::ConnectedState);
        service.close();
    }

    void multiInstallationMqttFixture() {
        const auto fixture = qEnvironmentVariable("WHAT_MULTI_MQTT_FIXTURE");
        if (fixture.isEmpty()) QSKIP("Opt-in multiple broker fixture");
        QTemporaryDir dir;
        const auto identity = TlsIdentity::renew(dir.path(), true);
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setLocalCertificate(identity.certificate);
        ssl.setPrivateKey(identity.key);
        NetworkService service;
        service.configure("multi-mqtt-fixture", "unused", ssl);
        QJsonArray definitions;
        for (int i = 0; i < 3; ++i)
            definitions.append(QJsonObject{{"id", QString(32, QChar('a' + i))}, {"name", QString::number(i)},
                {"type", "mqtt"}, {"enabled", true}, {"controls", i != 2}, {"token", QString(64, QChar('a' + i))},
                {"mqtt", QJsonObject{{"host", "localhost"}, {"port", qEnvironmentVariableIntValue(i == 1 ? "WHAT_MULTI_MQTT_PORT2" : "WHAT_MULTI_MQTT_PORT1")},
                                     {"tls", false}, {"scope", "local"}}}});
        service.setInstallations(definitions);
        QVERIFY(service.listen(QHostAddress::LocalHost, 0));
        auto frame = c->snapshot();
        frame["device_id"] = "multi-mqtt-fixture";
        frame["sensors"] = QJsonArray{row("fixture", 1)};
        frame["sample_age"] = 0;
        frame["_publishing"] = QJsonObject{};
        service.setSnapshot(frame);
        QFile ready(fixture + ".ready");
        QVERIFY(ready.open(QIODevice::WriteOnly));
        ready.write("ready"); ready.close();
        QString previous;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 60000) {
            QFile input(fixture + ".command");
            if (input.open(QIODevice::ReadOnly)) {
                const auto command = QString::fromUtf8(input.readAll()).trimmed();
                if (command == "done") break;
                if (command != previous) {
                    previous = command;
                    if (command == "disable-first") {
                        auto entry = definitions[0].toObject();
                        entry["enabled"] = false;
                        definitions[0] = entry;
                        service.setInstallations(definitions);
                        frame["sensors"] = QJsonArray{row("fixture", 2)};
                    }
                }
            }
            service.setSnapshot(frame);
            QTest::qWait(100);
        }
        service.close();
    }

    void installationEditorLoads() {
        QTest::failOnWarning(QRegularExpression(".*(Binding loop|ReferenceError|TypeError).*"));
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(window->findChild<QObject *>("installationEditor"));
        auto *editor = window->findChild<QObject *>("installationEditor");
        QVERIFY(QMetaObject::invokeMethod(editor, "edit", Q_ARG(QVariant, QVariantMap{})));
        auto *name = editor->findChild<QObject *>("installationName");
        QVERIFY(name);
        name->setProperty("text", "Test House");
        QVERIFY(QMetaObject::invokeMethod(editor, "save"));
        QCOMPARE(local.installations().size(), 2);
        QCOMPARE(local.installations().last().toMap()["name"].toString(), QString("Test House"));
        QSignalSpy configurationChanges(&local, &Controller::installationConfigurationChanged);
        const auto id = local.installations().last().toMap()["id"].toString();
        emit local.server.installationState(id, {{"clients", 1}, {"lastCommunication", QDateTime::currentMSecsSinceEpoch()}});
        QCOMPARE(configurationChanges.count(), 0); // A heartbeat must not rebuild editable rows or reset selectors.
        QCOMPARE(local.installationStatus()[id].toMap()["clients"].toInt(), 1);
        const auto preview = qEnvironmentVariable("WHAT_INSTALLATIONS_PREVIEW");
        if (!preview.isEmpty()) {
            window->setProperty("previewPage", 2);
            QTest::qWait(400);
            QVERIFY(window->grabWindow().save(preview + "/installations.png"));
            QVERIFY(QMetaObject::invokeMethod(editor, "edit", Q_ARG(QVariant, local.installations().last())));
            QTest::qWait(400);
            QVERIFY(window->grabWindow().save(preview + "/installation-editor.png"));
        }
    }

    void homeAssistantWebSettings() {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.yaml"), SettingsStore::format());
        {
            HomeAssistantWeb web(settings, dir.path(), true);
            QVERIFY(!web.enabled());
            QVERIFY(!web.loading());
            QVERIFY(!QDir(dir.filePath("home-assistant-web")).exists());
            QCOMPARE(web.hotkey(), QString("Ctrl+Alt+H"));
            QCOMPARE(web.scale(), 80);
            QCOMPARE(web.pipScale(), 80);
            QVERIFY(!web.configure("file:///C:/secret.txt", "windowed", 100, "Ctrl+Alt+H"));
            QVERIFY(!web.configure("https://user:password@example.com", "windowed", 100, "Ctrl+Alt+H"));
            QVERIFY(!web.configure("https://example.com", "windowed", 100, "H"));
            QVERIFY(!web.configure("https://example.com", "windowed", 20, "Ctrl+Alt+H"));
            QVERIFY(web.configure("https://example.com/lovelace", "fullscreen", 150, "Ctrl+Shift+F12"));
            web.setEnabled(true);
            // Enabling and configuring must not create Chromium at startup.
            QVERIFY(!web.loading());
            QVERIFY(!QDir(dir.filePath("home-assistant-web")).exists());
            settings.sync();
        }
        HomeAssistantWeb restored(settings, dir.path(), true);
        QVERIFY(restored.enabled());
        QCOMPARE(restored.url(), QString("https://example.com/lovelace"));
        QCOMPARE(restored.mode(), QString("fullscreen"));
        QCOMPARE(restored.scale(), 150);
        QCOMPARE(restored.hotkey(), QString("Ctrl+Shift+F12"));
        QCOMPARE(restored.options()["pipHotkey"].toString(), QString("Ctrl+Alt+P"));
        auto changed = restored.options();
        changed["pipHotkey"] = "Ctrl+Shift+F12";
        QVERIFY(!restored.configure("https://changed.example.com", "pip", 100, restored.hotkey(), changed));
        QCOMPARE(restored.url(), QString("https://example.com/lovelace"));
        QVERIFY(!restored.loading());
        const auto exported = ConfigurationFile::snapshot(settings);
        const auto imported = ConfigurationFile::decode(ConfigurationFile::encode(exported));
        QVERIFY2(imported.error.isEmpty(), qPrintable(imported.error));
        QCOMPARE(imported.values["modules/haweb/url"], exported["modules/haweb/url"]);
        QCOMPARE(imported.values["modules/haweb/scale"].toInt(), 150);
        QVERIFY(!ConfigurationFile::decode(ConfigurationFile::encode(
            {{"modules/haweb/url", "javascript:alert(1)"}})).error.isEmpty());
        restored.setEnabled(false);
        QSignalSpy opened(&restored, &HomeAssistantWeb::pageRequested);
        restored.open();
        QCOMPARE(opened.size(), 1);
        QVERIFY(!restored.overlayVisible());
    }

    void homeAssistantWebHotkey() {
        if (!qEnvironmentVariableIsSet("WHAT_TEST_WEBVIEW2"))
            QSKIP("Set WHAT_TEST_WEBVIEW2=1 to exercise global hotkey registration in a desktop session.");
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.yaml"), SettingsStore::format());
        HomeAssistantWeb web(settings, dir.path(), false);
        auto options = web.options();
        options["pipHotkey"] = "Ctrl+Alt+F22";
        QVERIFY(web.configure("", "windowed", 100, "Ctrl+Alt+F24", options));
        web.setEnabled(true);
        QVERIFY2(web.hotkeyError().isEmpty(), qPrintable(web.hotkeyError()));
        QVERIFY2(web.pipHotkeyError().isEmpty(), qPrintable(web.pipHotkeyError()));
        QSignalSpy opened(&web, &HomeAssistantWeb::pageRequested);
        QVERIFY(PostThreadMessage(GetCurrentThreadId(), WM_HOTKEY, 0x5748, 0));
        QTRY_COMPARE(opened.size(), 1);
        QVERIFY(PostThreadMessage(GetCurrentThreadId(), WM_HOTKEY, 0x5749, 0));
        QTRY_COMPARE(opened.size(), 2);
        web.setEnabled(false);
        // Simulate another app claiming the shortcut, then change ours to a free one.
        QVERIFY(RegisterHotKey(nullptr, 0x5750, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F24));
        const auto release = qScopeGuard([] { UnregisterHotKey(nullptr, 0x5750); });
        web.setEnabled(true);
        QVERIFY(!web.hotkeyError().isEmpty());
        QVERIFY(web.configure("", "windowed", 100, "Ctrl+Alt+F23"));
        QVERIFY2(web.hotkeyError().isEmpty(), qPrintable(web.hotkeyError()));
        web.setEnabled(false);
        QVERIFY(RegisterHotKey(nullptr, 0x5751, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F22));
        const auto releasePip = qScopeGuard([] { UnregisterHotKey(nullptr, 0x5751); });
        web.setEnabled(true);
        QVERIFY(!web.pipHotkeyError().isEmpty());
        QVERIFY(web.hotkeyError().isEmpty());
        options["pipHotkey"] = "";
        QVERIFY(web.configure("", "windowed", 100, "Ctrl+Alt+F23", options));
        QVERIFY(web.pipHotkeyError().isEmpty());
        QVERIFY(PostThreadMessage(GetCurrentThreadId(), WM_HOTKEY, 0x5749, 0));
        QTest::qWait(30);
        QCOMPARE(opened.size(), 2); // A queued message cannot invoke a disabled shortcut.
    }

    void homeAssistantWebAdvancedSettings() {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.yaml"), SettingsStore::format());
        HomeAssistantWeb web(settings, dir.path(), true);
        auto options = web.options();
        options["cloudUrl"] = "https://cloud.example.com/lovelace";
        options["fallback"] = true;
        options["desktopDashboard"] = "/dashboard-desktop/home";
        options["fullscreenDashboard"] = "https://primary.example.com/dashboard-wall";
        options["pipAspectRatio"] = "16:9";
        options["keepAudio"] = true;
        options["pipHotkey"] = "Ctrl+Alt+F20";
        options["trayAction"] = "pip";
        options["pipScale"] = 80;
        options["pipWindowScale"] = 125;
        options["sidebarWidth"] = 310;
        QVERIFY(web.configure("https://primary.example.com/", "desktop", 100, "", options));
        QVERIFY(!web.loading());
        const auto saved = web.options();
        for (const auto &bad : QList<QVariantMap>{{{"cloudUrl", "http://cloud.example.com"}},
                 {{"pipDashboard", "//another-host/path"}}, {{"pipDashboard", "file:///C:/private"}},
                 {{"pipDashboard", "/\\another-host/path"}}, {{"autoRecover", "true"}},
                 {{"pipAspectRatio", "unknown"}}, {{"unknown", true}},
                 {{"pipHotkey", "P"}}, {{"trayAction", "unknown"}},
                 {{"pipScale", true}}, {{"pipWindowScale", "150"}},
                 {{"pipScale", 99.5}}, {{"pipScale", 201}}, {{"sidebarWidth", 179}}}) {
            QVERIFY(!web.configure("https://changed.example.com", "pip", 200, "", bad));
            QCOMPARE(web.options(), saved);
            QCOMPARE(web.url(), QString("https://primary.example.com/"));
        }
        settings.sync();
        QSettings disk(dir.filePath("settings.yaml"), SettingsStore::format());
        HomeAssistantWeb restored(disk, dir.path(), true);
        QCOMPARE(restored.options(), saved);
        QCOMPARE(restored.pipScale(), 80);
        QCOMPARE(restored.scale(), 100);
        const auto exported = ConfigurationFile::snapshot(settings);
        const auto imported = ConfigurationFile::decode(ConfigurationFile::encode(exported));
        QVERIFY2(imported.error.isEmpty(), qPrintable(imported.error));
        QCOMPARE(imported.values["modules/haweb/options"].toObject().toVariantMap(), saved);
        QVERIFY(!ConfigurationFile::decode(ConfigurationFile::encode(
            {{"modules/haweb/options", QJsonObject{{"cloudUrl", "javascript:alert(1)"}}}})).error.isEmpty());

        const QRect work(0, 0, 1920, 1040);
        QCOMPARE(HomeAssistantWeb::snapPictureInPicture(QRect(9, 10, 480, 360), work).topLeft(), QPoint(0, 0));
        QCOMPARE(HomeAssistantWeb::snapPictureInPicture(QRect(1430, 670, 480, 360), work).bottomRight(), work.bottomRight());
        const QRect dragged(100, 150, 500, 360);
        const auto resized = HomeAssistantWeb::resizePictureInPicture(dragged, 16.0 / 9,
            Qt::LeftEdge | Qt::TopEdge, QSize(360, 260));
        QCOMPARE(resized.bottomRight(), dragged.bottomRight());
        QVERIFY(qAbs(double(resized.width()) / resized.height() - 16.0 / 9) < 0.01);
        QVERIFY(resized.width() >= 360 && resized.height() >= 260);
        options["pipWindowScale"] = 50;
        QVERIFY(web.configure(web.url(), web.mode(), web.scale(), web.hotkey(), options));
        const auto bounds = settings.value("modules/haweb/pip_bounds").toList();
        QCOMPARE(bounds.size(), 4);
        QVERIFY(bounds[2].toInt() >= 360 && bounds[3].toInt() >= 260);
        QVERIFY(qAbs(bounds[2].toDouble() / bounds[3].toDouble() - 16.0 / 9) < 0.01);
    }

    void homeAssistantWebSidebar() {
        if (!qEnvironmentVariableIsSet("WHAT_TEST_WEBVIEW2"))
            QSKIP("Set WHAT_TEST_WEBVIEW2=1 to test the sidebar in the browser renderer.");
        QTemporaryDir dir;
        dir.setAutoRemove(false);
        const auto path = dir.path();
        const auto cleanup = qScopeGuard([path] {
            if (!QTest::qWaitFor([&] { return QDir(path).removeRecursively(); }, 5000))
                qWarning("WebView2 still holds the temporary test profile");
        });
        QFile fixture(QFINDTESTDATA("fixtures/ha-sidebar.html"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const auto html = fixture.readAll();
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QUrl result;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    if (socket->property("served").toBool()) return;
                    const auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n")) return;
                    socket->setProperty("served", true);
                    const auto target = QUrl::fromEncoded(request.split(' ').value(1));
                    if (target.path() == "/result") result = target;
                    const auto body = target.path() == "/" ? html : QByteArray("ok");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        QSettings settings(dir.filePath("settings.yaml"), SettingsStore::format());
        HomeAssistantWeb web(settings, dir.path(), true);
        auto options = web.options();
        options["sidebarWidth"] = 310;
        QVERIFY(web.configure(QString("http://127.0.0.1:%1/").arg(server.serverPort()), "windowed", 100, "", options));
        web.browserWindow()->resize(1000, 700);
        web.browserWindow()->show();
        web.setEnabled(true);
        web.setPresented(true);
        QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 20000);
        const QUrlQuery query(result);
        QVERIFY2(query.queryItemValue("error").isEmpty(), qPrintable(query.queryItemValue("error")));
        QCOMPARE(query.queryItemValue("ok"), QString("1"));
        QTRY_COMPARE(web.options()["sidebarWidth"].toInt(), 350);
        QCOMPARE(settings.value("modules/haweb/options").toMap()["sidebarWidth"].toInt(), 350);
    }

    void homeAssistantWebNativeRecoveryAndPermissions() {
        if (!qEnvironmentVariableIsSet("WHAT_TEST_WEBVIEW2"))
            QSKIP("Set WHAT_TEST_WEBVIEW2=1 to test browser recovery and permissions.");
        QTemporaryDir dir;
        dir.setAutoRemove(false);
        const auto profilePath = dir.path();
        const auto cleanup = qScopeGuard([profilePath] {
            if (!QTest::qWaitFor([&] { return QDir(profilePath).removeRecursively(); }, 5000))
                qWarning("WebView2 still holds the temporary test profile");
        });
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QHash<QString, int> requests;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    if (socket->property("served").toBool()) return;
                    const auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n")) return;
                    socket->setProperty("served", true);
                    const auto path = QUrl::fromEncoded(request.split(' ').value(1)).path();
                    ++requests[path];
                    const QByteArray code = path == "/denied" ? "403 Forbidden" : path == "/busy" ? "503 Unavailable" : "200 OK";
                    QByteArray body = "<html><body>WHAT Web test</body></html>";
                    if (path == "/links")
                        body = "<html><body><a href=\"http://external.invalid/fixture\" target=\"_blank\" "
                               "style=\"position:fixed;inset:0;display:block\">External link</a></body></html>";
                    if (path == "/script-navigation")
                        body = "<html><body><script>setTimeout(() => { location.href = "
                               "'http://external.invalid/script'; }, 300);</script></body></html>";
                    if (path == "/dialog") body = R"(
                        <html><body><div id="shadow"></div><script>
                        const root = document.querySelector('#shadow').attachShadow({mode:'open'});
                        root.innerHTML = '<div role="dialog">Dialog</div>';
                        document.addEventListener('keydown', event => root.replaceChildren());
                        setTimeout(() => {
                            document.dispatchEvent(new KeyboardEvent('keydown', {key:'Escape', bubbles:true, cancelable:true}));
                            fetch('/dialog-dismissed');
                        }, 400);
                        </script></body></html>)";
                    if (path == "/escape") body = R"(
                        <html><body><script>
                        setTimeout(() => document.dispatchEvent(new KeyboardEvent('keydown',
                            {key:'Escape', bubbles:true, cancelable:true})), 400);
                        </script></body></html>)";
                    if (path == "/consumed") body = R"(
                        <html><body><script>
                        document.addEventListener('keydown', event => event.preventDefault());
                        setTimeout(() => {
                            document.dispatchEvent(new KeyboardEvent('keydown', {key:'Escape', bubbles:true, cancelable:true}));
                            fetch('/escape-consumed');
                        }, 400);
                        </script></body></html>)";
                    socket->write("HTTP/1.1 " + code + "\r\nContent-Type: text/html\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        QSettings settings(dir.filePath("settings.yaml"), SettingsStore::format());
        HomeAssistantWeb web(settings, dir.path(), true);
        QWindow overlay;
        overlay.setFlags(Qt::Tool | Qt::FramelessWindowHint);
        web.setOverlayWindow(&overlay);
        web.browserWindow()->resize(640, 480);
        web.browserWindow()->show();
        const QString origin = QString("http://127.0.0.1:%1").arg(server.serverPort());
        QVERIFY(web.configure(origin + '/', "windowed", 100, ""));
        web.setEnabled(true);
        web.setPresented(true);
        QTRY_VERIFY_WITH_TIMEOUT(web.ready(), 30000);
        QVERIFY(web.diagnostics()["runtime"].toString() != "Not started");

        // Terminate only this temporary profile's browser process, identified by
        // WebView2 itself. WHAT and unrelated Edge/browser sessions stay alive.
        const DWORD browserPid = web.diagnostics()["browserPid"].toUInt();
        QVERIFY(browserPid && browserPid != GetCurrentProcessId());
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, browserPid);
        QVERIFY(process);
        const auto closeProcess = qScopeGuard([process] { CloseHandle(process); });
        wchar_t executable[32768]{};
        DWORD length = DWORD(std::size(executable));
        QVERIFY(QueryFullProcessImageNameW(process, 0, executable, &length));
        QVERIFY(QString::fromWCharArray(executable).endsWith("msedgewebview2.exe", Qt::CaseInsensitive));
        QVERIFY(TerminateProcess(process, 1));
        QTRY_VERIFY_WITH_TIMEOUT(web.ready() && web.diagnostics()["browserPid"].toUInt() != browserPid &&
            web.diagnostics()["retries"].toInt() == 1, 15000);

        web.refreshPermissions();
        QTRY_VERIFY(!web.permissionsBusy());
        QVERIFY(web.permissions().size() >= 5);
        const auto permissionState = [&web](int kind) {
            for (const auto &value : web.permissions())
                if (value.toMap()["kind"].toInt() == kind) return value.toMap()["state"].toInt();
            return -1;
        };
        web.setPermission(origin, 1, 2); // Microphone: Block.
        QTRY_VERIFY(!web.permissionsBusy() && permissionState(1) == 2);
        web.resetPermissions();
        QTRY_VERIFY(!web.permissionsBusy() && permissionState(1) == 0);

        auto options = web.options();
        options["keepAudio"] = true;
        QVERIFY(web.configure(origin + '/', "windowed", 100, "", options));
        web.setPresented(false);
        QCOMPARE(web.diagnostics()["state"].toString(), QString("Hidden"));
        QVERIFY(!web.diagnostics()["muted"].toBool());
        web.toggleMuted();
        QVERIFY(web.options()["muted"].toBool());
        options = web.options();
        options["keepAudio"] = false;
        QVERIFY(web.configure(origin + '/', "windowed", 100, "", options));
        QTRY_COMPARE(web.diagnostics()["state"].toString(), QString("Suspended"));
        web.setPresented(true);
        QCOMPARE(web.diagnostics()["state"].toString(), QString("Visible"));

        options["pipDashboard"] = "/pip";
        options["fullscreenDashboard"] = "/wall";
        QVERIFY(web.configure(origin + '/', "windowed", 100, "", options));
        web.showOverlay("pip");
        QTRY_VERIFY(web.ready() && requests["/pip"] > 0);
        web.showOverlay("fullscreen");
        QTRY_VERIFY(web.ready() && requests["/wall"] > 0);
        web.hideOverlay();
        QTRY_VERIFY(web.ready() && requests["/"] > 1);
        options["fullscreenDashboard"] = "";

        QVERIFY(web.configure(origin + "/dialog", "windowed", 100, "", options));
        web.showOverlay("fullscreen");
        QTRY_VERIFY(requests["/dialog-dismissed"] > 0);
        QTest::qWait(100);
        QVERIFY(web.overlayVisible());
        QVERIFY(web.configure(origin + "/consumed", "windowed", 100, "", options));
        QTRY_VERIFY(requests["/escape-consumed"] > 0);
        QTest::qWait(100);
        QVERIFY(web.overlayVisible());
        QVERIFY(web.configure(origin + "/escape", "windowed", 100, "", options));
        QTRY_VERIFY(!web.overlayVisible());

        ReleaseUrlCapture capture;
        QDesktopServices::setUrlHandler("http", &capture, "open");
        const auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("http"); });
        web.openInBrowser();
        QCOMPARE(capture.urls.size(), 1);
        QCOMPARE(capture.urls.first(), QUrl(origin + "/escape"));

        QVERIFY(web.configure(origin + "/links", "windowed", 100, "", options));
        QTRY_VERIFY(web.ready() && requests["/links"] > 0);
        web.browserWindow()->raise();
        web.browserWindow()->requestActivate();
        // Send a real native mouse event to the fixture's Chromium content child.
        HWND renderer = nullptr;
        EnumChildWindows(reinterpret_cast<HWND>(web.browserWindow()->winId()), [](HWND window, LPARAM value) -> BOOL {
            wchar_t name[128]{};
            GetClassNameW(window, name, int(std::size(name)));
            if (QString::fromWCharArray(name).contains("RenderWidgetHost")) {
                *reinterpret_cast<HWND *>(value) = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&renderer));
        QVERIFY(renderer);
        PostMessageW(renderer, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 30));
        PostMessageW(renderer, WM_LBUTTONUP, 0, MAKELPARAM(30, 30));
        QTRY_COMPARE(capture.urls.size(), 2);
        QCOMPARE(capture.urls.last(), QUrl("http://external.invalid/fixture"));
        QVERIFY(web.ready());
        QCOMPARE(web.diagnostics()["host"].toString(), QString("127.0.0.1"));

        QSignalSpy blockedNavigation(&web, &HomeAssistantWeb::notice);
        QVERIFY(web.configure(origin + "/script-navigation", "windowed", 100, "", options));
        QTRY_VERIFY(!blockedNavigation.isEmpty());
        QVERIFY(blockedNavigation.last()[0].toString().contains("Blocked a script"));
        QCOMPARE(capture.urls.size(), 2);
        QVERIFY(web.ready());
        QCOMPARE(web.diagnostics()["host"].toString(), QString("127.0.0.1"));

        options["fallback"] = true;
        options["cloudUrl"] = "https://127.0.0.1:1/";
        QVERIFY(web.configure(origin + "/denied", "windowed", 100, "", options));
        QTRY_VERIFY(web.status().contains("authentication"));
        QVERIFY(web.issues().join('\n').contains("authentication"));
        QCOMPARE(web.diagnostics()["route"].toString(), QString("Primary address"));
        QVERIFY(!web.diagnostics()["retryPending"].toBool());
        QCOMPARE(web.diagnostics()["retries"].toInt(), 0);

        const auto identity = TlsIdentity::renew(dir.path(), true);
        struct CertificateEndpoint : QTcpServer {
            QSslConfiguration ssl;
            void incomingConnection(qintptr descriptor) override {
                auto *socket = new QSslSocket(this);
                socket->setSslConfiguration(ssl);
                socket->setSocketDescriptor(descriptor);
                connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
                socket->startServerEncryption();
            }
        } tls;
        tls.ssl = QSslConfiguration::defaultConfiguration();
        tls.ssl.setLocalCertificate(identity.certificate);
        tls.ssl.setPrivateKey(identity.key);
        tls.ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        QVERIFY(tls.listen(QHostAddress::LocalHost));
        QVERIFY(web.configure(QString("https://127.0.0.1:%1/").arg(tls.serverPort()), "windowed", 100, "", options));
        QTRY_VERIFY_WITH_TIMEOUT(web.status().contains("certificate"), 10000);
        QVERIFY(web.issues().join('\n').contains("certificate"));
        QCOMPARE(web.diagnostics()["route"].toString(), QString("Primary address"));
        QCOMPARE(web.diagnostics()["retries"].toInt(), 0);
        QVERIFY(!web.diagnostics()["retryPending"].toBool());

        QVERIFY(web.configure(origin + "/busy", "windowed", 100, "", options));
        QTRY_COMPARE(web.diagnostics()["route"].toString(), QString("Cloud fallback"));
        options["fallback"] = false;
        const int initial = requests["/busy"];
        QVERIFY(web.configure(origin + "/busy", "windowed", 100, "", options));
        QTRY_VERIFY_WITH_TIMEOUT(web.status().contains("Automatic retries stopped"), 35000);
        QCOMPARE(requests["/busy"] - initial, 4);
        QCOMPARE(web.diagnostics()["retries"].toInt(), 3);
        QVERIFY(!web.diagnostics()["retryPending"].toBool());
        QVERIFY(!web.issues().isEmpty());
        QVERIFY(web.configure(origin + '/', "windowed", 100, "", options));
        QTRY_VERIFY(web.ready());
        QVERIFY(web.issues().isEmpty());
        web.setEnabled(false);
        QVERIFY(!web.ready());
        QVERIFY(web.issues().isEmpty());
        web.setOverlayWindow(nullptr);
    }

    void homeAssistantWebPlacement() {
        // Geometry saved on a removed monitor must leave the whole PiP window
        // reachable, including its drag bar, on the remaining monitor.
        const QRect work(-1920, 40, 1920, 1040);
        const auto defaultBounds = HomeAssistantWeb::fitPictureInPicture({}, work);
        QCOMPARE(defaultBounds.size(), QSize(640, 480));
        QVERIFY(work.contains(defaultBounds));
        QVERIFY(work.contains(HomeAssistantWeb::fitPictureInPicture(QRect(8000, 6000, 9000, 8000), work)));
        QCOMPARE(HomeAssistantWeb::fitPictureInPicture(QRect(0, 0, 12, 12), work).size(), QSize(360, 260));
        QCOMPARE(HomeAssistantWeb::fitPictureInPicture({}, QRect(0, 0, 300, 200)), QRect(0, 0, 300, 200));

        QTemporaryDir dir;
        for (const auto &mode : {QString("desktop"), QString("pip")}) {
            {
                QSettings settings(dir.filePath("settings.yaml"), SettingsStore::format());
                HomeAssistantWeb web(settings, dir.path(), true);
                QVERIFY(web.configure("https://example.com", mode, 100, ""));
                web.setEnabled(true);
            }
            QSettings saved(dir.filePath("settings.yaml"), SettingsStore::format());
            HomeAssistantWeb restored(saved, dir.path(), true);
            QCOMPARE(restored.mode(), mode);
            const auto imported = ConfigurationFile::decode(ConfigurationFile::encode(ConfigurationFile::snapshot(saved)));
            QVERIFY2(imported.error.isEmpty(), qPrintable(imported.error));
            QCOMPARE(imported.values["modules/haweb/mode"].toString(), mode);
            if (mode == "desktop") {
                QWindow desktop;
                desktop.setFlags(Qt::Tool | Qt::FramelessWindowHint);
                restored.setDesktopWindow(&desktop);
                QVERIFY(restored.desktopActive());
                QVERIFY(desktop.isVisible());
                restored.setEnabled(false);
                QVERIFY(!desktop.isVisible());
                restored.setDesktopWindow(nullptr);
            }
        }
    }

    void homeAssistantWebModuleControls() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.setAnimationsEnabled(false);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                     {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(QMetaObject::invokeMethod(window, "openModule", Q_ARG(QVariant, QString("haweb"))));
        local.setModuleEnabled("haweb", true);
        const QMap<QString, QVariant> fields{{"hawebAddress", "https://example.com/dashboard"},
                                             {"hawebHotkey", "Ctrl+Alt+F12"},
                                             {"hawebScale", 125}, {"hawebMode", 1}};
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            auto *input = visualItem(window->contentItem(), it.key());
            QVERIFY(input);
            QVERIFY(input->isEnabled());
            const char *property = it.key() == "hawebScale" ? "value"
                : it.key() == "hawebMode" ? "currentIndex" : "text";
            QVERIFY(input->setProperty(property, it.value()));
        }
        auto *apply = visualItem(window->contentItem(), "hawebApply");
        QVERIFY(apply);
        QVERIFY(apply->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
        QVERIFY(!apply->isEnabled());
        QCOMPARE(local.homeAssistantWeb->url(), QString("https://example.com/dashboard"));
        QCOMPARE(local.homeAssistantWeb->hotkey(), QString("Ctrl+Alt+F12"));
        QCOMPARE(local.homeAssistantWeb->scale(), 125);
        QCOMPARE(local.homeAssistantWeb->mode(), QString("fullscreen"));
        auto *pipKey = visualItem(window->contentItem(), "hawebPipHotkey");
        auto *trayAction = visualItem(window->contentItem(), "hawebTrayAction");
        QVERIFY(pipKey && trayAction);
        pipKey->setProperty("text", "Ctrl+Alt+F21");
        QVERIFY(QMetaObject::invokeMethod(pipKey, "textEdited"));
        trayAction->setProperty("currentIndex", 2);
        QVERIFY(QMetaObject::invokeMethod(trayAction, "activated", Q_ARG(int, 2)));
        QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
        QCOMPARE(local.homeAssistantWeb->options()["pipHotkey"].toString(), QString("Ctrl+Alt+F21"));
        QCOMPARE(local.homeAssistantWeb->options()["trayAction"].toString(), QString("pip"));
        QVERIFY(!apply->isEnabled());
        // Edits stay local until Apply; reverting restores the saved values.
        auto *address = visualItem(window->contentItem(), "hawebAddress");
        address->setProperty("text", "https://example.org/unsaved");
        QVERIFY(apply->isEnabled());
        QCOMPARE(local.homeAssistantWeb->url(), QString("https://example.com/dashboard"));
        auto *revert = visualItem(window->contentItem(), "hawebRevert");
        QVERIFY(revert && revert->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(revert, "clicked"));
        QCOMPARE(address->property("text").toString(), local.homeAssistantWeb->url());
        QVERIFY(!apply->isEnabled());
        QVERIFY(!local.homeAssistantWeb->loading());
        auto *pageScale = visualItem(window->contentItem(), "hawebPageScale");
        QVERIFY(pageScale);
        pageScale->setProperty("value", 90);
        QVERIFY(QMetaObject::invokeMethod(pageScale, "valueModified"));
        QCOMPARE(local.homeAssistantWeb->scale(), 90);
        QCOMPARE(local.settings.value("modules/haweb/scale").toInt(), 90);
        QVERIFY(!apply->isEnabled());
        QCOMPARE(visualItem(window->contentItem(), "hawebScale")->property("value").toInt(), 90);
        local.homeAssistantWeb->setScale(500);
        QCOMPARE(local.homeAssistantWeb->scale(), 200);
        QCOMPARE(pageScale->property("value").toInt(), 200);
        local.homeAssistantWeb->setScale(0);
        QCOMPARE(local.homeAssistantWeb->scale(), 50);
        QCOMPARE(local.homeAssistantWeb->url(), QString("https://example.com/dashboard"));
        QVERIFY(!local.homeAssistantWeb->loading());
        // This layout check should not start Chromium or contact a real site.
        auto *host = window->findChild<QObject *>("hawebHost");
        QVERIFY(host);
        host->setProperty("blocked", true);
        window->setProperty("previewPage", 9);
        window->resize(window->minimumWidth(), window->height());
        QTRY_VERIFY(pageScale->isVisible());
        QTest::qWait(100);
        if (!qEnvironmentVariableIsEmpty("WHAT_WEB_PREVIEW"))
            window->grabWindow().save(qEnvironmentVariable("WHAT_WEB_PREVIEW"));
        for (auto *item : pageScale->parentItem()->childItems()) {
            if (item->isVisible())
                QVERIFY2(item->x() + item->width() <= pageScale->parentItem()->width() + 1,
                         qPrintable("Web toolbar overflows at " + item->objectName()));
        }
        QCOMPARE(warnings.size(), 0);
    }

    void homeAssistantWebNativeSession() {
        if (!qEnvironmentVariableIsSet("WHAT_TEST_WEBVIEW2"))
            QSKIP("Set WHAT_TEST_WEBVIEW2=1 to exercise the installed WebView2 Runtime with a local test server.");

        QTemporaryDir dir;
        // WebView2 may retain its profile briefly while its processes exit.
        dir.setAutoRemove(false);
        const auto profilePath = dir.path();
        const auto cleanup = qScopeGuard([profilePath] {
            const bool removed = QTest::qWaitFor([&] { return QDir(profilePath).removeRecursively(); }, 5000);
            if (!removed) qWarning("WebView2 still holds the temporary test profile");
        });
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QUrl> observations;
        QList<int> zoomWidths;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n")) return;
                    const auto target = QUrl::fromEncoded(request.split(' ').value(1));
                    QByteArray body;
                    if (target.path() == "/zoom") {
                        zoomWidths.append(QUrlQuery(target).queryItemValue("width").toInt());
                        body = "ok";
                    } else if (target.path() == "/state") {
                        observations.append(target);
                        body = "ok";
                    } else {
                        body = R"(<html><body style="background:#223344;color:white">WHAT browser fixture
                            <button id="button">Test control</button><script>
                            const previous = localStorage.getItem('fixture') || '';
                            localStorage.setItem('fixture', 'kept');
                            addEventListener('resize', () => fetch('/zoom?width=' + innerWidth));
                            requestAnimationFrame(() => requestAnimationFrame(() =>
                                fetch('/state?stored=' + previous + '&width=' + innerWidth)));
                            </script></body></html>)";
                    }
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        Controller local(dir.path(), true);
        local.setAnimationsEnabled(false);
        auto *web = local.homeAssistantWeb;
        const QString address = QString("http://127.0.0.1:%1/").arg(server.serverPort());
        QVERIFY(web->configure(address, "windowed", 100, "Ctrl+Alt+H"));
        web->setEnabled(true);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                     {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(!web->loading());
        window->setProperty("previewPage", 9);
        // Moving the view while Chromium is still starting must not destroy
        // the native parent passed to its asynchronous creation callback.
        web->showOverlay("pip");
        QTest::qWait(10);
        web->showOverlay("fullscreen");
        web->hideOverlay();
        QTRY_VERIFY_WITH_TIMEOUT(web->ready(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!observations.isEmpty(), 5000);
        QVERIFY(observations.last().query().contains("stored=&"));
        const auto beforeZoom = observations.size();
        const auto viewportWidth = [&] {
            return zoomWidths.isEmpty()
                ? QUrlQuery(observations.last()).queryItemValue("width").toInt() : zoomWidths.last();
        };
        auto *container = visualItem(window->contentItem(), "hawebBrowserContainer");
        QVERIFY(container);
        // The first document frame may precede WindowContainer's final layout.
        // A temporary PiP host can report a consistent, but obsolete, width.
        // Wait for the embedded container before comparing the zoom result.
        QTRY_COMPARE(container->window(), window);
        QTRY_COMPARE(web->browserWindow()->width(), qRound(container->width()));
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(viewportWidth() - web->browserWindow()->width()) <= 2, 5000);
        const int beforeZoomEvents = zoomWidths.size();
        web->setScale(125);
        // The main window can resize after reparenting. Verify the actual zoom
        // ratio against its current size, not a width captured in the PiP host.
        QTRY_VERIFY2_WITH_TIMEOUT(zoomWidths.size() > beforeZoomEvents &&
            qAbs(viewportWidth() * 1.25 - web->browserWindow()->width()) <= 2,
            qPrintable(QString("Native width %1; zoomed viewport %2")
                .arg(web->browserWindow()->width()).arg(viewportWidth())), 5000);
        QCOMPARE(observations.size(), beforeZoom); // Zoom keeps the loaded page and login.
        QCOMPARE(local.settings.value("modules/haweb/scale").toInt(), 125);
        web->setScale(100);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(viewportWidth() - web->browserWindow()->width()) <= 2, 5000);
        QTRY_VERIFY(web->browserWindow()->isVisible());
        const auto beforeOverlay = observations.size();

        // Reparent, dismiss, and navigate away without creating a new session.
        web->toggleOverlay();
        QTRY_VERIFY(web->overlayVisible());
        QVERIFY(web->ready());
        auto *overlay = window->findChild<QQuickWindow *>("homeAssistantWebOverlay");
        QVERIFY(overlay);
        QTRY_VERIFY(container->window() == overlay);
        QTRY_VERIFY(IsWindowVisible(GetWindow(reinterpret_cast<HWND>(web->browserWindow()->winId()), GW_CHILD)));
        if (qEnvironmentVariableIsSet("WHAT_WEBVIEW_SCREENSHOTS")) {
            QTest::qWait(400);
            overlay->screen()->grabWindow(overlay->winId()).save(
                qEnvironmentVariable("WHAT_WEBVIEW_SCREENSHOTS") + "/web-overlay.png");
        }
        web->hideOverlay();
        QTRY_VERIFY(container->window() == window);
        QTRY_VERIFY(IsWindowVisible(GetWindow(reinterpret_cast<HWND>(web->browserWindow()->winId()), GW_CHILD)));
        window->setProperty("previewPage", 0);
        QTRY_VERIFY(!web->browserWindow()->isVisible());
        window->setProperty("previewPage", 9);
        QTRY_VERIFY(web->browserWindow()->isVisible());
        QVERIFY(web->ready());
        QCOMPARE(observations.size(), beforeOverlay);

        auto *host = window->findChild<QObject *>("hawebHost");
        QVERIFY(host);
        auto displayOptions = web->options();
        displayOptions["autoHideToolbar"] = true;
        QVERIFY(web->configure(address, "windowed", 100, "", displayOptions));
        QTRY_VERIFY_WITH_TIMEOUT(!host->property("toolbarVisible").toBool(), 5000);
        window->raise();
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        emit web->focusToolbarRequested();
        QTRY_VERIFY(host->property("toolbarVisible").toBool());
        QTRY_VERIFY(window->activeFocusItem() && window->activeFocusItem()->objectName() == "hawebReload");
        QTest::qWait(2700);
        QVERIFY(host->property("toolbarVisible").toBool()); // Keyboard focus keeps it open.
        displayOptions["autoHideToolbar"] = false;
        QVERIFY(web->configure(address, "windowed", 100, "", displayOptions));

        web->showOverlay("pip");
        QTRY_VERIFY(overlay->isVisible());
        QCOMPARE(web->overlayMode(), QString("pip"));
        QVERIFY(overlay->screen()->availableGeometry().contains(overlay->geometry()));
        const auto originalPipSize = overlay->size();
        const int zoomEvents = zoomWidths.size();
        web->setPipScale(150);
        QTRY_VERIFY(zoomWidths.size() > zoomEvents);
        QCOMPARE(overlay->size(), originalPipSize);
        QCOMPARE(web->scale(), 100);
        QCOMPARE(web->pipScale(), 150);
        QCOMPARE(observations.size(), beforeOverlay);
        auto scaledOptions = web->options();
        scaledOptions["pipWindowScale"] = 125;
        QVERIFY(web->configure(address, "windowed", 100, "", scaledOptions));
        QTRY_COMPARE(overlay->size(), QSize(800, 600));
        QCOMPARE(web->pipScale(), 150);
        web->setPipScale(100);
        const QRect resized(overlay->screen()->availableGeometry().topLeft() + QPoint(30, 40), QSize(480, 360));
        overlay->setGeometry(resized);
        web->hideOverlay();
        web->showOverlay("pip");
        QTRY_COMPARE(overlay->geometry(), resized);
        QTRY_VERIFY(container->height() > overlay->height() - 100);
        auto *collapseToolbar = visualItem(overlay->contentItem(), "hawebPipCollapseToolbar");
        auto *revealToolbar = visualItem(overlay->contentItem(), "hawebPipRevealToolbar");
        auto *minimise = visualItem(overlay->contentItem(), "hawebPipMinimise");
        QVERIFY(collapseToolbar && revealToolbar && minimise);
        auto *pipLogo = visualItem(overlay->contentItem(), "hawebPipLogo");
        QVERIFY(pipLogo);
        pipLogo->setProperty("animationsEnabled", true);
        QTest::mouseMove(overlay, pipLogo->mapToScene(QPointF(10, 12)).toPoint());
        QTRY_VERIFY(pipLogo->property("animating").toBool());
        pipLogo->setProperty("animationsEnabled", false);
        QVERIFY(QMetaObject::invokeMethod(collapseToolbar, "clicked"));
        QTRY_VERIFY(revealToolbar->isVisible());
        emit web->revealToolbarRequested();
        QVERIFY(revealToolbar->isVisible()); // Pointer movement must not undo an explicit collapse.
        QVERIFY(QMetaObject::invokeMethod(revealToolbar, "clicked"));
        QTRY_VERIFY(minimise->isVisible());
        QVERIFY(QMetaObject::invokeMethod(minimise, "clicked"));
        QTRY_VERIFY(web->pipMinimised());
        QTRY_COMPARE(overlay->height(), 44);
        QCOMPARE(overlay->geometry().bottom(), overlay->screen()->availableGeometry().bottom());
        QTRY_VERIFY(!web->browserWindow()->isVisible());
        QCOMPARE(observations.size(), beforeOverlay);
        const QPoint moved = resized.topLeft() + QPoint(20, 25);
        overlay->setPosition(moved);
        QVERIFY(QMetaObject::invokeMethod(minimise, "clicked"));
        QTRY_VERIFY(!web->pipMinimised());
        QTRY_COMPARE(overlay->geometry(), QRect(moved, resized.size()));
        QTRY_VERIFY(web->browserWindow()->isVisible());
        QCOMPARE(observations.size(), beforeOverlay);
        web->togglePipMinimised();
        web->hideOverlay();
        web->togglePictureInPicture();
        QTRY_COMPARE(overlay->geometry(), QRect(moved, resized.size()));
        QVERIFY(!web->pipMinimised());
        // The compact toolbar must fit even at the smallest permitted width.
        overlay->resize(360, 260);
        QTest::qWait(50);
        auto *toolbar = visualItem(overlay->contentItem(), "hawebPipToolbar");
        QVERIFY(toolbar);
        for (auto *item : toolbar->childItems()) {
            if (item->isVisible())
                QVERIFY2(item->x() + item->width() <= toolbar->width() + 1,
                         qPrintable("PiP toolbar overflows at " + item->objectName()));
        }
        auto *actions = visualItem(overlay->contentItem(), "hawebPipActions");
        QVERIFY(actions && actions->isVisible());
        for (auto *item : actions->childItems())
            if (item->isVisible())
                QVERIFY2(item->x() + item->width() <= actions->width() + 1,
                         qPrintable("PiP action overflows at " + item->objectName()));
        auto *zoomIn = visualItem(overlay->contentItem(), "hawebPipZoomIn");
        QVERIFY(zoomIn);
        QVERIFY(QMetaObject::invokeMethod(zoomIn, "clicked"));
        QCOMPARE(web->pipScale(), 110);
        QCOMPARE(web->scale(), 100);
        web->setPipScale(100);
        if (qEnvironmentVariableIsSet("WHAT_WEBVIEW_SCREENSHOTS"))
            overlay->screen()->grabWindow(overlay->winId()).save(
                qEnvironmentVariable("WHAT_WEBVIEW_SCREENSHOTS") + "/web-pip-small.png");
        overlay->setGeometry(resized);
        // Exercise Windows' actual sizing loop, not only setGeometry(). Key
        // messages are addressed to this fixture window and finish the loop.
        const auto overlayHwnd = reinterpret_cast<HWND>(overlay->winId());
        QTimer finishSizing;
        finishSizing.setSingleShot(true);
        connect(&finishSizing, &QTimer::timeout, this, [overlayHwnd] {
            PostMessageW(overlayHwnd, WM_KEYDOWN, VK_RIGHT, 1);
            PostMessageW(overlayHwnd, WM_KEYDOWN, VK_DOWN, 1);
            PostMessageW(overlayHwnd, WM_KEYDOWN, VK_RETURN, 1);
        });
        finishSizing.start(100);
        QVERIFY(overlay->startSystemResize(Qt::RightEdge | Qt::BottomEdge));
        QTRY_VERIFY_WITH_TIMEOUT(overlay->width() > resized.width(), 3000);
        QVERIFY(GetWindowLongPtrW(reinterpret_cast<HWND>(overlay->winId()), GWL_EXSTYLE) & WS_EX_TOPMOST);
        if (qEnvironmentVariableIsSet("WHAT_WEBVIEW_SCREENSHOTS")) {
            QTest::qWait(400);
            overlay->screen()->grabWindow(overlay->winId()).save(
                qEnvironmentVariable("WHAT_WEBVIEW_SCREENSHOTS") + "/web-pip.png");
        }
        web->hideOverlay();

        // Desktop stays below WHAT, survives a minimise request, and temporarily
        // lends its browser to an overlay without navigating or logging in again.
        QVERIFY(web->configure(address, "desktop", 100, ""));
        auto *desktop = window->findChild<QQuickWindow *>("homeAssistantWebDesktop");
        QVERIFY(desktop);
        QTRY_VERIFY(web->desktopActive() && desktop->isVisible());
        QCOMPARE(desktop->geometry(), desktop->screen()->availableGeometry());
        QTRY_VERIFY(container->window() == desktop);
        const auto desktopHwnd = reinterpret_cast<HWND>(desktop->winId());
        const auto mainHwnd = reinterpret_cast<HWND>(window->winId());
        const auto belowMain = [&] {
            for (auto above = GetWindow(desktopHwnd, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV))
                if (above == mainHwnd) return true;
            return false;
        };
        QTRY_VERIFY(belowMain());
        PostMessageW(desktopHwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        QTest::qWait(100);
        QVERIFY(!IsIconic(desktopHwnd));
        if (qEnvironmentVariableIsSet("WHAT_TEST_DESKTOP_SHELL")) {
            // Opt-in because Show Desktop briefly affects other open apps.
            // Production adds native minimise styles after loading Main.qml.
            // Without them the frameless test window cannot be minimised by Win+D.
            WindowFrame frame(mainHwnd);
            IShellDispatch4 *shell = nullptr;
            QVERIFY(SUCCEEDED(CoCreateInstance(__uuidof(Shell), nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&shell))));
            const auto releaseShell = qScopeGuard([&] { shell->Release(); });
            window->raise();
            window->requestActivate();
            QTRY_VERIFY(window->isActive());
            QVERIFY(SUCCEEDED(shell->ToggleDesktop()));
            const auto restoreDesktop = qScopeGuard([&] { shell->ToggleDesktop(); });
            QTest::qWait(700);
            QTRY_VERIFY(IsWindowVisible(desktopHwnd));
            RECT bounds{};
            GetWindowRect(desktopHwnd, &bounds);
            const POINT center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
            QTRY_COMPARE_WITH_TIMEOUT(GetAncestor(WindowFromPoint(center), GA_ROOT), desktopHwnd, 3000);
            QVERIFY(!(GetWindowLongPtrW(desktopHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST));
            if (qEnvironmentVariableIsSet("WHAT_WEBVIEW_SCREENSHOTS"))
                desktop->screen()->grabWindow(desktop->winId()).save(
                    qEnvironmentVariable("WHAT_WEBVIEW_SCREENSHOTS") + "/web-desktop.png");
        }
        window->hide();
        QTRY_VERIFY(web->browserWindow()->isVisible());
        web->showOverlay("pip");
        QTRY_VERIFY(!desktop->isVisible());
        QTRY_VERIFY(container->window() == overlay);
        web->hideOverlay();
        QTRY_VERIFY(desktop->isVisible());
        QTRY_VERIFY(container->window() == desktop);
        QTRY_VERIFY(IsWindowVisible(GetWindow(reinterpret_cast<HWND>(web->browserWindow()->winId()), GW_CHILD)));
        QCOMPARE(observations.size(), beforeOverlay);
        QVERIFY(web->configure(address, "windowed", 100, ""));
        QTRY_VERIFY(!desktop->isVisible());
        QVERIFY(!web->desktopActive());
        window->show();
        QTRY_VERIFY(container->window() == window);
        web->openView("desktop");
        QTRY_VERIFY(web->desktopActive());
        QCOMPARE(web->mode(), QString("windowed"));
        web->openView("windowed");
        QTRY_VERIFY(!web->desktopActive());
        QTRY_VERIFY(container->window() == window);
        QCOMPARE(observations.size(), beforeOverlay);

        QVERIFY(QMetaObject::invokeMethod(window, "openQuitConfirmation"));
        QTRY_VERIFY(!web->browserWindow()->isVisible());
        auto *quit = window->findChild<QObject *>("trayQuitConfirmation");
        QVERIFY(quit);
        QVERIFY(QMetaObject::invokeMethod(quit, "reject"));
        QTRY_VERIFY(web->browserWindow()->isVisible());

        window->hide();
        web->toggleOverlay();
        QTRY_VERIFY(overlay->isVisible());
        QTRY_VERIFY(web->browserWindow()->isVisible());
        QVERIFY(!window->isVisible());
        web->hideOverlay();
        window->show();
        QTRY_VERIFY(web->browserWindow()->isVisible());

        // Non-destructive cache clearing must keep local storage used for login.
        observations.clear();
        web->clearCache(false);
        QTRY_VERIFY_WITH_TIMEOUT(!web->cacheBusy() && web->ready() && !observations.isEmpty(), 15000);
        QVERIFY(observations.last().query().contains("stored=kept"));
        observations.clear();
        web->clearCache(true);
        QTRY_VERIFY_WITH_TIMEOUT(!web->cacheBusy() && web->ready() && !observations.isEmpty(), 15000);
        QVERIFY(observations.last().query().contains("stored=&"));

        const int normalWidth = QUrlQuery(observations.last()).queryItemValue("width").toInt();
        QVERIFY(web->configure(address, "fullscreen", 150, ""));
        observations.clear();
        web->reload();
        QTRY_VERIFY_WITH_TIMEOUT(web->ready() && !observations.isEmpty(), 10000);
        const int zoomedWidth = QUrlQuery(observations.last()).queryItemValue("width").toInt();
        QVERIFY(normalWidth > 0);
        QVERIFY2(qAbs(zoomedWidth / double(normalWidth) - 1.0 / 1.5) < 0.05,
                 qPrintable(QString("Dashboard width at 100%: %1; at 150%: %2")
                                .arg(normalWidth).arg(zoomedWidth)));
        web->open();
        QTRY_VERIFY(web->overlayVisible());
        web->setEnabled(false);
        QVERIFY(!web->overlayVisible());
        QVERIFY(!web->ready());
        QVERIFY(!web->loading());
        if (qEnvironmentVariableIsSet("WHAT_WEBVIEW_SCREENSHOTS")) {
            web->setEnabled(true);
            QVERIFY(QMetaObject::invokeMethod(window, "openModule", Q_ARG(QVariant, QString("haweb"))));
            if (auto *section = window->findChild<QObject *>("Display and AudioSectionToggle"))
                QVERIFY(QMetaObject::invokeMethod(section, "clicked"));
            QTest::qWait(500);
            window->screen()->grabWindow(window->winId()).save(
                qEnvironmentVariable("WHAT_WEBVIEW_SCREENSHOTS") + "/web-settings.png");
        }
        window->hide();
    }

    void customSensorsDoNotStarve() {
        CustomModule module(false);
        for (int i = 0; i < 5; ++i) {
            auto definition = module.newDefinition("sensor");
            definition["name"] = QString("Slow source %1").arg(i);
            definition["source"] = "script";
            definition["shell"] = "powershell";
            definition["command"] = "Start-Sleep -Milliseconds 1100; Write-Output ready";
            definition["interval"] = 1;
            definition["timeout"] = 10;
            auto outputs = definition["outputs"].toList();
            auto output = outputs[0].toMap();
            output["kind"] = "text";
            outputs[0] = output;
            definition["outputs"] = outputs;
            QVERIFY(!module.save(definition).contains("error"));
        }
        module.setEnabled(true);
        auto allCollected = [&] {
            const auto readings = module.readings();
            return readings.size() == 5 && std::all_of(readings.begin(), readings.end(), [](const auto &row) {
                return row.toObject()["available"].toBool() && row.toObject()["value"] == "ready";
            });
        };
        QTRY_VERIFY_WITH_TIMEOUT(allCollected(), 15000);
        module.setEnabled(false);
    }

    void iconImageImportIsBounded() {
        QTemporaryDir dir;
        const auto path = dir.filePath("icon.png");
        QImage source(1024, 512, QImage::Format_ARGB32);
        source.fill(QColor(10, 100, 180, 150));
        QVERIFY(source.save(path));
        const auto result = SensorTrayManager::readImageFile(path);
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        const auto image = QImage::fromData(QByteArray::fromBase64(result["image"].toString().toLatin1()));
        QCOMPARE(image.size(), QSize(256, 128));
        QCOMPARE(image.pixelColor(0, 0).alpha(), 150);
        QVERIFY(SensorTrayManager::readImageFile(dir.filePath("missing.png")).contains("error"));
        QImage oversized(8193, 1, QImage::Format_ARGB32);
        oversized.fill(Qt::red);
        QVERIFY(oversized.save(path));
        QVERIFY(SensorTrayManager::readImageFile(path).contains("error"));
    }

    void customSourcesAndStableIdentity() {
        CustomModule module(true);
        QJsonObject output{{"id", module.newId()}, {"name", "Free Space"}, {"path", "disk.free"},
                           {"kind", "number"},     {"unit", "B"},          {"state_class", "measurement"}};
        QJsonObject definition{{"id", module.newId()},
                               {"name", "My Disk"},
                               {"mode", "sensor"},
                               {"source", "script"},
                               {"shell", "cmd"},
                               {"command", "echo {\"disk\":{\"free\":123}}"},
                               {"format", "json"},
                               {"interval", 30},
                               {"timeout", 5},
                               {"enabled", true},
                               {"outputs", QJsonArray{output}}};
        QVERIFY(CustomSources::validate(definition).isEmpty());
        const auto result = CustomSources::run(definition, [] { return false; });
        QVERIFY2(result["ok"].toBool(), qPrintable(result["error"].toString()));
        const auto reading = CustomSources::readings(definition, result).first().toObject();
        QCOMPARE(reading["value"].toInt(), 123);
        QCOMPARE(reading["source"].toString(), "custom");
        QCOMPARE(reading["id"].toString(), "custom_" + output["id"].toString());

        // Text output remains text even when it happens to look like JSON.
        auto textDefinition = definition;
        textDefinition["format"] = "text";
        textDefinition["command"] = "echo true";
        auto textOutput = output;
        textOutput["kind"] = "text";
        textOutput["path"] = "";
        textOutput["unit"] = "";
        textOutput["state_class"] = "";
        textDefinition["outputs"] = QJsonArray{textOutput};
        const auto textResult = CustomSources::run(textDefinition, [] { return false; });
        QVERIFY2(textResult["ok"].toBool(), qPrintable(textResult["error"].toString()));
        QVERIFY(textResult["value"].isString());
        QCOMPARE(textResult["value"].toString(), "true");

        // A missing array element must become unavailable, never assert or read past the array.
        output["path"] = "values.999";
        definition["outputs"] = QJsonArray{output};
        const auto missing = CustomSources::readings(
            definition, {{"ok", true}, {"value", QJsonObject{{"values", QJsonArray{1}}}}});
        QVERIFY(!missing.first().toObject()["available"].toBool());
        QVERIFY(!module.save(definition.toVariantMap()).contains("error"));
        definition["name"] = "Renamed Disk";
        QVERIFY(!module.save(definition.toVariantMap()).contains("error"));
        QCOMPARE(module.definitions().size(), 1);
        QCOMPARE(module.definitions().first().toObject()["id"], definition["id"]);
        output["unit"] = "GB";
        definition["outputs"] = QJsonArray{output};
        QVERIFY(module.save(definition.toVariantMap()).contains("error"));
        QCOMPARE(module.definitions()
                     .first()
                     .toObject()["outputs"]
                     .toArray()
                     .first()
                     .toObject()["unit"]
                     .toString(),
                 "B");
    }

    void customCommandPermissionsAndPreview() {
        CustomModule module(true);
        QJsonObject definition{{"id", module.newId()}, {"name", "Fixture Command"},
                               {"mode", "action"},     {"source", "script"},
                               {"shell", "cmd"},       {"command", "echo private-output"},
                               {"format", "text"},     {"interval", 30},
                               {"timeout", 5},         {"enabled", true}};
        QVERIFY(!module.save(definition.toVariantMap()).contains("error"));
        definition["leave_running"] = true;
        QVERIFY(!module.save(definition.toVariantMap()).contains("error"));
        CustomModule restored(true);
        restored.restore(module.definitions(), false, false, false);
        QVERIFY(restored.definitions().first().toObject()["leave_running"].toBool());
        auto invalidSensor = definition;
        invalidSensor["mode"] = "sensor";
        QVERIFY(restored.save(invalidSensor.toVariantMap()).contains("error"));
        module.setEnabled(true);
        QSignalSpy finished(&module, &CustomModule::actionFinished);
        module.runAction(definition["id"].toString(), "denied", std::make_shared<std::atomic_bool>(true));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.takeFirst()[1].toBool());
        module.setControlEnabled(true);
        // A global grant must not expose or execute a command without its own opt-in.
        QVERIFY(module.shared(true)["actions"].toArray().isEmpty());
        QCOMPARE(module.shared()["actions"].toArray().size(), 1);
        const auto remote = std::make_shared<std::atomic_bool>(true);
        module.runAction(definition["id"].toString(), "local-only", remote);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.takeFirst()[1].toBool());
        definition["ha_enabled"] = true;
        QVERIFY(!module.save(definition.toVariantMap()).contains("error"));
        QCOMPARE(module.shared(true)["actions"].toArray().size(), 1);
        module.runAction(definition["id"].toString(), "allowed-local-ha", remote);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(finished.takeFirst()[1].toBool());
        module.setCloudEnabled(true);
        module.runAction(definition["id"].toString(), "cloud-not-allowed", remote, true);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.takeFirst()[1].toBool());
        definition["cloud_enabled"] = true;
        QVERIFY(!module.save(definition.toVariantMap()).contains("error"));
        module.runAction(definition["id"].toString(), "allowed-cloud", remote, true);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(finished.takeFirst()[1].toBool());
        module.setCloudEnabled(false);
        module.runAction(definition["id"].toString(), "global-cloud-denied", remote, true);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.takeFirst()[1].toBool());
        auto revoked = std::make_shared<std::atomic_bool>(false);
        module.runAction(definition["id"].toString(), "revoked", revoked);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(!finished.takeFirst()[1].toBool());
        module.runAction(definition["id"].toString(), "local");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(finished.takeFirst()[1].toBool());
        const auto wire = QJsonDocument(module.shared()).toJson();
        QVERIFY(!wire.contains("private-output"));
        QVERIFY(!wire.contains("command\""));
        QVERIFY(!wire.contains("shell"));
        module.test(definition.toVariantMap());
        QTRY_VERIFY_WITH_TIMEOUT(!module.testing(), 5000);
        QVERIFY(module.preview()["ok"].toBool());
        QVERIFY(module.preview()["value"].toString().contains("private-output"));
        QCOMPARE(module.preview()["definition_id"].toString(), definition["id"].toString());
        QCOMPARE(module.preview()["execution"].toMap()["exit_code"].toInt(), 0);
        QVERIFY(module.preview()["execution"].toMap()["stdout"].toString().contains("private-output"));
        // Permission changes cancel an accepted job, including its child process.
        definition["command"] = "ping -n 20 127.0.0.1 > nul";
        QVERIFY(!module.save(definition.toVariantMap()).contains("error"));
        module.runAction(definition["id"].toString(), "permission-revoked", remote);
        QTest::qWait(100);
        module.setControlEnabled(false);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(!finished.takeFirst()[1].toBool());
        module.setEnabled(false);
        QVERIFY(module.shared()["actions"].toArray().isEmpty());
        module.setEnabled(true);
        QCOMPARE(module.shared()["actions"].toArray().size(), 1);
    }

    void customCloudFiltering() {
        PublishingPolicy policy;
        QJsonObject frame{
            {"custom", QJsonObject{{"enabled", true},
                                   {"control_enabled", true},
                                   {"cloud", false},
                                   {"actions", QJsonArray{QJsonObject{{"id", "custom_action_test"}}}}}},
            {"_default_scope", 3},
            {"sensors", QJsonArray{QJsonObject{{"id", "custom_test"},
                                               {"source", "custom"},
                                               {"kind", "text"},
                                               {"value", "private"},
                                               {"available", true}}}}};
        const auto cloud = policy.apply(frame, true, 0);
        QVERIFY(cloud["sensors"].toArray().isEmpty());
        QVERIFY(cloud["custom"].toObject()["actions"].toArray().isEmpty());
        QVERIFY(!cloud["custom"].toObject()["control_enabled"].toBool());
        const auto local = policy.apply(frame, false, 0);
        QCOMPARE(local["sensors"].toArray().size(), 1);
        QCOMPARE(local["custom"].toObject()["actions"].toArray().size(), 1);

        auto custom = frame["custom"].toObject();
        custom["cloud"] = true;
        custom["actions"] = QJsonArray{QJsonObject{{"id", "custom_action_local"}},
                                        QJsonObject{{"id", "custom_action_cloud"}}};
        custom["_cloud_actions"] = QJsonArray{"custom_action_cloud"};
        frame["custom"] = custom;
        const auto permittedCloud = policy.apply(frame, true, 1);
        const auto permittedLocal = policy.apply(frame, false, 1);
        QCOMPARE(permittedCloud["custom"].toObject()["actions"].toArray().size(), 1);
        QCOMPARE(permittedCloud["custom"].toObject()["actions"].toArray().first().toObject()["id"].toString(),
                 "custom_action_cloud");
        QCOMPARE(permittedLocal["custom"].toObject()["actions"].toArray().size(), 2);
        QVERIFY(!permittedCloud["custom"].toObject().contains("_cloud_actions"));
        QVERIFY(!permittedLocal["custom"].toObject().contains("_cloud_actions"));
    }

    void customEditorDraftAndYaml() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        auto *manager = local.customSources;
        QJsonObject output{{"id", manager->newId()}, {"name", "Reading"}, {"path", ""},
                           {"kind", "text"},         {"unit", ""},        {"state_class", ""}};
        QJsonObject definition{{"id", manager->newId()},
                               {"name", "Original"},
                               {"mode", "sensor"},
                               {"source", "script"},
                               {"shell", "cmd"},
                               {"command", "echo value"},
                               {"format", "text"},
                               {"interval", 30},
                               {"timeout", 5},
                               {"enabled", true},
                               {"outputs", QJsonArray{output}}};
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl("qrc:/qml/custom/SensorEditor.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> editor(component.createWithInitialProperties(
            {{"manager", QVariant::fromValue(manager)}, {"definition", definition.toVariantMap()}}));
        QVERIFY2(editor, qPrintable(component.errorString()));
        auto *name = editor->findChild<QObject *>("customName");
        // Repeater delegates belong to the visual tree, not necessarily the
        // QObject ownership tree used by findChild.
        std::function<QQuickItem *(QQuickItem *)> findOutput = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == "customOutputName_0")
                return item;
            for (auto *child : item->childItems())
                if (auto *found = findOutput(child))
                    return found;
            return nullptr;
        };
        auto *outputName = findOutput(qobject_cast<QQuickItem *>(editor.get()));
        QVERIFY(name);
        QVERIFY(outputName);
        name->setProperty("text", "Edited");
        outputName->setProperty("text", "Output Name");
        QVERIFY(QMetaObject::invokeMethod(outputName, "textEdited"));
        QVariant result;
        QVERIFY(QMetaObject::invokeMethod(editor.get(), "draftValue", Q_RETURN_ARG(QVariant, result)));
        const auto draft = result.toMap();
        QCOMPARE(draft["name"].toString(), "Edited");
        QCOMPARE(draft["outputs"].toList().first().toMap()["name"].toString(), "Output Name");
        QVERIFY(!manager->save(draft).contains("error"));
        QCOMPARE(QJsonArray::fromVariantList(local.settings.value("custom/definitions").toList()),
                 manager->definitions());
        QCOMPARE(warnings.count(), 0);
    }

    void customCommandAndSensorForms() {
        QTest::failOnWarning(QRegularExpression("QQmlComponent: Component is not ready"));
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.setAnimationsEnabled(false);
        auto *manager = local.customSources;
        manager->setEnabled(true);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->resize(1100, 760);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 8);
        auto item = [&](const QString &name) { return visualItem(window->contentItem(), name); };
        auto capture = [&](const QString &name) {
            const auto directory = qEnvironmentVariable("WHAT_REDESIGN_PREVIEW");
            if (!directory.isEmpty()) {
                QTest::qWait(100);
                window->grabWindow().save(directory + '/' + name + ".png");
            }
        };
        if (!qEnvironmentVariableIsEmpty("WHAT_REDESIGN_PREVIEW")) {
            // This fixture starts with motion disabled, suitable for page stills.
            window->setProperty("previewPage", 2);
            capture("settings-normalised");
            window->setProperty("previewPage", 5);
            capture("modules-normalised");
            window->setProperty("previewPage", 8);
        }
        QTRY_VERIFY(item("addSavedCommand") && item("addSavedCommand")->isVisible());
        capture("commands-empty");
        QTRY_VERIFY(item("addSavedCommandFooter") && item("addSavedCommandFooter")->isVisible());
        QVERIFY(QMetaObject::invokeMethod(item("addSavedCommandFooter"), "clicked"));
        QTRY_VERIFY(item("commandEditor"));
        auto *dialog = window->findChild<QObject *>("customDefinitionDialog");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QVERIFY(item("commandName"));
        item("commandName")->setProperty("text", "Fixture Command");
        item("customShell")->setProperty("currentIndex", 2); // Command Prompt
        auto *script = item("customScript");
        QVERIFY(script);
        script->forceActiveFocus(Qt::TabFocusReason);
        const QString longScript = QString("echo line\n").repeated(100);
        script->setProperty("text", longScript);
        script->setProperty("cursorPosition", longScript.size());
        auto *formViewport = item("customEditorScroll")->property("contentItem").value<QQuickItem *>();
        QVERIFY(formViewport);
        auto cursorInView = [&] {
            const auto cursor = script->property("cursorRectangle").toRectF();
            const auto point = script->mapToItem(formViewport, cursor.bottomLeft());
            return point.y() >= 0 && point.y() <= formViewport->height() + 1;
        };
        QTRY_VERIFY2(cursorInView(), qPrintable(QString("Cursor %1, script height %2, viewport %3, contentHeight %4, contentY %5, focus %6")
            .arg(script->mapToItem(formViewport, script->property("cursorRectangle").toRectF().bottomLeft()).y())
            .arg(script->height()).arg(formViewport->height()).arg(formViewport->property("contentHeight").toReal())
            .arg(formViewport->property("contentY").toReal()).arg(script->hasActiveFocus())));
        script->setProperty("text", "echo preview-output");
        item("commandName")->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(!item("commandHaPermission")->property("checked").toBool());
        QVERIFY(!item("commandCloudPermission")->isVisible());
        const auto saveButton = item("saveCustomDefinition");
        QVERIFY(saveButton && saveButton->isVisible());
        const auto savePosition = saveButton->mapToScene(QPointF(0, saveButton->height()));
        QVERIFY(savePosition.y() < window->height());
        QVERIFY(QMetaObject::invokeMethod(item("testCustomDefinition"), "clicked"));
        QTRY_VERIFY_WITH_TIMEOUT(!manager->testing(), 5000);
        QVERIFY(manager->preview()["ok"].toBool());
        QVERIFY(manager->preview()["execution"].toMap()["stdout"].toString().contains("preview-output"));
        auto *editorScroll = item("customEditorScroll");
        QVERIFY(editorScroll);
        auto *flick = qvariant_cast<QQuickItem *>(editorScroll->property("contentItem"));
        QVERIFY(flick);
        QTRY_VERIFY(flick->property("contentY").toDouble() > 0);
        capture("command-editor");
        QVERIFY(QMetaObject::invokeMethod(item("saveCustomDefinition"), "clicked"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(manager->commands().size(), 1);
        QCOMPARE(manager->sensors().size(), 0);
        QVERIFY(manager->shared(true)["actions"].toArray().isEmpty());
        const auto commandId = manager->definitions().first().toObject()["id"].toString();
        QTRY_COMPARE(item("savedCommandList")->property("count").toInt(), 1);
        capture("commands-page");

        // Editing a saved command keeps its ID and persists explicit HA grants.
        QTRY_VERIFY(item("commandRow_" + commandId));
        QVERIFY(QMetaObject::invokeMethod(item("commandRow_" + commandId), "clicked"));
        QTRY_VERIFY(item("editSavedCommand_" + commandId));
        QVERIFY(QMetaObject::invokeMethod(item("editSavedCommand_" + commandId), "clicked"));
        QTRY_VERIFY(item("commandEditor"));
        item("commandHaPermission")->setProperty("checked", true);
        QTRY_VERIFY(item("commandCloudPermission")->isVisible());
        item("commandCloudPermission")->setProperty("checked", true);
        QVERIFY(QMetaObject::invokeMethod(item("saveCustomDefinition"), "clicked"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        const auto stored = QJsonArray::fromVariantList(local.settings.value("custom/definitions").toList());
        QCOMPARE(stored.size(), 1);
        QCOMPARE(stored.first().toObject()["id"].toString(), commandId);
        QVERIFY(stored.first().toObject()["ha_enabled"].toBool());
        QVERIFY(stored.first().toObject()["cloud_enabled"].toBool());
        QVERIFY(!stored.first().toObject().contains("result"));

        // The sensor flow starts with a simple app-presence reading, not script fields.
        window->setProperty("previewPage", 1);
        QVERIFY(QMetaObject::invokeMethod(item("sensorsPage"), "customSensorRequested"));
        QTRY_VERIFY(item("sensorEditor"));
        item("customName")->setProperty("text", "Test Runner Active");
        item("customTarget")->setProperty("text", "controller_tests.exe");
        QVERIFY(!item("customScript")->isVisible());
        QVERIFY(QMetaObject::invokeMethod(item("testCustomDefinition"), "clicked"));
        QTRY_VERIFY_WITH_TIMEOUT(!manager->testing(), 5000);
        QVERIFY(manager->preview()["ok"].toBool());
        const auto reading = manager->preview()["readings"].toList().first().toMap();
        QCOMPARE(reading["name"].toString(), "Test Runner Active");
        QCOMPARE(reading["kind"].toString(), "binary");
        QVERIFY(reading["available"].toBool());
        QVERIFY(reading["value"].toBool());
        capture("sensor-editor");
        QVERIFY(QMetaObject::invokeMethod(item("saveCustomDefinition"), "clicked"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(manager->sensors().size(), 1);
        QCOMPARE(manager->commands().size(), 1);

        window->setProperty("previewPage", 8);
        QVERIFY(QMetaObject::invokeMethod(item("runOneOffCommand"), "clicked"));
        QTRY_VERIFY(item("commandEditor"));
        QVERIFY(!item("commandName")->isVisible());
        QVERIFY(!item("commandHaPermission")->isVisible());
        QVERIFY(!item("saveCustomDefinition")->isVisible());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());

        // A row still runs locally when HA is blocked. Inspecting its output or
        // expanding another row must not launch any additional command.
        QVERIFY(QMetaObject::invokeMethod(item("runSavedCommand_" + commandId), "clicked"));
        QTRY_VERIFY_WITH_TIMEOUT(manager->commands().first().toMap()["result"].toMap()["ok"].toBool(), 5000);
        auto *lastRun = item("commandLastRun_" + commandId);
        QVERIFY(lastRun);
        QTRY_VERIFY(lastRun->property("text").toString().endsWith("s ago"));
        auto *page = item("commandsPage");
        QVERIFY(page);
        page->setProperty("now", QDateTime::currentMSecsSinceEpoch() + 125000.0);
        QCOMPARE(lastRun->property("text").toString(), QString("2 min ago"));
        page->setProperty("now", double(QDateTime::currentMSecsSinceEpoch()));
        const QStringList names{"Refresh Backup Index", "Run Network Check", "Write Study Status",
                                "Clean Temporary Exports", "Update Media Library"};
        for (const auto &name : names) {
            auto definition = manager->newDefinition("action");
            definition["name"] = name;
            definition["description"] = "A command saved on this PC, with individual Home Assistant access.";
            definition["command"] = "echo fixture";
            definition["ha_enabled"] = name != names.first();
            definition["cloud_enabled"] = name == names.last();
            QVERIFY(!manager->save(definition).contains("error"));
        }
        manager->setControlEnabled(true);
        manager->setCloudEnabled(true);
        QTRY_COMPARE(item("savedCommandList")->property("count").toInt(), 6);
        QVERIFY(QMetaObject::invokeMethod(item("runSavedCommand_" + commandId), "clicked"));
        QTRY_VERIFY_WITH_TIMEOUT(manager->commands().first().toMap()["result"].toMap()["ok"].toBool(), 5000);
        item("commandsPage")->setProperty("expandedCommand", "");
        capture("commands-page");
        local.setAnimationsEnabled(true);
        QVERIFY(QMetaObject::invokeMethod(item("commandRow_" + commandId), "clicked"));
        QTRY_VERIFY(item("editSavedCommand_" + commandId));
        QVERIFY(QMetaObject::invokeMethod(item("commandOutput_" + commandId), "clicked"));
        QVERIFY(QMetaObject::invokeMethod(item("runSavedCommand_" + commandId), "clicked"));
        QTRY_VERIFY_WITH_TIMEOUT(!manager->commands().first().toMap()["running"].toBool(), 5000);
        QTRY_VERIFY(item("commandOutput_" + commandId)
                    && item("commandOutput_" + commandId)->property("checked").toBool());
        capture("command-expanded");
        const auto nextId = manager->commands()[1].toMap()["id"].toString();
        QVERIFY(QMetaObject::invokeMethod(item("commandRow_" + nextId), "clicked"));
        QTRY_VERIFY(!item("editSavedCommand_" + commandId));
        QTRY_VERIFY(item("editSavedCommand_" + nextId));
        QVERIFY(manager->commands()[1].toMap()["result"].toMap().isEmpty());
        window->resize(1600, 950);
        capture("commands-page-wide");
        QVERIFY(QMetaObject::invokeMethod(item("commandRow_" + nextId), "clicked"));
        QTRY_VERIFY(!item("editSavedCommand_" + nextId));
        window->resize(1000, 700);
        auto *permissions = item("Home Assistant PermissionsSectionToggle");
        QVERIFY(permissions);
        QVERIFY(QMetaObject::invokeMethod(permissions, "clicked"));
        QTest::qWait(300); // Let the view settle the expanded header's scroll extent.
        auto *list = item("savedCommandList");
        QVERIFY(list);
        QTRY_VERIFY(permissions->mapToScene(QPointF()).y() >= list->mapToScene(QPointF()).y());
        capture("command-permissions");
        auto *version = item("sidebarVersion");
        QVERIFY(version && version->isVisible());
        QVERIFY(version->mapToScene(QPointF(0, version->height())).y() < window->height());
        QVERIFY(list->mapToScene(QPointF(0, list->height())).y() < window->height());
        QCOMPARE(warnings.count(), 0);
    }

    void announcementsAndRichNotificationValidation() {
        QVERIFY(Announcements::validate("speak", {{"message", "Fixture"}, {"rate", 1.2}}).isEmpty());
        QVERIFY(!Announcements::validate("speak", {{"message", "Fixture"}, {"rate", 99}}).isEmpty());
        QVERIFY(!Announcements::validate("play_audio", {{"url", "file:///C:/private.wav"}}).isEmpty());
        QVERIFY(!Announcements::validate("play_audio", {{"url", "https://user:password@example.com/audio"}})
                     .isEmpty());
        QVERIFY(Announcements::validate("play_audio", {{"url", "https://example.com/audio.wav"}}).isEmpty());
        Announcements player(true);
        QSignalSpy completed(&player, &Announcements::completed);
        player.execute("speak", {{"message", "Fixture"}}, "denied");
        QCOMPARE(completed.size(), 1);
        QVERIFY(!completed.takeFirst()[1].toBool());
        player.setPermitted(true);
        player.execute("speak", {{"message", "Fixture"}}, "revoked",
                       std::make_shared<std::atomic_bool>(false));
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(!completed.takeFirst()[1].toBool());
        QJsonObject args{{"title", "A <title>"},
                         {"message", "A & message"},
                         {"reply", true},
                         {"actions", QJsonArray{QJsonObject{{"id", "confirm"}, {"title", "Confirm"}}}}};
        QVERIFY(RichNotifications::validate(args).isEmpty());
        args["actions"] =
            QJsonArray{QJsonObject{{"id", "confirm"}, {"title", "Open"}, {"url", "file:///private"}}};
        QVERIFY(!RichNotifications::validate(args).isEmpty());
        args["actions"] = QJsonArray{};
        QVERIFY(!RichNotifications::validate(args).isEmpty());
        args["reply"] = false;
        args["ttl"] = 1.5;
        QVERIFY(!RichNotifications::validate(args).isEmpty());
        args["ttl"] = 60;
        RichNotifications notifications(true);
        QSignalSpy notified(&notifications, &RichNotifications::completed);
        notifications.send(args, "denied", std::make_shared<std::atomic_bool>(true));
        QCOMPARE(notified.size(), 1);
        QVERIFY(!notified.takeFirst()[1].toBool());
        notifications.setPermitted(true);
        notifications.send(args, "accepted", std::make_shared<std::atomic_bool>(true));
        QTRY_COMPARE(notified.size(), 1);
        QVERIFY(notified.takeFirst()[1].toBool());
    }

    void nativeSpeechAtZeroVolume() {
        if (!qEnvironmentVariableIsSet("WHAT_TEST_NATIVE_SPEECH"))
            QSKIP("Enable WHAT_TEST_NATIVE_SPEECH to check the local Windows speech provider silently");
        Announcements player(false);
        player.setPermitted(true);
        QTRY_VERIFY_WITH_TIMEOUT(!player.voices().isEmpty() || !player.state()["error"].toString().isEmpty(),
                                 10000);
        QVERIFY2(!player.voices().isEmpty(), qPrintable(player.state()["error"].toString()));
        QSignalSpy completed(&player, &Announcements::completed);
        player.execute("speak", {{"message", "WHAT speech test"}, {"volume", 0.0}}, "native");
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 15000);
        const auto result = completed.takeFirst();
        QVERIFY2(result[1].toBool(), qPrintable(result[2].toString()));
        QTRY_COMPARE_WITH_TIMEOUT(player.state()["volume"].toDouble(), 0.0, 5000);
        QVERIFY2(player.state()["error"].toString().isEmpty(),
                 qPrintable(player.state()["error"].toString()));
        player.setPermitted(false);
        QVERIFY(!player.state()["enabled"].toBool());
    }

    void appProcessDiagnostics() {
        QCOMPARE(AppUsage::cpuPercent(10000000, 1000000000, 4), 25.0);
        QCOMPARE(AppUsage::cpuPercent(80000000, 1000000000, 4), 100.0);
        QCOMPARE(AppUsage::cpuPercent(10000000, 0, 4), 0.0);
        AppUsage usage;
        auto first = usage.sample();
        QVERIFY(first["cpu_percent"].isNull());
        QVERIFY(first["memory_bytes"].toDouble() > 0);
        QTest::qWait(20);
        const double cpu = usage.sample()["cpu_percent"].toDouble(-1);
        QVERIFY(cpu >= 0 && cpu <= 100);
        QVERIFY(!AppRestart::waitForPrevious(-1, 0));
        QVERIFY(!AppRestart::waitForPrevious(QCoreApplication::applicationPid(), 0));
    }

    void beacnNativeSourceSupport() {
        for (int source : {0, 1, 2, 3, 4, 7})
            QVERIFY(BeacnBackend::nativeSourceType(source) >= 0);
        for (int source : {-1, 5, 6, 8})
            QCOMPARE(BeacnBackend::nativeSourceType(source), -1);
    }

    void beacnValidationAndPermissionIsolation() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        const QVariantList controls{
            QVariantMap{
                {"id", "volume_2"}, {"kind", "number"}, {"available", true}, {"min", 0}, {"max", 100}},
            QVariantMap{
                {"id", "dim_timeout"}, {"kind", "number"}, {"available", true}, {"min", 1}, {"max", 86400}},
            QVariantMap{{"id", "mute_2"}, {"kind", "switch"}, {"available", true}},
            QVariantMap{{"id", "profile"},
                        {"kind", "select"},
                        {"available", true},
                        {"options", QStringList{"Studio", "Gaming"}}},
            QVariantMap{{"id", "next_page"}, {"kind", "button"}, {"available", false}}};
        QVERIFY(BeacnModule::validate({{"control", "volume_2"}, {"value", 35.5}}, controls).isEmpty());
        QVERIFY(!BeacnModule::validate({{"control", "volume_2"}, {"value", 101}}, controls).isEmpty());
        QVERIFY(!BeacnModule::validate({{"control", "volume_2"}, {"value", true}}, controls).isEmpty());
        QVERIFY(!BeacnModule::validate({{"control", "dim_timeout"}, {"value", 1.5}}, controls).isEmpty());
        QVERIFY(BeacnModule::validate({{"control", "mute_2"}, {"value", false}}, controls).isEmpty());
        QVERIFY(!BeacnModule::validate({{"control", "mute_2"}, {"value", 0}}, controls).isEmpty());
        QVERIFY(BeacnModule::validate({{"control", "profile"}, {"value", "Gaming"}}, controls).isEmpty());
        QVERIFY(!BeacnModule::validate({{"control", "profile"}, {"value", "Deleted"}}, controls).isEmpty());
        QVERIFY(!BeacnModule::validate({{"control", "next_page"}}, controls).isEmpty());
        QVERIFY(!BeacnModule::validate({{"control", "raw_memory"}}, controls).isEmpty());
        QVERIFY(!BeacnModule::validate({{"control", "mute_2"}, {"value", true}, {"address", 123}}, controls)
                     .isEmpty());
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        QVERIFY(!local.moduleEnabled("beacn"));
        const bool windows = local.moduleEnabled("windows");
        local.saveBeacn(true, false, false);
        QVERIFY(local.moduleEnabled("beacn"));
        QCOMPARE(local.moduleEnabled("windows"), windows);
        local.handleBeacnCommand(
            "remote", {{"id", "fixture"}, {"args", QJsonObject{{"control", "mute_2"}, {"value", true}}}});
        QVERIFY(local.beacnState()["last_command"].toString().contains("permission"));
        local.saveBeacn(true, true, false);
        QVERIFY(!local.beacnState()["cloud"].toBool());
        local.beacnCommand({{"control", "mute_2"}, {"value", true}});
        QVERIFY(local.beacnState()["last_command"].toString().contains("test mode"));
        local.setModuleEnabled("beacn", false);
        QVERIFY(!local.beacnState()["ready"].toBool());
        QVERIFY(local.beacnState()["controls"].toList().isEmpty());
    }

    void beacnWorkerCancellationAndShutdown() {
        auto module = std::make_unique<BeacnModule>();
        auto epoch = std::make_shared<std::atomic_uint64_t>(2);
        auto lease = std::make_shared<std::atomic_bool>(false);
        bool finished = false;
        bool succeeded = true;
        module->command({{"control", "mute_2"}, {"value", true}}, lease, epoch, 1,
                        [&](bool ok, QString reason) {
                            succeeded = ok;
                            finished = true;
                            QVERIFY(reason.contains("cancelled"));
                        });
        QVERIFY(module->busy());
        bool rejected = false;
        module->command({}, lease, epoch, 1,
                        [&](bool ok, QString reason) { rejected = !ok && reason.contains("busy"); });
        QVERIFY(rejected);
        QTRY_VERIFY(finished);
        QVERIFY(!succeeded);
        QVERIFY(!module->busy());
        module->configure(false, {}, {});
        module->configure(false, {}, {});
        module.reset(); // No orphan worker or late callback after shutdown.
    }

    void beacnStalledWorkerShutdownIsBounded() {
        auto module = std::make_unique<BeacnModule>();
        QPointer<QThread> thread = module->thread;
        auto context = new QObject;
        context->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, context, &QObject::deleteLater);
        auto entered = std::make_shared<QSemaphore>();
        auto resume = std::make_shared<QSemaphore>();
        auto release = qScopeGuard([resume] { resume->release(); });
        QMetaObject::invokeMethod(
            context,
            [entered, resume] {
                entered->release();
                resume->acquire();
            },
            Qt::QueuedConnection);
        QVERIFY(entered->tryAcquire(1, 3000));
        bool replied = false;
        module->command({}, std::make_shared<std::atomic_bool>(true),
                        std::make_shared<std::atomic_uint64_t>(0), 0, [&](bool, QString) { replied = true; });
        QElapsedTimer elapsed;
        elapsed.start();
        module.reset();
        QVERIFY(elapsed.elapsed() < 1000);
        QVERIFY(thread && thread->isRunning());
        resume->release();
        QTRY_VERIFY(thread.isNull());
        QVERIFY(!replied);
    }

    void icueAutomationCommandsBypassStalledSdk() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        auto entered = std::make_shared<QSemaphore>();
        auto resume = std::make_shared<QSemaphore>();
        auto release = qScopeGuard([resume] { resume->release(); });
        QTRY_VERIFY(!local.icueJob.isRunning());
        local.icueJob.submit(
            [entered, resume] {
                entered->release();
                resume->acquire();
                return true;
            },
            [](bool) {});
        QVERIFY(entered->tryAcquire(1, 3000));
        QVERIFY(!local.icueCommandBusy("icue_automation", {{"control", "preset"}}));
        QVERIFY(local.icueCommandBusy("icue_automation", {{"control", "profile"}}));
        bool replied = false;
        local.submitIcueCommand(
            "icue_automation", {{"control", "preset"}}, [] { return false; },
            [&](QString error) {
                QVERIFY(error.isEmpty());
                replied = true;
            });
        QTRY_VERIFY(replied);
        QVERIFY(local.icueJob.isRunning());
        QVERIFY(!local.icueAutomationPending);
        resume->release();
        QTRY_VERIFY(!local.icueJob.isRunning());
    }

    void beacnHelperStagingVerifiesAndRepairs() {
        QTemporaryDir dir;
        const auto source = dir.path() + "/bundled.dll";
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes("fixture bridge bytes");
        file.write(bytes);
        file.close();
        QString error;
        const auto staged = prepareBeacnHook(source, dir.path() + "/cache", &error);
        QVERIFY2(!staged.isEmpty(), qPrintable(error));
        QVERIFY(staged != source);
        QFile cached(staged);
        QVERIFY(cached.open(QIODevice::WriteOnly));
        cached.write("corrupt");
        cached.close();
        QCOMPARE(prepareBeacnHook(source, dir.path() + "/cache", &error), staged);
        QVERIFY(cached.open(QIODevice::ReadOnly));
        QCOMPARE(cached.readAll(), bytes);
        cached.close();
        QVERIFY(prepareBeacnHook(dir.path() + "/missing", dir.path() + "/cache", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void persistedModulePermissionsAreBooleans() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        QVERIFY(!local.snapshot().value("status").toString().isEmpty());
        local.settings.setValue("modules/wallpaper/enabled", true);
        local.settings.setValue("modules/beacn/enabled", true);
        for (auto module : {"wallpaper", "beacn"})
            for (auto flag : {"control", "cloud", "share_path"})
                local.settings.setValue(QString("modules/%1/%2").arg(module, flag), QString("false"));
        for (auto state : {local.wallpaperShared(), local.beacnShared()}) {
            QVERIFY(state["control_enabled"].isBool());
            QVERIFY(!state["control_enabled"].toBool());
            QVERIFY(state["cloud"].isBool());
            QVERIFY(!state["cloud"].toBool());
        }
        local.settings.setValue("modules/wallpaper/control", QString("true"));
        local.settings.setValue("modules/beacn/control", QString("true"));
        QVERIFY(local.wallpaperShared()["control_enabled"].toBool());
        QVERIFY(local.beacnShared()["control_enabled"].toBool());
        QVERIFY(local.wallpaperShared()["share_path"].isBool());
        local.settings.setValue("modules/wallpaper/enabled", false);
        const auto disabled = local.wallpaperShared();
        for (auto key : {"enabled", "running", "installed", "control_enabled", "cloud", "share_path", "busy"})
            QVERIFY2(!disabled.contains(key) || disabled.value(key).isBool(), key);
        QVERIFY(!disabled.contains("current_wallpaper"));
    }

    void beacnBrowserReadingsAndSharing() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.setModuleEnabled("windows", false);
        local.setModuleEnabled("hwinfo", false);
        local.saveBeacn(true, true, false);
        const QVariantMap state{{"ready", true},
                                {"connected", true},
                                {"sample_rate", 48000},
                                {"exe_path", "C:/BEACN/BEACN.exe"},
                                {"dll_path", "C:/BEACN/API.dll"},
                                {"channels", QVariantList{QVariantMap{{"id", 2},
                                                                      {"name", "Game"},
                                                                      {"valid", true},
                                                                      {"linked", true},
                                                                      {"personalPercent", 35.0},
                                                                      {"audiencePercent", 35.0},
                                                                      {"personalMuted", false},
                                                                      {"audienceMuted", true}}}},
                                {"controls", QVariantList{QVariantMap{{"id", "volume_2"},
                                                                      {"name", "Game linked volume"},
                                                                      {"kind", "number"},
                                                                      {"available", true},
                                                                      {"value", 35.0},
                                                                      {"min", 0},
                                                                      {"max", 100}}}}};
        local.acceptBeacn(state);
        local.setBrowserFilters("", "beacn", false, false);
        bool reading = false, control = false;
        for (auto v : local.browser()) {
            const auto r = v.toMap();
            if (r["header"].toBool()) {
                QVERIFY(r["name"].toString().startsWith("BEACN"));
                continue;
            }
            QCOMPARE(r["source"].toString(), QString("beacn"));
            if (r["id"] == "beacn_2_personal_volume") {
                reading = true;
                QVERIFY(r["available"].toBool());
                QCOMPARE(r["value"].toDouble(), 35.0);
                QVERIFY(!r["enabled"].toBool());
            }
            if (r["control"] == "beacn") {
                control = true;
                QVERIFY(r["controlOnly"].toBool());
            }
        }
        QVERIFY(reading);
        QVERIFY(control);
        QVERIFY(local.snapshot()["sensors"].toArray().isEmpty());
        local.setSensorEnabled("beacn_2_personal_volume", true);
        auto frame = local.snapshot();
        QCOMPARE(frame["sensors"].toArray().size(), 1);
        QVERIFY(!frame["beacn"].toObject().contains("exe_path"));
        QVERIFY(!frame["beacn"].toObject().contains("channels"));
        local.settings.setValue("publishing/beacn_2_personal_volume", QVariantMap{{"scope", 3}});
        local.refreshPublishingPolicies();
        local.publishPolicy();
        PublishingPolicy policy;
        QVERIFY(policy.apply(local.snapshot(), true, 0)["sensors"].toArray().isEmpty());
        local.saveBeacn(true, true, true);
        QCOMPARE(policy.apply(local.snapshot(), true, 1)["sensors"].toArray().size(), 1);
        local.setFavourite("beacn_control_volume_2", true);
        local.setBrowserFilters("", "beacn", false, true);
        QCOMPARE(local.browser().size(), 2);
        local.setBrowserFilters("", "beacn", false, false);
        local.collapseCategories(true);
        for (auto v : local.browser())
            QVERIFY(v.toMap()["header"].toBool());
        local.collapseCategories(false);
        local.acceptBeacn({{"ready", false}});
        QVERIFY(!local.snapshot()["sensors"].toArray().first().toObject()["available"].toBool());
        local.saveBeacnPaths(" C:/chosen/BEACN.exe ", " C:/chosen/driver.dll ");
        QCOMPARE(local.beacnState()["exe_override"].toString(), QString("C:/chosen/BEACN.exe"));
        local.saveBeacnPaths("", "");
        QVERIFY(local.beacnState()["exe_override"].toString().isEmpty());
        local.setModuleEnabled("beacn", false);
        QVERIFY(local.browser().isEmpty());
        QVERIFY(local.snapshot()["sensors"].toArray().isEmpty());
    }

    void qmlBeacnLiveControls() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.setModuleEnabled("windows", false);
        local.setModuleEnabled("hwinfo", false);
        local.saveBeacn(true, false, false);
        QVariantMap state{
            {"ready", true},
            {"connected", true},
            {"status", "Connected"},
            {"exe_path", "C:/Program Files/BEACN/BEACN App/BEACN.exe"},
            {"dll_path", "C:/Program Files/BEACN/Mic Device Driver/x64/BEACNUsbAudioapi_x64.dll"},
            {"installation", QVariantMap{{"status", "Verified installed BEACN 1.4.2.12 and x64 driver API"}}},
            {"global", QVariantMap{{"autoResetDefaults", true}}},
            {"controls", QVariantList{QVariantMap{{"id", "volume_2"},
                                                  {"name", "Game linked volume"},
                                                  {"kind", "number"},
                                                  {"value", 35.0},
                                                  {"available", true},
                                                  {"min", 0},
                                                  {"max", 100}},
                                      QVariantMap{{"id", "mute_2"},
                                                  {"name", "Game knob mute"},
                                                  {"kind", "switch"},
                                                  {"value", false},
                                                  {"available", true}},
                                      QVariantMap{{"id", "recording_default"},
                                                  {"name", "Recording default assignment"},
                                                  {"kind", "select"},
                                                  {"value", "Mic"},
                                                  {"available", true},
                                                  {"options", QStringList{"Mic", "Other"}}},
                                      QVariantMap{{"id", "next_page"},
                                                  {"name", "Next knob page"},
                                                  {"kind", "button"},
                                                  {"available", true}}}}};
        local.acceptBeacn(state);
        local.setBrowserFilters("beacn controls", "beacn", false, false);
        local.setWindowAnimationsEnabled(
            false); // This fixture tests controls; window motion has its own test.
        QStringList bindingWarnings;
        QQmlApplicationEngine engine;
        connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &warnings) {
            for (const auto &warning : warnings)
                if (warning.description().contains("Binding loop"))
                    bindingWarnings.append(warning.toString());
        });
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->setProperty("previewPage", 5);
        window->setPosition(-32000, -32000);
        window->show();
        std::function<QQuickItem *(QQuickItem *, QString)> find = [&](QQuickItem *p,
                                                                      QString name) -> QQuickItem * {
            if (p->objectName() == name)
                return p;
            for (auto *child : p->childItems())
                if (auto *v = find(child, name))
                    return v;
            return nullptr;
        };
        auto item = [&](QString name) { return find(window->contentItem(), name); };
        auto *search = item("moduleSearch");
        QVERIFY(search);
        search->setProperty("text", "beacn");
        QVERIFY(item("beacnExecutablePath"));
        QCOMPARE(item("beacnExecutablePath")->property("text").toString(), state["exe_path"].toString());
        QCOMPARE(item("beacnDriverPath")->property("text").toString(), state["dll_path"].toString());
        QCOMPARE(item("beacnConnectionStatus")->property("text").toString(), QString("Connected"));
        QVERIFY(!item("beacnControl_volume_2") || !item("beacnControl_volume_2")->isVisible());
        if (!qEnvironmentVariableIsEmpty("WHAT_BEACN_MODULE_PREVIEW")) {
            QTest::qWait(250);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_BEACN_MODULE_PREVIEW")));
        }
        window->setProperty("previewPage", 1);
        QTRY_VERIFY(item("beacnFilter") && item("beacnFilter")->isVisible());

        QTRY_VERIFY(openBrowserControl(window, local, "beacn", "id", "beacn_control_volume_2"));
        QTRY_VERIFY(item("beacnControl_volume_2") && item("beacnControl_volume_2")->isVisible());
        auto *volume = item("beacnControl_volume_2");
        QVERIFY(volume->isEnabled());
        auto controls = state["controls"].toList();
        auto row = controls[0].toMap();
        row["value"] = 42;
        controls[0] = row;
        state["controls"] = controls;
        local.acceptBeacn(state);
        QTest::qWait(350);
        QCOMPARE(item("beacnControl_volume_2"), volume);
        QVERIFY(volume->isEnabled());
        if (!qEnvironmentVariableIsEmpty("WHAT_BEACN_PREVIEW")) {
            QTest::qWait(200);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_BEACN_PREVIEW")));
        }
        state["ready"] = false;
        state["status"] = "BEACN device disconnected";
        local.acceptBeacn(state);
        QTRY_VERIFY(!volume->isEnabled());
        QTRY_COMPARE(item("beacnConnectionStatus")->property("text").toString(), QString("Needs Attention"));
        local.setModuleEnabled("beacn", false);
        QTRY_VERIFY(!item("beacnFilter")->isVisible());
        QTRY_COMPARE(item("beacnConnectionStatus")->property("text").toString(), QString("Disabled"));
        QVERIFY2(bindingWarnings.isEmpty(), qPrintable(bindingWarnings.join('\n')));
    }

    void beacnCloudRedaction() {
        PublishingPolicy policy;
        QJsonObject frame{{"beacn", QJsonObject{{"enabled", true},
                                                {"ready", true},
                                                {"cloud", false},
                                                {"control_enabled", true},
                                                {"global", QJsonObject{{"profile", "Private profile"}}}}}};
        auto cloud = policy.apply(frame, true, 0)["beacn"].toObject();
        QVERIFY(!cloud["enabled"].toBool());
        QVERIFY(!cloud.contains("global"));
        QCOMPARE(policy.apply(frame, false, 1)["beacn"].toObject()["global"].toObject()["profile"].toString(),
                 QString("Private profile"));
        auto allowed = frame["beacn"].toObject();
        allowed["cloud"] = true;
        frame["beacn"] = allowed;
        QVERIFY(policy.apply(frame, true, 2)["beacn"].toObject()["enabled"].toBool());
    }

    void configurationRoundTripAndPrivacy() {
        QTemporaryDir dir;
        QSettings profile(dir.filePath("settings.yaml"), SettingsStore::format());
        profile.setValue("device_id", "private-device");
        profile.setValue("token", "private-token");
        profile.setValue("mqtt/password", QByteArray("private-password"));
        profile.setValue("catalog", "private-readings");
        profile.setValue("modules/wallpaper/path", "private-executable");
        profile.setValue("ui/animations", true);
        profile.setValue("ui/timing/logo", 1700);
        profile.setValue("sources/wifi", false);
        const QString id = QString::fromUtf8("sensor/CPU \"温度\"\\test");
        profile.setValue("selected", QStringList{id});
        profile.setValue("publishing/" + id, QVariantMap{{"scope", 3}, {"high", true}, {"decimals", -1}});
        profile.setValue("mqtt/options", QVariantMap{{"host", "broker.example"},
                                                     {"port", 8883},
                                                     {"tls", true},
                                                     {"scope", "cloud"},
                                                     {"username", "user"},
                                                     {"key", "private-key-path"},
                                                     {"password", "unexpected-secret"}});
        const auto snapshot = ConfigurationFile::snapshot(profile);
        const auto yaml = ConfigurationFile::encode(snapshot);
        QVERIFY(!yaml.contains("private-"));
        QVERIFY(!yaml.contains("unexpected-secret"));
        const auto decoded = ConfigurationFile::decode(yaml);
        QVERIFY2(decoded.error.isEmpty(), qPrintable(decoded.error));
        QCOMPARE(decoded.values, snapshot);
        QCOMPARE(decoded.values["selected"].toArray()[0].toString(), id);
        QCOMPARE(decoded.values["mqtt/options"].toObject().size(), 5);
        QCOMPARE(decoded.values["sources/wifi"], QJsonValue(false));
    }

    void configurationRejectsUnsafeOrInvalidInput() {
        for (const auto &values : QList<QJsonObject>{
                 {{"token", "injected"}},
                 {{"ui/animations", "true"}},
                 {{"port", 80}},
                 {{"remote/url", "https://user:password@example.com"}},
                 {{"ui/timing/logo", -1}},
                 {{"modules/wallpaper/path", "C:/untrusted.exe"}},
                 {{"publishing/id", QJsonObject{{"scope", 9}, {"high", true}, {"decimals", -1}}}}}) {
            QVERIFY(!ConfigurationFile::decode(ConfigurationFile::encode(values)).error.isEmpty());
        }
        const auto yaml = ConfigurationFile::encode({{"ui/animations", true}});
        QVERIFY(!ConfigurationFile::decode(yaml + "  \"ui/animations\": false\n").error.isEmpty());
        QVERIFY(!ConfigurationFile::decode(QByteArray(yaml).replace("version: 1", "version: 99"))
                     .error.isEmpty());
        QVERIFY(!ConfigurationFile::decode(yaml + "  \"browser/query\": !!python/object:evil {}\n")
                     .error.isEmpty());
        QVERIFY(
            !ConfigurationFile::decode(QByteArray(ConfigurationFile::maximumBytes + 1, ' ')).error.isEmpty());
        QVERIFY(ConfigurationFile::permission("controls/shell"));
        QVERIFY(ConfigurationFile::permission("selected"));
        QVERIFY(!ConfigurationFile::permission("ui/timing/logo"));
    }

    void configurationAppliesOnRestartAndPreservesIdentity() {
        QTemporaryDir dir;
        QSettings profile(dir.filePath("settings.yaml"), SettingsStore::format());
        profile.setValue("device_id", "keep-device");
        profile.setValue("token", QByteArray("keep-token"));
        profile.setValue("controls/shell", false);
        profile.setValue("ui/animations", false);
        profile.setValue("mqtt/password", QByteArray("old-broker-secret"));
        profile.sync();
        const QJsonObject values{{"ui/animations", true},
                                 {"selected", QJsonArray{"cpu"}},
                                 {"remote/url", "https://ha.example"},
                                 {"remote/enabled", true},
                                 {"mqtt/options", QJsonObject{{"host", "new-broker"},
                                                              {"port", 8883},
                                                              {"tls", true},
                                                              {"scope", "cloud"},
                                                              {"username", "u"}}}};
        QVERIFY(
            ConfigurationFile::write(profile.fileName() + ".import.yaml", ConfigurationFile::encode(values))
                .isEmpty());
        QVERIFY(!profile.value("ui/animations").toBool());
        const auto message = ConfigurationFile::applyPending(profile);
        QVERIFY2(message.startsWith("Configuration imported."), qPrintable(message));
        QCOMPARE(profile.value("device_id").toString(), QString("keep-device"));
        QCOMPARE(profile.value("token").toByteArray(), QByteArray("keep-token"));
        QVERIFY(profile.value("ui/animations").toBool());
        QVERIFY(!profile.value("controls/shell").toBool());
        QVERIFY(!profile.value("remote/enabled").toBool());
        QVERIFY(!profile.contains("mqtt/password"));
        QVERIFY(profile.value("mqtt/paused").toBool());
        QCOMPARE(profile.value("remote/endpoint").toString(), QString("wss://ha.example/api/what/connect"));
        QCOMPARE(profile.value("selected").toStringList(), QStringList{"cpu"});
        QVERIFY(QFile::exists(profile.fileName() + ".before-import"));
        QVERIFY(!QFile::exists(profile.fileName() + ".import.yaml"));
        QVERIFY(ConfigurationFile::write(profile.fileName() + ".import.yaml", "invalid").isEmpty());
        QVERIFY(ConfigurationFile::applyPending(profile).startsWith("Configuration import rejected"));
        QCOMPARE(profile.value("device_id").toString(), QString("keep-device"));
    }

    void configurationPermissionsRequireOptIn() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.configurationDraft = {
            {"ui/animations", false}, {"controls/shell", true}, {"controls/what", true},
            {"selected", QJsonArray{"cpu"}}};
        QSignalSpy finished(&local, &Controller::configurationResult);
        local.stageConfiguration(false);
        QTRY_COMPARE(finished.count(), 1);
        const auto result = ConfigurationFile::read(dir.filePath("test-settings.yaml.import.yaml"));
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(!result.values.contains("controls/shell"));
        QVERIFY(!result.values.contains("controls/what"));
        QVERIFY(!result.values.contains("selected"));
        QCOMPARE(result.values["ui/animations"], QJsonValue(false));
        local.cancelConfigurationImport();
        QVERIFY(!QFile::exists(dir.filePath("test-settings.yaml.import.yaml")));
    }

    void qmlUpdatePromptAppearance() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.setLicense("pro");
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        updater.available = "0.8.0-beta.1";
        emit updater.changed();
        emit updater.updateAvailable("0.8.0-beta.1");
        auto *dialog = window->findChild<QObject *>("updateAvailablePrompt");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(dialog->property("title").toString(), QString("Update Available"));
        auto *promptLogo = window->findChild<QQuickItem *>("updatePromptLogo");
        QVERIFY(promptLogo);
        QTest::mouseMove(
            window,
            promptLogo->mapToScene(QPointF(promptLogo->width() / 2, promptLogo->height() / 2)).toPoint());
        QTRY_VERIFY(promptLogo->property("animating").toBool());
        local.setAnimationsEnabled(false);
        QTRY_VERIFY(!promptLogo->property("animating").toBool());
        local.setAnimationsEnabled(true);
        QCOMPARE(dialog->property("version").toString(), QString("0.8.0-beta.1"));
        std::function<QQuickItem *(QQuickItem *)> findBackdrop = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == "glassBackdrop")
                return item;
            for (auto *child : item->childItems())
                if (auto *found = findBackdrop(child))
                    return found;
            return nullptr;
        };
        auto *backdrop = findBackdrop(window->contentItem());
        QVERIFY(backdrop);
        auto background = backdrop->grabToImage();
        QVERIFY(background);
        QSignalSpy imageReady(background.data(), &QQuickItemGrabResult::ready);
        QTRY_VERIFY(!background->image().isNull());
        // The square backdrop covers the window corners as well as its centre.
        QCOMPARE(background->image().pixelColor(0, 0).alpha(), 255);
        QCOMPARE(background->image().pixelColor(background->image().width() - 1, 0).alpha(), 255);
        local.setAnimationsEnabled(false);
        const auto folder = qEnvironmentVariable("WHAT_PROMPT_PREVIEW");
        if (!folder.isEmpty()) {
            QTest::qWait(350);
            QVERIFY(window->grabWindow().save(folder + "/update-dialog.png"));
        }
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("opened").toBool());
        auto *install = window->findChild<QObject *>("installUpdatePrompt");
        QVERIFY(install);
        QVERIFY(QMetaObject::invokeMethod(install, "open"));
        QTRY_VERIFY(install->property("opened").toBool());
        if (!folder.isEmpty()) {
            QTest::qWait(350);
            QVERIFY(window->grabWindow().save(folder + "/install-dialog.png"));
        }
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!install->property("opened").toBool());
        window->hide();
    }

    void qmlConfigurationPreview() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.setLicense("pro");
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->setProperty("previewPage", 2);
        auto *appPermission = window->findChild<QObject *>("appControlPermission");
        QVERIFY(appPermission);
        QVERIFY(!appPermission->property("checked").toBool());
        appPermission->setProperty("checked", true);
        QVERIFY(QMetaObject::invokeMethod(appPermission, "toggled"));
        QVERIFY(local.appState()["control_enabled"].toBool());
        local.setAppControlEnabled(false);
        QTRY_VERIFY(!appPermission->property("checked").toBool());
        QVERIFY(window->findChild<QObject *>("exportConfiguration"));
        QVERIFY(window->findChild<QObject *>("importConfiguration"));
        auto *dialog = window->findChild<QObject *>("configurationImportDialog");
        auto *permissions = window->findChild<QObject *>("configurationImportPermissions");
        QVERIFY(dialog);
        QVERIFY(permissions);
        permissions->setProperty("checked", true);
        emit local.configurationPreview(QString::fromUtf8(
            ConfigurationFile::encode({{"ui/timing/logo", 1500}, {"controls/shell", false}})));
        QTRY_VERIFY(dialog->property("visible").toBool());
        QVERIFY(!permissions->property("checked").toBool());
        if (!qEnvironmentVariableIsEmpty("WHAT_CONFIG_PREVIEW")) {
            window->show();
            QVERIFY(QTest::qWaitForWindowExposed(window));
            QTest::qWait(250);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_CONFIG_PREVIEW")));
        }
        QMetaObject::invokeMethod(dialog, "close");
        window->hide();
    }

    void dynamicLightingShutdown() {
        QTest::failOnWarning(QRegularExpression("QProcess: Destroyed while process.*"));
        for (const auto *mode : {"--helper-exit-on-eof", "--helper-ignore-eof"}) {
            auto lighting = std::make_unique<DynamicLighting>();
            auto *process = new QProcess(lighting.get());
            lighting->process = process;
            process->setCreateProcessArgumentsModifier(
                [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
            process->start(QCoreApplication::applicationFilePath(), {QString::fromLatin1(mode)});
            QVERIFY(process->waitForStarted());
            QTRY_VERIFY(process->bytesAvailable() >= 5);
            const auto ready = process->readAllStandardOutput();
            QCOMPARE(ready, QByteArray("ready"));
            HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, DWORD(process->processId()));
            QVERIFY(child);
            const auto closeHandle = qScopeGuard([child] { CloseHandle(child); });
            QSignalSpy finished(process, &QProcess::finished);
            QElapsedTimer elapsed;
            elapsed.start();
            // No event processing here: this also covers destruction after app.exec().
            lighting.reset();
            QCOMPARE(WaitForSingleObject(child, 0), DWORD(WAIT_OBJECT_0));
            QVERIFY(elapsed.elapsed() < 2000);
            QCOMPARE(finished.size(), 1);
            if (QString::fromLatin1(mode) == "--helper-exit-on-eof") {
                QCOMPARE(finished.first()[0].toInt(), 0);
                QCOMPARE(finished.first()[1].value<QProcess::ExitStatus>(), QProcess::NormalExit);
            }
        }
    }

    void dynamicLightingPermissionsAndValidation() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        QVERIFY(!local.dynamicLightingState()["enabled"].toBool());
        QVERIFY(!local.dynamicLightingState()["control_enabled"].toBool());
        QVERIFY(!local.dynamicLightingState()["cloud"].toBool());
        local.setDynamicLightingOption("enabled", true);
        local.setDynamicLightingOption("control", true);
        QVERIFY(local.dynamicLightingState()["control_enabled"].toBool());
        auto command = QJsonObject{{"device", "test-device"},
                                   {"operation", "set"},
                                   {"rgb", QJsonArray{0, 128, 255}},
                                   {"brightness", 50}};
        QVERIFY(DynamicLighting::validate(command).isEmpty());
        command["rgb"] = QJsonArray{0, 128, 256};
        QVERIFY(!DynamicLighting::validate(command).isEmpty());
        command["rgb"] = QJsonArray{0, 128, 255};
        command["brightness"] = 101;
        QVERIFY(!DynamicLighting::validate(command).isEmpty());
        QJsonObject lightingState{{"enabled", true},
                                  {"control_enabled", true},
                                  {"cloud", false},
                                  {"devices", QJsonArray{QJsonObject{{"id", "fixture"}}}}};
        QJsonObject frame{{"sensors", QJsonArray{}}, {"dynamic_lighting", lightingState}};
        PublishingPolicy localPolicy, cloudPolicy;
        QCOMPARE(
            localPolicy.apply(frame, false, 0)["dynamic_lighting"].toObject()["devices"].toArray().size(), 1);
        QVERIFY(
            cloudPolicy.apply(frame, true, 0)["dynamic_lighting"].toObject()["devices"].toArray().isEmpty());
        local.setModuleEnabled("windows", false);
        QVERIFY(!local.dynamicLightingState()["enabled"].toBool());
        QVERIFY(local.lightingShared()["devices"].toArray().isEmpty());
        local.setModuleEnabled("windows", true);
        QVERIFY(local.dynamicLightingState()["enabled"].toBool());
        local.setDynamicLightingOption("control", false);
        QVERIFY(!local.dynamicLightingState()["control_enabled"].toBool());
    }

    void coreTempSharedMemory() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QByteArray bytes(4740, 0);
        auto integer = [&](int offset, quint32 value) {
            for (int i = 0; i < 4; ++i)
                bytes[offset + i] = char(value >> (i * 8));
        };
        auto real = [&](int offset, float value) {
            quint32 bits;
            memcpy(&bits, &value, 4);
            integer(offset, bits);
        };
        integer(1536, 2);
        integer(1540, 1);
        integer(2688, 2);
        integer(1024, 100);
        integer(0, 44);
        integer(4, 60);
        real(1544, 54.5);
        real(1548, 57);
        real(2572, 4000);
        bytes[2686] = 1;
        bytes[2687] = 1;
        integer(2692, 125);
        real(3204, 55);
        auto parsed = CoreTemp::parse(bytes);
        QVERIFY(parsed["connected"].toBool());
        auto row = [&](QString id) {
            for (auto value : parsed["readings"].toArray())
                if (value.toObject()["id"] == id)
                    return value.toObject();
            return QJsonObject{};
        };
        QCOMPARE(row("coretemp_cpu_0_core_0_temperature")["value"].toDouble(), 54.5);
        QCOMPARE(row("coretemp_cpu_0_power")["value"].toDouble(), 55.0);
        QCOMPARE(row("coretemp_cpu_0_highest_core_temperature")["value"].toDouble(), 57.0);
        real(1548, std::numeric_limits<float>::quiet_NaN());
        parsed = CoreTemp::parse(bytes);
        QCOMPARE(row("coretemp_cpu_0_highest_core_temperature")["value"].toDouble(), 54.5);
        real(1544, std::numeric_limits<float>::infinity());
        parsed = CoreTemp::parse(bytes);
        QVERIFY(row("coretemp_cpu_0_highest_core_temperature").isEmpty());
        real(1544, 54.5);
        real(1548, 57);
        integer(1540, 2);
        integer(1028, 100);
        real(1552, 70);
        real(1556, 82);
        parsed = CoreTemp::parse(bytes);
        QCOMPARE(row("coretemp_cpu_0_highest_core_temperature")["value"].toDouble(), 57.0);
        QCOMPARE(row("coretemp_cpu_1_highest_core_temperature")["value"].toDouble(), 82.0);
        integer(1540, 1);
        QVERIFY(!CoreTemp::parse(bytes.left(100))["connected"].toBool());
        QVERIFY(!CoreTemp::parse(bytes.left(3000))["connected"].toBool());
        integer(1536, 257);
        QVERIFY(!CoreTemp::parse(bytes)["connected"].toBool());
        integer(1536, 2);
        bytes[2684] = 1;
        real(1544, 98.6f);
        real(1548, 104);
        parsed = CoreTemp::parse(bytes);
        QCOMPARE(row("coretemp_cpu_0_highest_core_temperature")["value"].toDouble(), 40.0);
        bytes[2685] = 1;
        real(1544, 18); // 10 degrees Celsius below TjMax.
        real(1548, 36);
        parsed = CoreTemp::parse(bytes);
        QVERIFY(row("coretemp_cpu_0_core_0_temperature").isEmpty());
        QCOMPARE(row("coretemp_cpu_0_core_0_distance_to_tjmax")["unit"].toString(), QString::fromUtf8("°C"));
        QCOMPARE(row("coretemp_cpu_0_highest_core_temperature")["value"].toDouble(), 90.0);
        integer(1024, 0);
        parsed = CoreTemp::parse(bytes);
        QVERIFY(row("coretemp_cpu_0_highest_core_temperature").isEmpty());
        integer(1024, 100);
        parsed = CoreTemp::parse(bytes.left(2688), false);
        QVERIFY(parsed["connected"].toBool());
        QVERIFY(row("coretemp_cpu_0_power").isEmpty());
        QTemporaryDir directory;
        Controller local(directory.path(), true);
        QVERIFY(!local.moduleEnabled("coretemp"));
        local.setModuleEnabled("coretemp", true);
        local.acceptCoreTemp(parsed);
        QVERIFY(local.coretempHealthy());
        local.selected.insert("coretemp_cpu_0_core_0_distance_to_tjmax");
        bool shared = false;
        for (auto value : local.snapshot()["sensors"].toArray())
            shared |= value.toObject()["source"] == "coretemp";
        QVERIFY(shared);
        local.setBrowserFilters("", "coretemp", false, false);
        QVERIFY(!local.browser().isEmpty());
        local.setModuleEnabled("coretemp", false);
        QVERIFY(!local.coretempHealthy());
        for (auto value : local.snapshot()["sensors"].toArray())
            QVERIFY(value.toObject()["source"] != "coretemp");
    }

    void workerCapturesOwnTheirReadingsAndPermissions() {
        QTemporaryDir dir;
        std::function<QJsonObject()> frame;
        std::function<Controller::BrowserInput()> browser;
        {
            Controller local(dir.path(), true);
            local.sampleTimer.stop();
            local.hwinfoTimer.stop();
            local.publishTimer.stop();
            local.catalog.clear();
            local.selected.clear();
            local.windowsClock.start();
            auto sensor = row("win_capture", 42);
            sensor["source"] = "windows";
            local.catalog.insert("win_capture", sensor);
            local.selected.insert("win_capture");
            frame = local.captureSnapshot();
            browser = local.captureBrowserInput();
            local.catalog["win_capture"]["value"] = 99;
            local.selected.clear();
            local.settings.setValue("modules/windows/enabled", false);
        }
        // The controller no longer exists: workers may only access captured values.
        auto future = QtConcurrent::run([frame, browser] { return qMakePair(frame(), browser()); });
        future.waitForFinished();
        const auto result = future.result();
        QCOMPARE(result.first["sensors"].toArray().size(), 1);
        QCOMPARE(result.first["sensors"].toArray().first().toObject()["value"].toInt(), 42);
        QCOMPARE(result.first["selected_sensor_ids"].toArray().first().toString(), QString("win_capture"));
        bool found = false;
        for (const auto &value : result.second.rows) {
            const auto item = value.toMap();
            if (item.value("id") == "win_capture") {
                found = true;
                QCOMPARE(item.value("value").toInt(), 42);
            }
        }
        QVERIFY(found);
    }

    void controllerPolicyChangeRejectsInflightSnapshot() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.publishTimer.stop();
        local.sensorTrayTimer.stop();
        local.snapshotTimer.stop();
        QTRY_VERIFY(!local.snapshotJob.isRunning());
        auto resume = std::make_shared<QSemaphore>();
        auto entered = std::make_shared<QSemaphore>();
        const auto oldEpoch = local.snapshotEpoch;
        local.snapshotTasks.enqueue({[resume, entered] {
                                         entered->release();
                                         resume->tryAcquire(1, 3000);
                                         return QJsonObject{{"private_fixture", true}};
                                     },
                                     false, false, oldEpoch});
        local.dispatchSnapshot();
        QVERIFY(entered->tryAcquire(1, 3000));
        auto release = qScopeGuard([resume] { resume->release(); });
        local.selected.clear();
        local.publishPolicy();
        QVERIFY(local.snapshotEpoch > oldEpoch);
        resume->release();
        QTRY_VERIFY(!local.snapshotJob.isRunning() && local.snapshotTasks.isEmpty());
        bool leaked = true;
        QMetaObject::invokeMethod(
            local.server.worker, [&] { leaked = local.server.worker->latest.contains("private_fixture"); },
            Qt::BlockingQueuedConnection);
        QVERIFY(!leaked);
    }

    void uiSelectionCountsRefreshAfterSelectionAndModuleChanges() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.publishTimer.stop();
        ++local.collectionGeneration; // Ignore collection jobs started by the constructor.
        local.catalog.clear();
        local.selected.clear();
        auto sensor = row("win_count", 42);
        sensor["source"] = "windows";
        local.catalog.insert("win_count", sensor);
        local.selected.insert("win_count");
        QTRY_COMPARE(local.uiInfo().value("selected").toInt(), 1);
        local.selected.clear();
        emit local.changed();
        QTRY_COMPARE(local.uiInfo().value("selected").toInt(), 0);
        local.selected.insert("win_count");
        emit local.changed();
        QTRY_COMPARE(local.uiInfo().value("selected").toInt(), 1);
        local.settings.setValue("modules/windows/enabled", false);
        emit local.changed();
        QTRY_COMPARE(local.uiInfo().value("selected").toInt(), 0);
        local.settings.setValue("modules/windows/enabled", true);
        emit local.changed();
        QTRY_COMPARE(local.uiInfo().value("selected").toInt(), 1);
    }

    void browserMakesProgressDuringContinuousSamples() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.publishTimer.stop();
        ++local.collectionGeneration;
        local.catalog.clear();
        local.selected.clear();
        local.windowsClock.start();
        for (int i = 0; i < 1000; ++i) {
            const auto id = "win_progress_" + QString::number(i);
            auto sensor = row(id, i);
            sensor["source"] = "windows";
            local.catalog.insert(id, sensor);
        }
        local.browserModel.replace({});
        local.browserVisible = true;
        QTimer arriving;
        connect(&arriving, &QTimer::timeout, &local, [&] { emit local.sensorsChanged(); });
        arriving.start(5);
        local.refreshBrowserAsync();
        QTRY_VERIFY_WITH_TIMEOUT(local.browserModel.rowCount() >= 1000, 5000);
    }

    void catalogueSnapshotsSurviveUpdatesAndMissingReadings() {
        SensorCatalog catalogue;
        for (int i = 0; i < 1000; ++i)
            catalogue.insert(QString::number(i), {{"value", i}});
        const auto captured = catalogue;
        catalogue["10"]["value"] = 2000;
        catalogue.insert("new", {{"value", 1}});
        QCOMPARE(captured.value("10").value("value").toInt(), 10);
        QVERIFY(!captured.contains("new"));
        int count = 0;
        for (auto it = captured.cbegin(); it != captured.cend(); ++it) {
            QCOMPARE(it.value().value("value").toInt(), it.key().toInt());
            ++count;
        }
        QCOMPARE(count, 1000);
        QCOMPARE(catalogue.size(), 1001);
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        ++local.collectionGeneration;
        local.catalog.clear();
        local.mergeReadings("windows", {row("win_a", 1), row("win_b", 2)});
        local.mergeReadings("hwinfo", {row("hw_a", 3)});
        local.mergeReadings("windows", {row("win_b", 4)});
        QVERIFY(!local.catalog.value("win_a").value("available").toBool());
        QCOMPARE(local.catalog.value("hw_a").value("value").toInt(), 3);
        QCOMPARE(local.catalog.value("win_b").value("value").toInt(), 4);
    }

    void preparedReadingsPreserveConcurrentChanges() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        ++local.collectionGeneration;
        local.catalog.clear();
        local.selected.clear();
        local.settings.setValue("selection_initialized", true);
        local.mergeReadings("hwinfo", {row("hw_a", 1), row("hw_missing", 2)});
        // Exercise the collision path, not just two independent catalogue pages.
        QString nativeId;
        for (int n = 0; nativeId.isEmpty(); ++n) {
            const auto id = QString("win_native_%1").arg(n);
            if (qHash(id, 0) % 256 == qHash(QString("hw_a"), 0) % 256)
                nativeId = id;
        }
        local.mergeReadings("windows", {row(nativeId, 3)});
        const auto snapshot = local.catalog;
        const auto prepare = local.prepareReadingsUpdate("hwinfo");
        local.catalog[nativeId]["value"] = 4;
        local.mergeReadings("coretemp", {row("core_new", 5)});
        const auto update = prepare({row("hw_a", 6), row("hw_new", 7)});
        local.applyReadingsUpdate("hwinfo", update);
        QCOMPARE(snapshot.value("hw_a")["value"].toInt(), 1);
        QCOMPARE(snapshot.value(nativeId)["value"].toInt(), 3);
        QVERIFY(!snapshot.contains("hw_new"));
        QCOMPARE(local.catalog.value("hw_a")["value"].toInt(), 6);
        QCOMPARE(local.catalog.value("hw_new")["value"].toInt(), 7);
        QCOMPARE(local.catalog.value(nativeId)["value"].toInt(), 4);
        QCOMPARE(local.catalog.value("core_new")["value"].toInt(), 5);
        QVERIFY(!local.catalog.value("hw_missing")["available"].toBool());
        QVERIFY(local.catalog.value("hw_missing")["value"].isNull());
        QCOMPARE(local.catalog.size(), 5);
        QCOMPARE(local.catalogSourceIds.value("hwinfo"), QSet<QString>({"hw_a", "hw_missing", "hw_new"}));
        QCOMPARE(local.catalogSourceIds.value("coretemp"), QSet<QString>({"core_new"}));
    }

    void preparedReadingsRespectConcurrentCapacityLimit() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        ++local.collectionGeneration;
        local.catalog.clear();
        local.selected.clear();
        for (int n = 0; n < 9989; ++n)
            local.catalog.insert(QString("filler_%1").arg(n), {{"source", "windows"}, {"value", n}});
        auto recommended = row("hw_new", 1);
        recommended["name"] = "CPU Package";
        const auto update = local.prepareReadingsUpdate("hwinfo")({recommended});
        local.catalog.insert("win_last_slot", {{"source", "windows"}, {"value", 2}});
        local.settings.setValue("selection_initialized", false);
        local.applyReadingsUpdate("hwinfo", update);
        QCOMPARE(local.catalog.size(), 9990);
        QVERIFY(local.catalog.contains("win_last_slot"));
        QVERIFY(!local.catalog.contains("hw_new"));
        QVERIFY(!local.catalogSourceIds.value("hwinfo").contains("hw_new"));
        QVERIFY(!local.selected.contains("hw_new"));
    }

    void sensorValueUpdatesDoNotInvalidateMetadata() {
        BrowserModel model;
        QVariantMap first{{"id", "one"}, {"name", "Temperature"}, {"value", 20}, {"available", true}};
        model.replace({first});
        QSignalSpy changeSignals(&model, &QAbstractItemModel::dataChanged);
        auto next = first;
        next["value"] = 21;
        const auto prepared = BrowserModel::prepare(model.snapshot(), {next}, model.generation());
        model.apply(prepared);
        QCOMPARE(changeSignals.size(), 1);
        const auto roles = qvariant_cast<QList<int>>(changeSignals.last().at(2));
        QVERIFY(roles.contains(BrowserModel::Value));
        QVERIFY(!roles.contains(BrowserModel::Row));
        QCOMPARE(model.data(model.index(0), BrowserModel::Value).toInt(), 21);
        next["name"] = "CPU temperature";
        model.update({next});
        QVERIFY(qvariant_cast<QList<int>>(changeSignals.last().at(2)).contains(BrowserModel::Row));
    }

    void selectedReadingsAndLargeCatalogueBenchmark() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        ++local.collectionGeneration; // Ignore collection jobs started by the constructor.
        local.publishTimer.stop();
        local.icueTimer.stop();
        local.catalog.clear();
        local.selected.clear();
        local.windowsClock.start();
        for (int n = 0; n < 3000; ++n) {
            auto item = row("win_fixture_" + QString::number(n), n);
            local.catalog.insert(item["id"].toString(), item);
            local.selected.insert(item["id"].toString());
        }
        local.uiInfoValid = false;
        QTRY_COMPARE(local.uiInfo().value("selected").toInt(), 3000);
        local.selected.detach();
        QElapsedTimer infoClock;
        infoClock.start();
        for (int n = 0; n < 100; ++n)
            QCOMPARE(local.uiInfo().value("selected").toInt(), 3000);
        qInfo() << "Cached UI status (3000 selected), average us" << infoClock.nsecsElapsed() / 100000;
        QSet<QString> wanted{"win_fixture_42", "app_uptime"};
        const auto subset = local.allReadings(&wanted);
        QCOMPARE(subset.size(), 2);
        for (auto item : subset)
            QVERIFY(wanted.contains(item.toObject()["id"].toString()));
        QElapsedTimer clock;
        clock.start();
        for (int n = 0; n < 100; ++n)
            local.allReadings(&wanted);
        qInfo() << "AUDIT: selected tray lookup (2 of 3008), average us" << clock.nsecsElapsed() / 100000;
        clock.restart();
        for (int n = 0; n < 10; ++n)
            local.snapshot();
        qInfo() << "AUDIT: snapshot (3000 selected), average us" << clock.nsecsElapsed() / 10000;
        clock.restart();
        auto buildInput = local.captureBrowserInput();
        qInfo() << "AUDIT: browser GUI capture (3000), us" << clock.nsecsElapsed() / 1000;
        clock.restart();
        auto input = buildInput();
        qInfo() << "AUDIT: browser worker input (3000), us" << clock.nsecsElapsed() / 1000;
        clock.restart();
        auto formatted = Controller::formatBrowser(input.rows, local.browserFilters(), {});
        qInfo() << "AUDIT: browser worker formatting (3000), us" << clock.nsecsElapsed() / 1000;
        QVERIFY(!formatted.isEmpty());
    }

    void browserReorderPreservesPersistentIndexes() {
        BrowserModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVariantList values;
        for (int n = 0; n < 3000; ++n)
            values.append(QVariantMap{{"id", QString::number(n)}, {"value", n}});
        model.replace(values);
        QPersistentModelIndex retained(model.index(42));
        std::reverse(values.begin(), values.end());
        values.prepend(QVariantMap{{"id", "new"}});
        model.update(values);
        QVERIFY(retained.isValid());
        QCOMPARE(retained.data(Qt::UserRole + 1).toMap()["id"].toString(), QString("42"));
        QCOMPARE(model.rowCount(), 3001);
        model.update(values.mid(1));
        QVERIFY(retained.isValid());
        QCOMPARE(model.rowCount(), 3000);
    }

    void browserMixedChangesPreserveSurvivingIndexes() {
        BrowserModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVariantList original;
        for (const auto id : {"a", "b", "c", "d", "e"})
            original.append(QVariantMap{{"id", id}, {"value", 1}});
        model.replace(original);
        QPersistentModelIndex a(model.index(0)), b(model.index(1)), removed(model.index(2)),
            e(model.index(4));
        const QVariantList next{
            QVariantMap{{"id", "b"}, {"value", 2}}, QVariantMap{{"id", "new"}, {"value", 3}},
            QVariantMap{{"id", "e"}, {"value", 1}}, QVariantMap{{"id", "a"}, {"value", 4}}};
        model.apply(BrowserModel::prepare(model.snapshot(), next, model.generation()));
        QCOMPARE(model.snapshot(), next);
        QCOMPARE(a.row(), 3);
        QCOMPARE(b.row(), 0);
        QCOMPARE(e.row(), 2);
        QVERIFY(!removed.isValid());
        QCOMPARE(a.data(BrowserModel::Value).toInt(), 4);
        QCOMPARE(b.data(BrowserModel::Value).toInt(), 2);
    }

    void policyRevocationDoesNotWaitForNetworkWorker() {
        NetworkService service;
        auto resume = std::make_shared<QSemaphore>();
        QSemaphore entered;
        QMetaObject::invokeMethod(service.worker, [resume, &entered] {
            entered.release();
            resume->tryAcquire(1, 2000);
        });
        entered.acquire();
        auto release = qScopeGuard([resume] { resume->release(); });
        service.setEventSnapshot({{"private", "old"}});
        QElapsedTimer clock;
        clock.start();
        service.replaceSnapshot({{"sensors", QJsonArray{}}});
        QVERIFY2(clock.elapsed() < 500, "GUI waited for a blocked network worker during policy revocation");
        service.setSnapshot({{"safe", true}});
        resume->release();
        bool safe = false;
        QMetaObject::invokeMethod(
            service.worker,
            [&] {
                safe = service.worker->latest["safe"].toBool() && service.worker->pendingEvents.isEmpty() &&
                       service.worker->appliedGeneration == service.policyGeneration();
            },
            Qt::BlockingQueuedConnection);
        QVERIFY(safe);
    }

    void displayReadOnlyScanQueuesCommands() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.setDisplaysEnabled(true);
        QTRY_VERIFY(!local.displayJob.isRunning());
        auto resume = std::make_shared<QSemaphore>();
        QSemaphore entered;
        local.displayQuerying = true;
        local.displayJob.submit(
            [resume, &entered] {
                entered.release();
                resume->tryAcquire(1, 2000);
                return 0;
            },
            [&](int) {
                local.displayQuerying = false;
                local.drainDisplayCommand();
            });
        entered.acquire();
        auto release = qScopeGuard([resume] { resume->release(); });
        QVERIFY(!local.displaysState()["busy"].toBool());
        local.localDisplayCommand({{"operation", "extend"}, {"monitor", QString(24, 'a')}});
        QVERIFY(local.displayCommandQueued);
        QVERIFY(local.displaysState()["busy"].toBool());
        resume->release();
        QTRY_VERIFY(!local.displayCommandQueued);
        QTRY_VERIFY(!local.displayJob.isRunning());
        QVERIFY(local.displayMessage.contains("disabled in test mode"));
    }

    void windowModePersistenceAndRestore() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        WindowFrame frame(reinterpret_cast<HWND>(window->winId()));
        for (const auto &mode : {QString("maximised"), QString("fullscreen"), QString("normal")}) {
            QSignalSpy frames(window, &QQuickWindow::frameSwapped);
            local.setWindowMode(mode);
            restoreWhatWindow(window, local.windowMode());
            const auto expected = mode == "fullscreen"  ? QWindow::FullScreen
                                  : mode == "maximised" ? QWindow::Maximized
                                                        : QWindow::Windowed;
            QTRY_COMPARE(window->visibility(), expected);
            QVERIFY(QTest::qWaitForWindowExposed(window));
            QTRY_VERIFY(!frames.isEmpty());
            QCOMPARE(local.windowMode(), mode);
            window->showMinimized();
            QTRY_COMPARE(window->visibility(), QWindow::Minimized);
            QCOMPARE(local.windowMode(), mode);
            window->hide();
            QCOMPARE(local.windowMode(), mode);
            QSettings persisted(local.settings.fileName(), SettingsStore::format());
            QCOMPARE(persisted.value("ui/window_mode").toString(), mode);
            const auto yaml = ConfigurationFile::encode(ConfigurationFile::snapshot(persisted));
            QCOMPARE(ConfigurationFile::decode(yaml).values["ui/window_mode"].toString(), mode);
            QQuickWindow freshWindow;
            freshWindow.resize(700, 500);
            restoreWhatWindow(&freshWindow, persisted.value("ui/window_mode").toString());
            QTRY_COMPARE(freshWindow.visibility(), expected);
            freshWindow.hide();
        }
        window->showNormal();
        QTRY_COMPARE(local.windowMode(), QString("normal"));
        local.setWindowMode("minimised");
        QCOMPARE(local.windowMode(), QString("normal"));
        QVERIFY(!ConfigurationFile::decode(ConfigurationFile::encode({{"ui/window_mode", "hidden"}}))
                     .error.isEmpty());
    }

    void mqttReportsActualAttemptAndRetry() {
        QTcpServer broker;
        QVERIFY(broker.listen(QHostAddress::LocalHost, 0));
        connect(&broker, &QTcpServer::newConnection, this, [&] {
            while (broker.hasPendingConnections()) {
                auto *socket = broker.nextPendingConnection();
                socket->abort();
                socket->deleteLater();
            }
        });
        MqttBridge bridge;
        QSignalSpy progress(&bridge, &MqttBridge::progress);
        bridge.configure(
            {{"enabled", true}, {"host", "127.0.0.1"}, {"port", int(broker.serverPort())}, {"tls", false}},
            "progress-fixture", QString(64, 'a'));
        auto hasPhase = [&](const QString &phase) {
            for (const auto &event : progress)
                if (event[0].toString() == phase)
                    return true;
            return false;
        };
        QVERIFY(hasPhase("connecting"));
        QTRY_VERIFY_WITH_TIMEOUT(hasPhase("retrying"), 12000);
        QCOMPARE(progress.last()[1].toInt(), 5000);
        bridge.stop();
    }

    void mqttBrokerDiagnostics_data() {
        QTest::addColumn<int>("connectCode");
        QTest::addColumn<int>("subscribeCode");
        QTest::addColumn<QString>("expected");
        QTest::newRow("connected") << 0 << 0 << "subscription confirmed; granted_qos=0";
        QTest::newRow("credentials-refused") << 5 << 0 << "connect failed; code=5";
        QTest::newRow("subscription-refused") << 0 << 128 << "subscribe failed; code=128";
    }

    void mqttBrokerDiagnostics() {
        QFETCH(int, connectCode);
        QFETCH(int, subscribeCode);
        QFETCH(QString, expected);
        QTcpServer broker;
        QVERIFY(broker.listen(QHostAddress::LocalHost, 0));
        // A small loopback broker exercises real Paho callbacks, including SUBACK.
        connect(&broker, &QTcpServer::newConnection, this, [&] {
            auto *socket = broker.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, connectCode, subscribeCode, buffer = QByteArray()]() mutable {
                buffer += socket->readAll();
                while (buffer.size() >= 2) {
                    int size = 0, shift = 0, header = 1;
                    uchar digit;
                    do {
                        if (header >= buffer.size())
                            return;
                        digit = uchar(buffer[header++]);
                        size |= (digit & 127) << shift;
                        shift += 7;
                    } while (digit & 128);
                    if (buffer.size() < header + size)
                        return;
                    const int type = uchar(buffer[0]) >> 4;
                    const auto body = buffer.mid(header, size);
                    buffer.remove(0, header + size);
                    if (type == 1) {
                        auto reply = QByteArray::fromHex("20020000");
                        reply[3] = char(connectCode);
                        socket->write(reply);
                    } else if (type == 8) {
                        auto reply = QByteArray::fromHex("9003") + body.left(2);
                        reply.append(char(subscribeCode));
                        socket->write(reply);
                    } else if (type == 12)
                        socket->write(QByteArray::fromHex("d000"));
                }
            });
        });
        MqttBridge bridge;
        QStringList logs;
        connect(&bridge, &MqttBridge::diagnostic, this, [&](QString text) { logs.append(text); });
        bridge.setDiagnostics(true);
        bridge.configure({{"enabled", true}, {"host", "127.0.0.1"}, {"port", int(broker.serverPort())},
                          {"tls", false}, {"scope", "local"}, {"username", "PRIVATE-USERNAME"},
                          {"password", "PRIVATE-PASSWORD"}}, "PRIVATE-DEVICE", "PRIVATE-PAIRING");
        QTRY_VERIFY_WITH_TIMEOUT(logs.join('\n').contains(expected), 12000);
        if (connectCode || subscribeCode) {
            QTRY_VERIFY(bridge.retry.isActive());
            QVERIFY(!bridge.online);
        } else {
            QTRY_VERIFY(bridge.completed.load() > 0);
            bridge.reportTraffic();
            QVERIFY(logs.last().contains("write_completions="));
            QVERIFY(bridge.traffic.submitted > 0);
        }
        bridge.stop();
        for (const auto &line : logs) {
            QVERIFY(!line.contains("PRIVATE-"));
            QVERIFY(!line.contains("127.0.0.1"));
            QCOMPARE(DiagnosticReport::activity(line), line);
        }
    }

    void mqttMessageDiagnosticsAreBoundedAndPrivate() {
        MqttBridge bridge;
        QStringList logs;
        connect(&bridge, &MqttBridge::diagnostic, this, [&](QString text) { logs.append(text); });
        bridge.configure({{"enabled", false}, {"preferred", true}}, "PRIVATE-DEVICE", "PRIVATE-PAIRING");
        bridge.online = true;
        bridge.epoch = QUuid::createUuid().toString(QUuid::WithoutBraces);
        bridge.setDiagnostics(true);
        const auto sid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto auth = bridge.envelope("{\"type\":\"auth\",\"protocol\":2}", sid, 1, "ha");
        bridge.receive(bridge.base + "/in", auth, false);
        QVERIFY(bridge.peer);
        QSignalSpy delivered(bridge.peer, &WirePeer::textMessageReceived);
        logs.clear();
        for (int n = 0; n < 100; ++n) {
            bridge.receive(bridge.base + "/in", auth, true);
            bridge.receive(bridge.base + "/in", auth, false);
            bridge.receive(bridge.base + "/in", "PRIVATE-BAD-JSON", false);
        }
        QCOMPARE(logs.size(), 3); // One log per reason, not one per bad packet.
        auto tampered = QJsonDocument::fromJson(auth).object();
        tampered["mac"] = "PRIVATE-BAD-SIGNATURE";
        bridge.receive(bridge.base + "/in", QJsonDocument(tampered).toJson(), false);
        QVERIFY(logs.last().contains("signature mismatch"));
        bridge.receive(bridge.base + "/in", bridge.envelope("{\"type\":\"ping\"}", sid, 2, "ha"), false);
        bridge.receive(bridge.base + "/in", bridge.envelope(
            "{\"type\":\"command\",\"args\":{\"text\":\"PRIVATE-PAYLOAD\"}}", sid, 3, "ha"), false);
        QCOMPARE(delivered.size(), 1);
        QCOMPARE(bridge.traffic.pings, quint64(1));
        QCOMPARE(bridge.traffic.requests, quint64(1));
        bridge.reportTraffic();
        QVERIFY(logs.last().contains("retained inbound=100"));
        QVERIFY(logs.last().contains("replayed sequence=100"));
        QVERIFY(logs.last().contains("rx_seq=3"));
        for (const auto &line : logs) {
            QVERIFY(!line.contains("PRIVATE-"));
            QVERIFY(!line.contains(sid));
            QVERIFY(!line.contains(bridge.epoch));
            QCOMPARE(DiagnosticReport::activity(line), line);
        }
        bridge.setDiagnostics(false);
        logs.clear();
        bridge.receive(bridge.base + "/in", "bad", false);
        bridge.reportTraffic();
        bridge.setPreferred(false);
        QVERIFY(logs.isEmpty());
        bridge.stop();
    }

    void pairingAddressIsSessionOnly() {
        const auto imported = ConfigurationFile::decode(ConfigurationFile::encode(
            {{"hide_connection", false}, {"start_minimised", true}}));
        QVERIFY(imported.error.isEmpty());
        QVERIFY(!imported.values.contains("hide_connection"));
        QVERIFY(imported.values["start_minimised"].toBool());
        QTemporaryDir dir;
        {
            Controller previous(dir.path(), true);
            previous.settings.setValue("hide_connection", false);
            previous.settings.sync();
        }
        for (int launch = 0; launch < 2; ++launch) {
            Controller local(dir.path(), true);
            UpdateManager updater(dir.path(), true);
            QQmlApplicationEngine engine;
            QSignalSpy warnings(&engine, &QQmlEngine::warnings);
            engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                         {"updater", QVariant::fromValue(&updater)}});
            engine.load(QUrl("qrc:/qml/Main.qml"));
            QVERIFY(!engine.rootObjects().isEmpty());
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            QVERIFY(window);
            auto *page = visualItem(window->contentItem(), "overviewScroll");
            auto *hide = visualItem(window->contentItem(), "hidePairingAddress");
            QVERIFY(page && hide);
            QVERIFY(page->property("hideAddress").toBool());
            QVERIFY(hide->property("checked").toBool());
            // Exercise the toggle's handler without opening the entire section.
            hide->setProperty("checked", false);
            QVERIFY(QMetaObject::invokeMethod(hide, "clicked"));
            QVERIFY(!page->property("hideAddress").toBool());
            QVERIFY(!local.settings.contains("hide_connection"));
            QVERIFY(!ConfigurationFile::snapshot(local.settings).contains("hide_connection"));
            QCOMPARE(warnings.count(), 0);
        }
    }

    void connectionHeartbeatsKeepIndependentSweeps();

    void heartbeatHistorySurvivesPageChanges() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        // This fixture supplies its own replies; a queued network shutdown must
        // not replace them with a disconnected status while testing navigation.
        QObject::disconnect(&local.server, &NetworkService::installationState, &local, nullptr);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                     {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->resize(1100, 760);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *graph = visualItem(window->contentItem(), "overviewHeartbeat");
        QVERIFY(graph);
        QTRY_VERIFY(graph->width() > 0);
        graph->setProperty("millisecondsPerPixel", 30000.0 / graph->width());
        const auto id = local.installationConfiguration().first().toMap()["id"].toString();
        auto *trace = visualItem(window->contentItem(), "installationHeartbeat_" + id);
        QVERIFY(trace);
        local.settings.setValue("service_enabled", true);
        auto beats = [&] { return engine.newQObject(trace).property("beats").toVariant().toList(); };
        auto reply = [&] {
            const auto time = double(QDateTime::currentMSecsSinceEpoch());
            local.installationStates[id] = {{"clients", 1}, {"feedback", true},
                {"lastCommunication", time}};
            emit local.installationsChanged();
            return time;
        };
        QTRY_VERIFY(trace->property("animating").toBool());
        reply();
        QTRY_VERIFY(trace->property("connected").toBool());
        QTRY_COMPARE(beats().size(), 1);
        const auto firstBeat = beats().first().toMap();
        const auto started = trace->property("sweepStarted").toDouble();

        window->setProperty("previewPage", 1);
        QTRY_VERIFY(!graph->property("active").toBool());
        QVERIFY(!trace->property("animating").toBool());
        QCOMPARE(beats().size(), 1);
        QTest::qWait(300);
        reply();
        QTRY_COMPARE(beats().size(), 2);
        QCOMPARE(beats().first().toMap()["time"], firstBeat["time"]);
        QCOMPARE(beats().first().toMap()["amplitude"], firstBeat["amplitude"]);
        window->setProperty("previewPage", 0);
        QTRY_VERIFY(trace->property("animating").toBool());
        QTRY_VERIFY(trace->property("currentPath").toString().contains(" C "));
        QCOMPARE(trace->property("sweepStarted").toDouble(), started);
        QCOMPARE(beats().size(), 2);

        // Leaving for several complete sweeps must resume at real elapsed time,
        // including replies received while the page was hidden.
        graph->setProperty("millisecondsPerPixel", 800.0 / graph->width());
        window->setProperty("previewPage", 1);
        QTRY_VERIFY(!graph->property("active").toBool());
        QTest::qWait(1900);
        const auto hiddenReply = reply();
        QTRY_COMPARE(beats().last().toMap()["time"].toDouble(), hiddenReply);
        window->setProperty("previewPage", 0);
        QTRY_VERIFY(trace->property("animating").toBool());
        QTRY_VERIFY(trace->property("sweepStarted").toDouble() > started);
        const auto resumed = trace->property("sweepStarted").toDouble();
        QVERIFY(double(QDateTime::currentMSecsSinceEpoch()) - resumed < 900);
        QCOMPARE(beats().last().toMap()["time"].toDouble(), hiddenReply);
        QTRY_VERIFY((trace->property("currentPath").toString()
                     + trace->property("previousPath").toString()).contains(" C "));
        QCOMPARE(warnings.count(), 0);
    }

    void installationColoursRemainStable() {
        QTemporaryDir dir;
        QVariantMap colours;
        {
            Controller local(dir.path(), true);
            local.stopService();
            for (int i = 0; i < 9; ++i)
                QVERIFY(local.saveInstallation({{"name", QString("House %1").arg(i)}, {"type", "api"},
                    {"url", ""}, {"enabled", false}, {"controls", false}}));
            QSet<QString> unique;
            for (const auto &value : local.installationConfiguration()) {
                const auto row = value.toMap();
                colours[row["id"].toString()] = row["colour"];
                unique.insert(row["colour"].toString());
            }
            QCOMPARE(unique.size(), colours.size());
            auto renamed = local.installationConfiguration().last().toMap();
            renamed["name"] = "Renamed House";
            QVERIFY(local.saveInstallation(renamed));
            const auto removed = local.installationConfiguration().first().toMap()["id"].toString();
            QVERIFY(local.deleteInstallation(removed));
            colours.remove(removed);
            local.settings.sync();
        }
        Controller reopened(dir.path(), true);
        reopened.stopService();
        for (const auto &value : reopened.installationConfiguration()) {
            const auto row = value.toMap();
            QCOMPARE(row["colour"], colours[row["id"].toString()]);
        }
    }

    void connectionHeartbeatPresentation() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"QML(
            import QtQuick
            import "qrc:/qml/components"
            import "qrc:/qml/controls"
            Window {
                width: 240; height: 80; visible: true
                color: Theme.inset
                property int beatCount: monitor.beats.length
                property bool animate: true
                onAnimateChanged: Theme.animationsEnabled = animate
                ConnectionHeartbeat {
                    id: monitor
                    objectName: "heartbeat"
                    anchors.centerIn: parent
                    connected: true
                    millisecondsPerPixel: 1800 / 220
                }
            }
        )QML");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *heartbeat = visualItem(window->contentItem(), "heartbeat");
        auto *baseline = visualItem(window->contentItem(), "haHeartbeatBaseline");
        auto *pulse = window->findChild<QObject *>("haHeartbeatPulse");
        QVERIFY(heartbeat && baseline && pulse);
        QTRY_VERIFY(pulse->property("running").toBool());
        QTRY_VERIFY(heartbeat->property("sweepStarted").toDouble() > 0);
        QCOMPARE(window->property("beatCount").toInt(), 0);
        const auto flat = window->grabWindow();
        QVERIFY(!flat.isNull());

        // Inspect pixels because the moving clips run on the render thread.
        // Only the upper part of an R peak reaches this area of the graph.
        const auto peakColumns = [&](const QImage &image) {
            QList<int> centres;
            const int topOfBaseline = qRound(
                heartbeat->mapToScene(QPointF(0, heartbeat->property("baselineY").toReal())).y()
                * window->devicePixelRatio() - 12 * window->devicePixelRatio());
            int first = -1;
            for (int x = 0; x <= image.width(); ++x) {
                bool hasPeak = false;
                for (int y = 0; x < image.width() && y < topOfBaseline; ++y)
                    hasPeak |= image.pixel(x, y) != flat.pixel(x, y);
                if (hasPeak && first < 0)
                    first = x;
                if (!hasPeak && first >= 0) {
                    centres.append((first + x - 1) / 2);
                    first = -1;
                }
            }
            return centres;
        };
        const auto sweepStarted = [&] {
            return heartbeat->property("sweepStarted").toDouble();
        };

        auto previousStart = sweepStarted();
        QTRY_VERIFY(sweepStarted() > previousStart);
        const auto drawingStart = sweepStarted();
        QTest::qWait(300);
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTRY_COMPARE(window->property("beatCount").toInt(), 1);
        QTest::qWait(350);
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTRY_COMPARE(window->property("beatCount").toInt(), 2);
        QTest::qWait(350);
        QCOMPARE(sweepStarted(), drawingStart); // Replies must not restart the sweep.
        const auto drawn = window->grabWindow();
        QCOMPARE(peakColumns(drawn).size(), 2);
        QTest::qWait(150);
        QCOMPARE(peakColumns(window->grabWindow()), peakColumns(drawn)); // Trace stays fixed.

        QTRY_VERIFY(sweepStarted() > drawingStart);
        const auto retained = window->grabWindow();
        QCOMPARE(peakColumns(retained).size(), 2); // Wrapping keeps the old line ahead.
        previousStart = sweepStarted();
        QTRY_VERIFY(sweepStarted() > previousStart);
        const auto overwritten = window->grabWindow();
        QCOMPARE(peakColumns(overwritten).size(), 0); // Silence overwrites it with a flat line.
        QVERIFY(pulse->property("running").toBool());

        if (!qEnvironmentVariableIsEmpty("WHAT_CONNECTION_PREVIEW")) {
            const auto path = qEnvironmentVariable("WHAT_CONNECTION_PREVIEW");
            QVERIFY(flat.save(path + ".flat.png"));
            QVERIFY(drawn.save(path + ".drawn.png"));
            QVERIFY(retained.save(path + ".retained.png"));
            QVERIFY(overwritten.save(path + ".overwritten.png"));
        }

        const auto retainedBeats = window->property("beatCount").toInt();
        heartbeat->setProperty("connected", false);
        QCOMPARE(window->property("beatCount").toInt(), retainedBeats);
        QVERIFY(pulse->property("running").toBool()); // A disconnected monitor still sweeps.
        heartbeat->setProperty("connected", true);
        heartbeat->setProperty("stale", true);
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTest::qWait(50);
        QCOMPARE(window->property("beatCount").toInt(), retainedBeats);
        QVERIFY(pulse->property("running").toBool());
        heartbeat->setProperty("stale", false);
        heartbeat->setProperty("active", false);
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTest::qWait(50);
        QVERIFY(!pulse->property("running").toBool());
        QVERIFY(baseline->isVisible());
        QTRY_COMPARE(window->property("beatCount").toInt(), retainedBeats + 1);
        heartbeat->setProperty("active", true);
        QTRY_VERIFY(pulse->property("running").toBool());
        QCOMPARE(window->property("beatCount").toInt(), retainedBeats + 1);
        window->setProperty("animate", false);
        QTRY_VERIFY(!pulse->property("running").toBool());
        QVERIFY(baseline->isVisible());
        window->setProperty("animate", true);
        QTRY_VERIFY(pulse->property("running").toBool());

        // Resizing must not restart the trace or change its time scale mid-pass.
        const auto resizeStart = sweepStarted();
        window->setWidth(480);
        heartbeat->setWidth(440);
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTRY_COMPARE(window->property("beatCount").toInt(), 1);
        QCOMPARE(sweepStarted(), resizeStart);
        QCOMPARE(heartbeat->property("historyMs").toInt(), 3600);
        QTRY_VERIFY_WITH_TIMEOUT(sweepStarted() > resizeStart, 4000);
        QCOMPARE(window->property("beatCount").toInt(), 1);
        QCOMPARE(warnings.count(), 0);
    }

    void connectionHeartbeatKeepsDrawnBeatsFixed() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"QML(
            import QtQuick
            import "qrc:/qml/components"
            import "qrc:/qml/controls"
            Window {
                width: 460; height: 80; visible: true
                color: Theme.inset
                property int beatCount: monitor.beats.length
                ConnectionHeartbeat {
                    id: monitor
                    objectName: "heartbeat"
                    anchors.centerIn: parent
                    width: 440
                    millisecondsPerPixel: 15000 / 440
                    connected: true
                }
            }
        )QML");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto *heartbeat = visualItem(window->contentItem(), "heartbeat");
        QVERIFY(heartbeat);
        QTRY_VERIFY(heartbeat->property("sweepStarted").toDouble() > 0);
        const auto started = heartbeat->property("sweepStarted").toDouble();
        const auto flat = window->grabWindow();
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTRY_COMPARE(window->property("beatCount").toInt(), 1);
        QTest::qWait(1000); // The first peak is visible, but its recovery tail is still being drawn.
        // Freeze the reveal before comparing geometry. A slow runner can draw
        // the second peak during grabWindow(), which is a different part of the trace.
        auto *pulse = heartbeat->findChild<QObject *>("haHeartbeatPulse");
        QVERIFY(pulse);
        QVERIFY(pulse->setProperty("paused", true));
        const auto before = window->grabWindow();
        const auto ratio = window->devicePixelRatio();
        const int peakBottom = qRound((heartbeat->mapToScene(QPointF(0,
            heartbeat->property("baselineY").toReal())).y() - 12) * ratio);
        const auto peakColumns = [&](const QImage &frame) {
            QList<int> columns;
            for (int x = 0; x < frame.width(); ++x) {
                for (int y = 0; y < peakBottom; ++y) {
                    if (frame.pixel(x, y) != flat.pixel(x, y)) {
                        columns.append(x);
                        break;
                    }
                }
            }
            return columns;
        };
        const auto firstPeak = peakColumns(before);
        QVERIFY(!firstPeak.isEmpty());
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTest::qWait(150);
        QCOMPARE(window->property("beatCount").toInt(), 2); // Keep close replies, without reshaping the first.
        const auto after = window->grabWindow();
        if (!qEnvironmentVariableIsEmpty("WHAT_CONNECTION_PREVIEW")) {
            QVERIFY(before.save(qEnvironmentVariable("WHAT_CONNECTION_PREVIEW") + ".before.png"));
            QVERIFY(after.save(qEnvironmentVariable("WHAT_CONNECTION_PREVIEW") + ".after.png"));
        }
        QCOMPARE(heartbeat->property("sweepStarted").toDouble(), started);
        const auto secondPeak = peakColumns(after);
        QVERIFY(!secondPeak.isEmpty());
        // Opposing render-thread translations can shift antialiased edge
        // coverage by one pixel. The peak's position and width must stay fixed.
        QVERIFY(qAbs(secondPeak.first() - firstPeak.first()) <= 1);
        QVERIFY(qAbs(secondPeak.last() - firstPeak.last()) <= 1);
        QVERIFY(qAbs(secondPeak.size() - firstPeak.size()) <= 2);
        QCOMPARE(warnings.count(), 0);
    }

    void connectionHeartbeatResizeKeepsSpacing() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"QML(
            import QtQuick
            import "qrc:/qml/components"
            import "qrc:/qml/controls"
            Window {
                width: 240; height: 80; visible: true
                color: Theme.inset
                property int beatCount: monitor.beats.length
                ConnectionHeartbeat {
                    id: monitor
                    objectName: "heartbeat"
                    x: 10; y: 20
                    width: parent.width - 20
                    millisecondsPerPixel: 20
                    connected: true
                }
            }
        )QML");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto *heartbeat = visualItem(window->contentItem(), "heartbeat");
        QVERIFY(heartbeat);
        QTRY_VERIFY(heartbeat->property("sweepStarted").toDouble() > 0);
        const auto started = heartbeat->property("sweepStarted").toDouble();
        const auto flat = window->grabWindow();
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTRY_COMPARE(window->property("beatCount").toInt(), 1);
        QTest::qWait(600);
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTRY_COMPARE(window->property("beatCount").toInt(), 2);
        QTest::qWait(600);

        const auto peakCentres = [&](const QImage &frame) {
            QList<int> centres;
            const auto ratio = window->devicePixelRatio();
            const int bottom = qRound((heartbeat->mapToScene(QPointF(0,
                heartbeat->property("baselineY").toReal())).y() - 12) * ratio);
            // Compare the shared plot area, excluding the changing right edge
            // where resizing adds or removes the dotted background.
            const int left = qRound((heartbeat->x() + 2) * ratio);
            const int right = qRound((heartbeat->x() + qMin(qreal(220), heartbeat->width()) - 4) * ratio);
            int first = -1;
            for (int x = left; x <= right; ++x) {
                bool hasPeak = false;
                for (int y = 0; x < right && y < bottom; ++y) {
                    // Grid antialiasing changes slightly with the Shape's size.
                    // Only the bright trace should count as a peak.
                    if (qGray(frame.pixel(x, y)) - qGray(flat.pixel(x, y)) > 60) {
                        hasPeak = true;
                        break;
                    }
                }
                if (hasPeak && first < 0)
                    first = x;
                if (!hasPeak && first >= 0) {
                    centres.append((first + x - 1) / 2);
                    first = -1;
                }
            }
            return centres;
        };
        // Ignore only the baseline endpoints outside the visible graph.
        const auto beatGeometry = [](const QString &path) {
            const int start = path.indexOf(" L ");
            return path.mid(start, path.lastIndexOf(" L ") - start);
        };
        const auto geometry = beatGeometry(heartbeat->property("currentPath").toString());
        const auto before = window->grabWindow();
        const auto peaks = peakCentres(before);
        QCOMPARE(peaks.size(), 2);
        QVERIFY(!geometry.isEmpty());

        // Grow and shrink mid-pass, including below the original 220 px width.
        for (const int width : {440, 165}) {
            window->setWidth(width + 20);
            QTRY_COMPARE(heartbeat->width(), qreal(width));
            QTest::qWait(100); // Let the render-thread clips resume.
            QCOMPARE(heartbeat->property("historyMs").toInt(), width * 20);
            QCOMPARE(heartbeat->property("sweepStarted").toDouble(), started);
            QCOMPARE(beatGeometry(heartbeat->property("currentPath").toString()), geometry);
            const auto resized = window->grabWindow();
            if (!qEnvironmentVariableIsEmpty("WHAT_CONNECTION_PREVIEW")) {
                const auto prefix = qEnvironmentVariable("WHAT_CONNECTION_PREVIEW");
                QVERIFY(before.save(prefix + ".before.png"));
                QVERIFY(resized.save(prefix + "." + QString::number(width) + ".png"));
            }
            const auto resizedPeaks = peakCentres(resized);
            QCOMPARE(resizedPeaks.size(), peaks.size());
            for (int peak = 0; peak < peaks.size(); ++peak)
                QVERIFY(qAbs(resizedPeaks[peak] - peaks[peak]) <= 1);
        }

        // A resize can be rendered before the restart timer runs. Keep the
        // newest beat ahead of the tip hidden during that intermediate frame.
        heartbeat->setWidth(165); // Hold the plot size while making room in its window.
        window->setWidth(460);
        QTest::qWait(100);
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTRY_COMPARE(window->property("beatCount").toInt(), 3);
        const auto beforeResize = peakCentres(window->grabWindow());
        heartbeat->setWidth(440);
        const auto duringResize = window->grabWindow();
        if (!qEnvironmentVariableIsEmpty("WHAT_CONNECTION_PREVIEW"))
            QVERIFY(duringResize.save(qEnvironmentVariable("WHAT_CONNECTION_PREVIEW") + ".during.png"));
        QCOMPARE(peakCentres(duringResize).size(), beforeResize.size());

        QTRY_VERIFY_WITH_TIMEOUT(heartbeat->property("sweepStarted").toDouble() > started, 9000);
        const auto previous = beatGeometry(heartbeat->property("previousPath").toString());
        QVERIFY(!previous.isEmpty());
        heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
        QTRY_COMPARE(window->property("beatCount").toInt(), 4);
        window->setWidth(620);
        heartbeat->setWidth(600);
        // Widening must neither stretch the old pass nor copy new replies into it.
        QCOMPARE(beatGeometry(heartbeat->property("previousPath").toString()), previous);
        QCOMPARE(warnings.count(), 0);
    }

    void connectionHeartbeatRetainsTrace() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"QML(
            import QtQuick
            import "qrc:/qml/components"
            import "qrc:/qml/controls"
            Window {
                width: 680; height: 80; visible: true
                color: Theme.inset
                ConnectionHeartbeat {
                    id: monitor
                    objectName: "heartbeat"
                    anchors.centerIn: parent
                    width: 640
                    millisecondsPerPixel: 7000 / 640
                    connected: true
                }
            }
        )QML");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto *heartbeat = visualItem(window->contentItem(), "heartbeat");
        QVERIFY(heartbeat);
        QTRY_VERIFY(heartbeat->property("sweepStarted").toDouble() > 0);
        const auto started = heartbeat->property("sweepStarted").toDouble();
        const auto flat = window->grabWindow();
        const auto hasPeakAt = [&](const QImage &image, int column) {
            const auto ratio = window->devicePixelRatio();
            const int bottom = qRound(
                (heartbeat->mapToScene(QPointF(0, heartbeat->property("baselineY").toReal())).y() - 12) * ratio);
            for (int x = column - 2; x <= column + 2; ++x)
                for (int y = 0; y < bottom; ++y)
                    if (image.pixel(x, y) != flat.pixel(x, y))
                        return true;
            return false;
        };

        // More than 16 actual replies can arrive before the sweep has erased
        // them, especially when multiple transports or entity lookups respond.
        int firstPeak = -1;
        for (int reply = 0; reply < 20; ++reply) {
            heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
            QTest::qWait(280);
            if (reply == 4) {
                const auto image = window->grabWindow();
                for (int x = 3; x < image.width() - 3; ++x) {
                    if (hasPeakAt(image, x)) {
                        firstPeak = x;
                        break;
                    }
                }
                QVERIFY(firstPeak >= 0);
            }
        }
        QCOMPARE(heartbeat->property("sweepStarted").toDouble(), started);
        QVERIFY2(hasPeakAt(window->grabWindow(), firstPeak),
                 "A beat disappeared before the drawing tip could overwrite it.");

        QTRY_VERIFY_WITH_TIMEOUT(heartbeat->property("sweepStarted").toDouble() > started, 3000);
        const auto previousPath = heartbeat->property("previousPath").toString();
        QVERIFY(!previousPath.isEmpty());
        const auto nextStarted = heartbeat->property("sweepStarted").toDouble();
        for (int reply = 0; reply < 4; ++reply) {
            heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
            QTest::qWait(280);
            QCOMPARE(heartbeat->property("previousPath").toString(), previousPath);
            QCOMPARE(heartbeat->property("sweepStarted").toDouble(), nextStarted);
        }

        // Losing and recovering a transport changes colour, not recorded history.
        const auto currentPath = heartbeat->property("currentPath").toString();
        heartbeat->setProperty("connected", false);
        QCOMPARE(heartbeat->property("previousPath").toString(), previousPath);
        QCOMPARE(heartbeat->property("currentPath").toString(), currentPath);
        heartbeat->setProperty("connected", true);
        QCOMPARE(heartbeat->property("sweepStarted").toDouble(), nextStarted);

        // Continue through further wraps while new replies arrive. A path can
        // retain its data yet still disappear if its moving clips reset early.
        for (int pass = 0; pass < 2; ++pass) {
            const auto passStarted = heartbeat->property("sweepStarted").toDouble();
            const auto oldTrace = heartbeat->property("previousPath").toString();
            QElapsedTimer limit;
            limit.start();
            while (heartbeat->property("sweepStarted").toDouble() == passStarted && limit.elapsed() < 9000) {
                heartbeat->setProperty("timestamp", double(QDateTime::currentMSecsSinceEpoch()));
                QTest::qWait(280);
                if (heartbeat->property("sweepStarted").toDouble() == passStarted)
                    QCOMPARE(heartbeat->property("previousPath").toString(), oldTrace);
                const auto image = window->grabWindow();
                bool visiblePeak = false;
                for (int x = 3; x < image.width() - 3 && !visiblePeak; ++x)
                    visiblePeak = hasPeakAt(image, x);
                QVERIFY2(visiblePeak, "The graph went blank during a continuing stream of replies.");
            }
            QVERIFY(heartbeat->property("sweepStarted").toDouble() > passStarted);
        }
        QCOMPARE(warnings.count(), 0);
    }

    void overviewConnectionProgressStates() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        QVERIFY(local.server.isListening());
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *title = window->findChild<QQuickItem *>("connectionSummaryStatus");
        auto *spinner = window->findChild<QQuickItem *>("connectionProgressSpinner");
        auto *timing = window->findChild<QQuickItem *>("connectionAttemptTiming");
        QVERIFY(title);
        QVERIFY(spinner);
        QVERIFY(timing);
        QTRY_COMPARE(title->property("text").toString(), QString("0 / 1 Home Assistant Installations Connected"));
        QVERIFY(!local.connectionProgress()["error"].toBool());
        QTRY_VERIFY(spinner->property("running").toBool());
        QTRY_VERIFY(timing->property("text").toString().startsWith("Waiting "));
        if (!qEnvironmentVariableIsEmpty("WHAT_CONNECTION_PREVIEW"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_CONNECTION_PREVIEW")));

        QWebSocket socket;
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        socket.setSslConfiguration(ssl);
        socket.open(QUrl(QString("wss://127.0.0.1:%1").arg(local.activePort)));
        QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
        socket.sendTextMessage(
            QJsonDocument(QJsonObject{{"type", "auth"}, {"protocol", 2}, {"token", local.token}})
                .toJson(QJsonDocument::Compact));
        QTRY_COMPARE(title->property("text").toString(), QString("1 / 1 Home Assistant Installations Connected"));
        QTRY_VERIFY(!spinner->property("running").toBool());
        QVERIFY(!timing->isVisible());
        auto *lastReply = window->findChild<QObject *>("haLastCommunication");
        const auto id = local.installations().first().toMap()["id"].toString();
        auto *trace = visualItem(window->contentItem(), "installationHeartbeat_" + id);
        QVERIFY(trace);
        auto *pulse = trace->findChild<QObject *>("haHeartbeatPulse");
        QVERIFY(lastReply && pulse);
        emit local.server.feedbackSupport(true);
        emit local.server.haCommunication(QDateTime::currentMSecsSinceEpoch());
        emit local.server.installationState(id, {{"clients", 1}, {"feedback", true},
            {"lastCommunication", QDateTime::currentMSecsSinceEpoch()}});
        QTRY_VERIFY(lastReply->property("text").toString().startsWith("Last Home Assistant response: 0 s"));
        QTRY_VERIFY(pulse->property("running").toBool());
        if (!qEnvironmentVariableIsEmpty("WHAT_CONNECTION_PREVIEW")) {
            QTest::qWait(300);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_CONNECTION_PREVIEW") + ".heartbeat.png"));
        }

        auto *monitor = visualItem(window->contentItem(), "overviewHeartbeat");
        auto *replyLabel = qobject_cast<QQuickItem *>(lastReply);
        QVERIFY(monitor && replyLabel);
        const auto originalWidth = window->width();
        const auto graphWidth = monitor->width();
        const auto history = monitor->property("historyMs").toInt();
        window->setWidth(originalWidth + 480);
        QTRY_VERIFY(monitor->width() >= graphWidth + 400);
        QVERIFY(replyLabel->width() <= 320);
        QVERIFY(monitor->property("historyMs").toInt() > history);
        QVERIFY(qAbs(replyLabel->x() + replyLabel->width() - monitor->parentItem()->width()) < 1);
        if (!qEnvironmentVariableIsEmpty("WHAT_CONNECTION_PREVIEW")) {
            QTest::qWait(100); // Let the render-thread clips resume at the new width.
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_CONNECTION_PREVIEW") + ".wide.png"));
        }
        window->setWidth(originalWidth);

        socket.close();
        QTRY_COMPARE(title->property("text").toString(), QString("0 / 1 Home Assistant Installations Connected"));
        QTRY_VERIFY(pulse->property("running").toBool());

        emit local.server.installationState(id, {{"clients", 0}, {"phase", "retrying"}, {"status", "Retrying"}});
        QTRY_COMPARE(title->property("text").toString(), QString("0 / 1 Home Assistant Installations Connected"));
        QVERIFY(local.setInstallationEnabled(id, false));
        QTRY_COMPARE(title->property("text").toString(), QString("No Home Assistant Installations Enabled"));
        local.stopService();
        QTRY_COMPARE(title->property("text").toString(), QString("Home Assistant Connections Stopped"));
    }

    void windowRestoreFromTrayAndSecondLaunch() {
        QQuickWindow window;
        window.resize(700, 500);
        WindowFrame frame(reinterpret_cast<HWND>(window.winId()));
        frame.openRequested = [&] { restoreWhatWindow(&window); };
        int duplicatePrompts = 0;
        frame.alreadyRunningRequested = [&] { ++duplicatePrompts; };
        window.setPosition(-32000, -32000);
        window.hide();
        QVERIFY(notifyExistingWhatWindow(QCoreApplication::applicationPid()));
        QTRY_VERIFY(window.isVisible());
        QCOMPARE(duplicatePrompts, 1);
        QTRY_VERIFY(QGuiApplication::primaryScreen()->availableGeometry().contains(window.position()));
        window.showMinimized();
        QTest::qWait(100);
        restoreWhatWindow(&window);
        QTRY_VERIFY(window.visibility() != QWindow::Minimized);
        QVERIFY(IsWindowVisible(reinterpret_cast<HWND>(window.winId())));
        QVERIFY(!IsIconic(reinterpret_cast<HWND>(window.winId())));
        window.showMaximized();
        QTest::qWait(100);
        window.hide();
        restoreWhatWindow(&window);
        QTRY_VERIFY(window.isVisible());
        QCOMPARE(window.windowState(), Qt::WindowMaximized);
        QCOMPARE(duplicatePrompts, 1); // Normal restores must not report a second launch.
        window.hide();
    }

    void diagnosticsCoversModulesAndSharing() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.settings.setValue("mqtt/preferred", true);
        local.settings.setValue("mqtt/options", QVariantMap{{"enabled", true},
                                                            {"scope", "local"},
                                                            {"tls", false},
                                                            {"host", "PRIVATE-BROKER"},
                                                            {"username", "PRIVATE-USER"}});
        local.beacnData = {{"status", "Fixture BEACN ready"},
                           {"connected", true},
                           {"ready", true},
                           {"api_version", "1.2.3"},
                           {"last_command", "PRIVATE-COMMAND"}};
        local.wallpaperData = {{"status", "Fixture Wallpaper ready"},
                               {"running", true},
                               {"current_wallpaper", "PRIVATE-WALLPAPER"}};
        local.catalog.insert("fixture", {{"id", "fixture"}, {"source", "beacn"}, {"available", false}});
        local.selected.insert("fixture");
        local.publishingPolicies["fixture"] = QJsonObject{{"scope", 3}, {"high", true}};
        QVERIFY(saveMqtt(local, {{"host", "PRIVATE-BROKER"}, {"port", 1883}, {"tls", false}, {"scope", "local"}, {"username", "PRIVATE-USER"}, {"enabled", false}}, {}));
        const auto build = local.captureDiagnostics(false);
        // The worker must see a stable snapshot even if live state changes before it runs.
        local.beacnData["status"] = "New status";
        const auto text = build();
        for (const auto *name : {"Windows", "HWiNFO", "Core Temp", "NVIDIA NVML", "iCUE RGB and Automation",
                                 "BEACN", "Wallpaper Engine", "Dynamic Lighting", "media sample age"})
            QVERIFY2(text.contains(name), name);
        QVERIFY(text.contains("Fixture BEACN ready"));
        QVERIFY(text.contains("Fixture Wallpaper ready"));
        QVERIFY(text.contains("\"local sharing selected\": 1"));
        QVERIFY(text.contains("\"cloud sharing selected\": 1"));
        QVERIFY(text.contains("\"fast updates selected\": 1"));
        QVERIFY(text.contains("\"type\": \"mqtt\""));
        QVERIFY(text.contains("\"mqtt_scope\": \"local\""));
        QVERIFY(text.contains("\"mqtt_tls\": false"));
        for (const auto *secret : {"PRIVATE-BROKER", "PRIVATE-USER", "PRIVATE-COMMAND", "PRIVATE-WALLPAPER"})
            QVERIFY2(!text.contains(secret), secret);
    }

    void alreadyRunningUsesTimedNotice() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *timer = window->findChild<QObject *>("noticeTimer");
        QVERIFY(timer);
        QVERIFY(QMetaObject::invokeMethod(window, "showAlreadyRunning"));
        QCOMPARE(window->property("feedback").toString(),
                 "WHAT is already running. Your existing window has been restored.");
        QCOMPARE(timer->property("interval").toInt(), 20000);
        QVERIFY(timer->property("running").toBool());
        QVERIFY(!window->findChild<QObject *>("alreadyRunningPrompt"));
        QVERIFY(QMetaObject::invokeMethod(window, "showAlreadyRunning"));
        QCOMPARE(timer->property("interval").toInt(), 20000);
        // Expire the notice without holding the test open for twenty seconds.
        timer->setProperty("interval", 1);
        QTRY_VERIFY(window->property("feedback").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(window, "notify", Q_ARG(QVariant, "Another notice"),
                                          Q_ARG(QVariant, QVariant())));
        QCOMPARE(timer->property("interval").toInt(), local.uiTimings().value("toast").toInt());
        window->hide();
    }

    void trayUiAppearanceAndModality() {
        // Give the synthetic tray popup an active owner, as a native tray click would.
        QWidget owner;
        owner.resize(320, 200);
        owner.show();
        owner.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&owner));
        QMenu menu(&owner);
        styleWhatTrayMenu(&menu, QIcon(":/assets/app-icon.ico"));
        menu.addAction("Open WHAT");
        menu.addAction("System Tray Manager");
        menu.addAction("Check for updates…");
        menu.addSeparator();
        menu.addAction("Start WebSocket server")->setEnabled(false);
        menu.addAction("Stop WebSocket server");
        menu.addAction("Restart WebSocket server");
        menu.addSeparator();
        menu.addAction("Quit WHAT");
        popupWhatTrayMenu(&menu);
        QTRY_VERIFY(menu.isVisible());
        const auto folder = qEnvironmentVariable("WHAT_TRAY_PREVIEW");
        if (!folder.isEmpty())
            menu.grab().save(folder + "/tray-menu.png");
        menu.hide();
        QTest::qWait(100);
        QDialog modal(&owner);
        modal.setWindowModality(Qt::ApplicationModal);
        modal.show();
        QTRY_VERIFY(modal.isVisible());
        popupWhatTrayMenu(&menu);
        QTest::qWait(50);
        QVERIFY(!menu.isVisible());
        modal.close();
    }

    void pageDraggingAndExpansion_data() {
        QTest::addColumn<int>("frameDelay");
        QTest::newRow("normal-frames") << 0;
        QTest::newRow("delayed-frames") << 40;
    }

    void pageDraggingAndExpansion() {
        QFETCH(int, frameDelay);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlApplicationEngine::warnings);
        engine.loadData(R"QML(
            import QtQuick
            import QtQuick.Controls.Basic
            import QtQuick.Layouts
            import "qrc:/qml/controls"
            ApplicationWindow {
                width: 440; height: 300; visible: true
                Component.onCompleted: {
                    Theme.animationsEnabled = true;
                    Theme.timings = Object.assign({}, Theme.timings, {panels: 600});
                }
                DragScrollView {
                    id: view
                    objectName: "dragView"
                    anchors.fill: parent
                    contentWidth: availableWidth
                    Column {
                        width: view.availableWidth
                        InputField {
                            objectName: "dragText"
                            text: "Text selection still works"
                            width: 260
                            selectByMouse: true
                        }
                        ValueSlider { objectName: "dragSlider"; width: 260; value: 0.2 }
                        Item { width: 1; height: 130 }
                        SettingsSection {
                            objectName: "dragSection"
                            title: "Test Expansion"
                            width: parent.width
                            Label { text: "Section contents"; Layout.preferredHeight: 350 }
                        }
                        Item { width: 1; height: 500 }
                    }
                }
            }
        )QML");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *view = visualItem(window->contentItem(), "dragView");
        QVERIFY(view);
        auto *flick = view->property("contentItem").value<QQuickItem *>();
        QVERIFY(flick);
        auto drag = [&](QPoint start, QPoint distance) {
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            for (int step = 1; step <= 6; ++step)
                QTest::mouseMove(window, start + distance * step / 6, 16);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start + distance);
        };
        drag(QPoint(400, 180), QPoint(0, -100));
        QVERIFY(flick->property("contentY").toReal() > 40);
        flick->setProperty("contentY", 0);
        auto *field = visualItem(window->contentItem(), "dragText");
        QVERIFY(field);
        drag(field->mapToScene(QPointF(15, 18)).toPoint(), QPoint(140, 0));
        QVERIFY(!field->property("selectedText").toString().isEmpty());
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
        auto *slider = visualItem(window->contentItem(), "dragSlider");
        QVERIFY(slider);
        drag(slider->mapToScene(QPointF(55, 18)).toPoint(), QPoint(150, 0));
        QVERIFY(slider->property("value").toReal() > 0.6);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);

        auto *section = visualItem(window->contentItem(), "dragSection");
        auto *toggle = visualItem(window->contentItem(), "Test ExpansionSectionToggle");
        QVERIFY(section && toggle);
        const auto collapsedHeight = section->height();
        if (frameDelay) {
            // Model a busy GUI frame without depending on machine load.
            connect(section, &QQuickItem::heightChanged, section, [frameDelay] {
                QThread::msleep(frameDelay);
            });
        }
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        // A fixed sleep can end after height changes but before the queued
        // post-layout scroll update. Require movement during expansion instead.
        QTRY_VERIFY2_WITH_TIMEOUT(section->height() > collapsedHeight
                                     && section->property("expansion").toReal() < 1
                                     && flick->property("contentY").toReal() > 0,
                                 "The viewport must move while the section is still opening", 2000);
        // User input wins over the remaining reveal motion.
        drag(QPoint(400, 180), QPoint(0, 70));
        const auto afterDrag = flick->property("contentY").toReal();
        QTest::qWait(650);
        QCOMPARE(flick->property("contentY").toReal(), afterDrag);

        // A navigation shortcut can request a section before its page appears.
        view->setVisible(false);
        flick->setProperty("contentY", 0);
        QVERIFY(QMetaObject::invokeMethod(section, "reveal"));
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
        view->setVisible(true);
        QTRY_VERIFY(flick->property("contentY").toReal() > 0);

        // Closing that section before the page appears cancels its request.
        view->setVisible(false);
        flick->setProperty("contentY", 0);
        QVERIFY(QMetaObject::invokeMethod(section, "reveal"));
        section->setProperty("expanded", false);
        view->setVisible(true);
        QTRY_COMPARE(section->height(), collapsedHeight);
        QCOMPARE(flick->property("contentY").toReal(), 0.0);
        QCOMPARE(warnings.count(), 0);
    }

    void nestedScrollWheelRouting() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlApplicationEngine::warnings);
        engine.loadData(R"QML(
            import QtQuick
            import QtQuick.Controls.Basic
            import "qrc:/qml/controls"
            ApplicationWindow {
                width: 500; height: 400; visible: true
                Component.onCompleted: Theme.animationsEnabled = false
                DragScrollView {
                    id: outer
                    objectName: "outerScroll"
                    anchors.fill: parent
                    contentWidth: availableWidth
                    Column {
                        width: outer.availableWidth
                        Item { width: 1; height: 30 }
                        DragScrollView {
                            objectName: "innerScroll"
                            width: 350; height: 140
                            contentWidth: availableWidth
                            Column {
                                width: 350
                                Repeater { model: 20; Label { height: 30; text: "Output line" } }
                            }
                        }
                        Item { width: 1; height: 900 }
                    }
                }
            }
        )QML");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto *outer = visualItem(window->contentItem(), "outerScroll")->property("contentItem").value<QQuickItem *>();
        auto *inner = visualItem(window->contentItem(), "innerScroll")->property("contentItem").value<QQuickItem *>();
        QVERIFY(outer && inner);
        auto wheel = [&](int delta) {
            const QPointF point(200, 80);
            QWheelEvent event(point, window->mapToGlobal(point.toPoint()), QPoint(), QPoint(0, delta),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(window, &event);
        };
        wheel(-120);
        QTRY_VERIFY(inner->property("contentY").toReal() > 0);
        QCOMPARE(outer->property("contentY").toReal(), 0.0);
        const auto end = inner->property("contentHeight").toReal() - inner->height();
        inner->setProperty("contentY", end);
        wheel(-120);
        QTRY_VERIFY(outer->property("contentY").toReal() > 0);
        QCOMPARE(inner->property("contentY").toReal(), end);
        QCOMPARE(warnings.count(), 0);
    }

    void expansionScrollTracksVisibleEdge() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlApplicationEngine::warnings);
        engine.loadData(R"QML(
            import QtQuick
            import QtQuick.Controls.Basic
            import "qrc:/qml/controls"
            ApplicationWindow {
                width: 440; height: 300; visible: true
                Component.onCompleted: {
                    Theme.animationsEnabled = true;
                    Theme.timings = Object.assign({}, Theme.timings, {panels: 2000});
                }
                DragScrollView {
                    id: view
                    objectName: "edgeView"
                    anchors.fill: parent
                    contentWidth: availableWidth
                    Column {
                        width: view.availableWidth
                        Item { width: 1; height: 220 }
                        Item {
                            id: section
                            objectName: "edgeSection"
                            width: parent.width
                            height: 64
                            function reveal() { scroll.follow(); }
                            ExpansionScroll { id: scroll; targetItem: section; expanded: true }
                        }
                        Item { width: 1; height: 500 }
                    }
                }
            }
        )QML");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto *view = visualItem(window->contentItem(), "edgeView");
        auto *section = visualItem(window->contentItem(), "edgeSection");
        QVERIFY(view && section);
        auto *flick = view->property("contentItem").value<QQuickItem *>();
        QVERIFY(flick);
        const auto *content = flick->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        QVERIFY(QMetaObject::invokeMethod(section, "reveal"));
        // Hold each visible height still while its queued layout update runs.
        // A second easing animation cannot catch up within this short bound.
        for (qreal height : {110.0, 180.0, 250.0, 400.0}) {
            section->setHeight(height);
            const auto top = section->mapToItem(content, QPointF()).y();
            const auto expected = qMin(top, qMax(0.0, top + height + 12 - flick->height()));
            QTRY_VERIFY2_WITH_TIMEOUT(qAbs(flick->property("contentY").toReal() - expected) < 0.5,
                qPrintable(QString("Scroll must follow the visible edge: height=%1, expected=%2, actual=%3")
                    .arg(height).arg(expected).arg(flick->property("contentY").toReal())), 150);
        }
        QCOMPARE(warnings.count(), 0);
        window->hide();
    }

    void installerDefaultColours() {
        // Evaluate the default QML palette, including theme-dependent bindings.
        // Parsing its expressions as colour literals broke when themes were added.
        QQmlApplicationEngine engine;
        engine.loadData(R"QML(
            import QtQuick
            import "qrc:/qml/controls"
            QtObject { property QtObject theme: Theme }
        )QML");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *theme = engine.rootObjects().first()->property("theme").value<QObject *>();
        QVERIFY(theme);
        QCOMPARE(theme->property("name").toString(), QString("blue"));

        QFile setup(":/packaging/Setup.cs");
        QVERIFY(setup.open(QIODevice::ReadOnly));
        const auto source = QString::fromUtf8(setup.readAll());
        const QRegularExpression declarations(
            R"(static\s+readonly\s+Color\s+(\w+)\s*=\s*([^;]+);)");
        const QRegularExpression rgb(
            R"(^Color\.FromArgb\(\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\s*\)$)");
        auto colours = declarations.globalMatch(source);
        QVERIFY(colours.hasNext());
        while (colours.hasNext()) {
            const auto declaration = colours.next();
            auto role = declaration.captured(1);
            role[0] = role[0].toLower();
            const auto value = rgb.match(declaration.captured(2).trimmed());
            QVERIFY2(value.hasMatch(), qPrintable("Unsupported installer colour expression: " + role));
            const QColor expected(value.captured(1).toInt(nullptr, 0),
                                  value.captured(2).toInt(nullptr, 0),
                                  value.captured(3).toInt(nullptr, 0));
            const auto actual = theme->property(role.toUtf8().constData()).value<QColor>();
            QVERIFY2(actual == expected,
                     qPrintable(QString("Installer %1 is %2; default QML theme is %3")
                                    .arg(role, expected.name(), actual.name())));
        }
    }

    void uiThemeSelectionAndPersistence() {
        QTemporaryDir dir;
        const auto originalPalette = QApplication::palette();
        const auto restorePalette = qScopeGuard([&] { QApplication::setPalette(originalPalette); });
        {
            Controller local(dir.path(), true);
            QCOMPARE(local.uiTheme(), QString("blue"));
            local.setAnimationsEnabled(false);
            UpdateManager updater(dir.path(), true);
            QQmlApplicationEngine engine;
            engine.setInitialProperties(
                {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
            engine.load(QUrl("qrc:/qml/Main.qml"));
            QVERIFY(!engine.rootObjects().isEmpty());
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            QVERIFY(window);
            auto *background = window;
            window->show();
            QVERIFY(QTest::qWaitForWindowExposed(window));
            window->setProperty("previewPage", 2);
            auto *search = visualItem(window->contentItem(), "settingsSearch");
            QVERIFY(search);
            search->setProperty("text", "theme");
            auto *appearance = visualItem(window->contentItem(), "AppearanceSectionToggle");
            QVERIFY(appearance);
            QVERIFY(QMetaObject::invokeMethod(appearance, "clicked"));
            auto *selector = visualItem(window->contentItem(), "themeSelector");
            QVERIFY(selector);
            QTRY_VERIFY(selector->isVisible());
            QCOMPARE(background->property("color").value<QColor>(), QColor("#10151e"));
            QMenu menu;
            styleWhatTrayMenu(&menu, QIcon(":/assets/app-icon.ico"));
            menu.addAction("Open WHAT");
            menu.addAction("Quit WHAT");
            menu.ensurePolished();
            menu.resize(menu.sizeHint());
            QCOMPARE(menu.grab().toImage().pixelColor(5, 5), background->property("color").value<QColor>());
            std::unique_ptr<QWidget> notice(createStartupNotice([] {}));
            auto *card = notice->findChild<QFrame *>("noticeCard");
            QVERIFY(card);

            // Exercise the actual selector, including its activation handler.
            selector->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Down);
            QTRY_COMPARE(local.uiTheme(), QString("dark-grey"));
            QTRY_COMPARE(background->property("color").value<QColor>(), QColor("#1b1b1b"));
            QTRY_COMPARE(QApplication::palette().color(QPalette::Window), background->property("color").value<QColor>());
            QTRY_COMPARE(menu.grab().toImage().pixelColor(5, 5), background->property("color").value<QColor>());
            QTRY_COMPARE(card->grab().toImage().pixelColor(5, 5), QColor("#222222"));
            const auto preview = qEnvironmentVariable("WHAT_THEME_PREVIEW");
            if (!preview.isEmpty()) {
                QVERIFY(window->grabWindow().save(preview + "/grey-settings.png"));
                QVERIFY(menu.grab().save(preview + "/grey-menu.png"));
                QVERIFY(notice->grab().save(preview + "/grey-notice.png"));
            }

            QTest::keyClick(window, Qt::Key_Up);
            QTRY_COMPARE(local.uiTheme(), QString("blue"));
            QTRY_COMPARE(background->property("color").value<QColor>(), QColor("#10151e"));
            QTRY_COMPARE(QApplication::palette().color(QPalette::Window), background->property("color").value<QColor>());
            QTRY_COMPARE(menu.grab().toImage().pixelColor(5, 5), background->property("color").value<QColor>());
            if (!preview.isEmpty())
                QVERIFY(window->grabWindow().save(preview + "/blue-settings.png"));

            for (const auto &theme : {"slate", "forest", "plum"}) {
                local.setUiTheme(theme);
                QTRY_COMPARE(local.uiTheme(), QString(theme));
                QTRY_COMPARE(QApplication::palette().color(QPalette::Window), background->property("color").value<QColor>());
                QVERIFY(background->property("color").value<QColor>() != QColor("#10151e"));
                const auto roundTrip = ConfigurationFile::decode(ConfigurationFile::encode(
                    ConfigurationFile::snapshot(local.settings)));
                QVERIFY(roundTrip.error.isEmpty());
                QCOMPARE(roundTrip.values["ui/theme"].toString(), QString(theme));
                if (!preview.isEmpty())
                    QVERIFY(window->grabWindow().save(preview + "/" + theme + "-settings.png"));
            }
            local.setUiTheme("dark-grey");
            local.setUiTheme("unknown");
            QCOMPARE(local.uiTheme(), QString("dark-grey"));
            const auto exported = ConfigurationFile::snapshot(local.settings);
            const auto imported = ConfigurationFile::decode(ConfigurationFile::encode(exported));
            QVERIFY(imported.error.isEmpty());
            QCOMPARE(imported.values["ui/theme"].toString(), QString("dark-grey"));
            QVERIFY(!ConfigurationFile::decode(
                ConfigurationFile::encode({{"ui/theme", "unknown"}})).error.isEmpty());

            window->setProperty("previewPage", 7);
            auto *warning = visualItem(window->contentItem(), "iconTextWarning");
            QVERIFY(warning);
            QTRY_VERIFY(warning->isVisible());
            QVERIFY(warning->property("text").toString().contains("1-2 characters"));
            QVERIFY(warning->height() >= warning->implicitHeight());
            if (!preview.isEmpty())
                QVERIFY(window->grabWindow().save(preview + "/grey-taskbar.png"));
            auto *picker = window->findChild<QObject *>("traySensorPicker");
            QVERIFY(picker);
            for (const auto &name : {"addTraySensorFooter", "addAppIconFooter"}) {
                auto *add = visualItem(window->contentItem(), name);
                QVERIFY(add && add->isVisible());
                QVERIFY(QMetaObject::invokeMethod(add, "clicked"));
                QTRY_VERIFY(picker->property("visible").toBool());
                QCOMPARE(picker->property("mode").toString(),
                         QString(name).contains("Tray") ? QString("tray") : QString("app"));
                QVERIFY(QMetaObject::invokeMethod(picker, "close"));
                QTRY_VERIFY(!picker->property("visible").toBool());
            }
            window->hide();
        }
        Controller restored(dir.path(), true);
        QCOMPARE(restored.uiTheme(), QString("dark-grey"));
    }

    void uiTimingsPersistClampAndReset() {
        QTemporaryDir dir;
        {
            QSettings previous(dir.filePath("test-settings.yaml"), SettingsStore::format());
            previous.setValue("ui/timing/tooltips", 2000);
        }
        Controller local(dir.path(), true);
        QCOMPARE(local.uiTimings()["logo"].toInt(), 1500);
        QVERIFY(!local.uiTimings().contains("tooltips"));
        QVERIFY(!local.settings.contains("ui/timing/tooltips"));
        local.setUiTiming("tooltips", 2000);
        QVERIFY(!local.settings.contains("ui/timing/tooltips"));
        const auto oldBackup = ConfigurationFile::decode(ConfigurationFile::encode(
            {{"ui/timing/tooltips", 350}, {"ui/timing/panels", 650}}));
        QVERIFY2(oldBackup.error.isEmpty(), qPrintable(oldBackup.error));
        QVERIFY(!oldBackup.values.contains("ui/timing/tooltips"));
        QCOMPARE(oldBackup.values["ui/timing/panels"].toInt(), 650);
        local.setUiTiming("panels", 650);
        local.setUiTiming("logo", 2200);
        local.setUiTiming("logoPause", 0);
        QCOMPARE(local.uiTimings()["panels"].toInt(), 650);
        QSettings persisted(local.settings.fileName(), SettingsStore::format());
        QCOMPARE(persisted.value("ui/timing/panels").toInt(), 650);
        local.setUiTiming("logo", -1);
        QCOMPARE(local.uiTimings()["logo"].toInt(), 250);
        local.setUiTiming("panels", 999999);
        QCOMPARE(local.uiTimings()["panels"].toInt(), 2000);
        for (const auto &key : {"pages", "dialogEnter", "dialogExit"}) {
            local.setUiTiming(key, 450);
            QCOMPARE(local.uiTimings()[key].toInt(), 450);
            QCOMPARE(persisted.value(QString("ui/timing/") + key).toInt(), 450);
            const QString setting = QString("ui/timing/") + key;
            const auto exported = ConfigurationFile::encode({{setting, 450}});
            QCOMPARE(ConfigurationFile::decode(exported).values[setting].toInt(), 450);
        }
        local.setUiTiming("unknown", 100);
        QVERIFY(!local.settings.contains("ui/timing/unknown"));
        local.setUiTiming("hover", 0);
        QCOMPARE(local.uiTimings()["hover"].toInt(), 0);
        local.resetUiTimings();
        QCOMPARE(local.uiTimings()["panels"].toInt(), 250);
        QCOMPARE(local.uiTimings()["dialogEnter"].toInt(), 250);
        QCOMPARE(local.uiTimings()["dialogExit"].toInt(), 180);
        QCOMPARE(local.uiTimings()["logoPause"].toInt(), 180);
        local.settings.setValue("ui/timing/hover", "invalid");
        QCOMPARE(local.uiTimings()["hover"].toInt(), 140);
    }

    void settingsSearchAndOverviewNavigation() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.setAnimationsEnabled(false);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        window->setProperty("previewPage", 2);
        std::function<QQuickItem *(QQuickItem *, QString)> find = [&](QQuickItem *p,
                                                                      QString name) -> QQuickItem * {
            if (p->objectName() == name)
                return p;
            for (auto *c : p->childItems())
                if (auto *v = find(c, name))
                    return v;
            return nullptr;
        };
        auto item = [&](QString name) { return find(window->contentItem(), name); };
        auto *search = item("settingsSearch");
        QVERIFY(search);
        search->setProperty("text", "broker password");
        auto *connection = item("Home Assistant InstallationsSectionToggle");
        QVERIFY(connection);
        QTRY_VERIFY(connection->isVisible());
        if (!connection->parentItem()->property("expanded").toBool())
            QVERIFY(QMetaObject::invokeMethod(connection, "clicked"));
        QTRY_VERIFY(item("addInstallation")->isVisible());
        QVERIFY(!item("animationsToggle")->isVisible());
        search->setProperty("text", "certificate renew");
        QTRY_VERIFY(item("Pairing and SecuritySectionToggle")->isVisible());
        QVERIFY(!item("addInstallation")->isVisible());
        search->setProperty("text", "nothing-matches-this");
        QTRY_VERIFY(item("settingsNoResults")->isVisible());
        search->setProperty("text", "");
        QVERIFY(!item("settingsNoResults")->isVisible());
        auto *appearance = item("AppearanceSectionToggle");
        auto *startup = item("StartupSectionToggle");
        QVERIFY(appearance && startup);
        QVERIFY(QMetaObject::invokeMethod(appearance, "clicked"));
        QVERIFY(appearance->parentItem()->property("expanded").toBool());
        QVERIFY(!connection->parentItem()->property("expanded").toBool());
        QVERIFY(QMetaObject::invokeMethod(startup, "clicked"));
        QVERIFY(startup->parentItem()->property("expanded").toBool());
        QVERIFY(!appearance->parentItem()->property("expanded").toBool());
        auto *scroll = item("settingsScroll");
        QVERIFY(scroll);
        auto *flick = scroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(flick);
        const QPointF searchPosition = search->mapToScene(QPointF());
        flick->setProperty("contentY", 100);
        QCOMPARE(search->mapToScene(QPointF()), searchPosition);
        QVERIFY(search->mapToItem(flick, QPointF(0, search->height())).y() <= 0);
        search->setProperty("text", "animation");
        QVERIFY(!appearance->parentItem()->property("expanded").toBool());
        search->setProperty("text", "");
        for (auto key : {"beacn", "wallpaper", "windows", "hwinfo", "icue", "coretemp", "nvidia"}) {
            QVERIFY(QMetaObject::invokeMethod(window, "openModule", Q_ARG(QVariant, QString(key))));
            QCOMPARE(window->property("previewPage").toInt(), 5);
            QVERIFY(item(QString(key) + "ModuleCard")->property("expanded").toBool());
            for (auto other : {"beacn", "wallpaper", "windows", "hwinfo", "icue", "coretemp", "nvidia"})
                if (QString(other) != key)
                    QVERIFY(!item(QString(other) + "ModuleCard")->property("expanded").toBool());
        }
        window->hide();
    }

    void entityCleanupPreviewAndConfirmation() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.confirmEntityCleanup();
        QVERIFY(local.entityCleanupRequest.isEmpty());
        for (const auto &transport : {"none", "cloud"}) {
            local.connectionTransport = transport;
            local.previewEntityCleanup();
            QVERIFY(local.entityCleanupRequest.isEmpty());
            QVERIFY(!local.entityCleanupState["busy"].toBool());
            QVERIFY(local.entityCleanupState["message"].toString().contains("local sharing"));
        }
        const auto installation = local.installations().first().toMap()["id"].toString();
        local.installationStates[installation] = {{"scope", "local"}, {"clients", 1}};
        {
            QMutexLocker lock(&local.server.worker->control->mutex);
            local.server.worker->control->installationLeases["ha-session"] = local.server.installationGuards[installation];
        }
        local.connectionTransport = "local";
        local.previewEntityCleanup();
        const auto request = local.entityCleanupRequest;
        QCOMPARE(request["operation"].toString(), QString("preview"));
        QVERIFY(local.entityCleanupState["busy"].toBool());
        const auto token = QString(32, 'a');
        QJsonObject result{{"request_id", request["id"]},
                           {"status", "preview"},
                           {"count", 1},
                           {"token", token},
                           {"entities", QJsonArray{"sensor.what_pc_old"}}};
        local.handleEntityCleanupResult("ha-session", {{"id", "reply"}, {"args", result}});
        QCOMPARE(local.entityCleanupState["status"].toString(), QString("preview"));
        QCOMPARE(local.entityCleanupToken, token);
        local.confirmEntityCleanup();
        QCOMPARE(local.entityCleanupRequest["operation"].toString(), QString("remove"));
        QCOMPARE(local.entityCleanupRequest["token"].toString(), token);
        result = {{"request_id", local.entityCleanupRequest["id"]}, {"status", "removed"}, {"count", 1}};
        local.handleEntityCleanupResult("other-session", {{"id", "reply2"}, {"args", result}});
        QVERIFY(local.entityCleanupState["busy"].toBool());
        local.handleEntityCleanupResult("ha-session", {{"id", "reply3"}, {"args", result}});
        QCOMPARE(local.entityCleanupState["status"].toString(), QString("removed"));
        QVERIFY(local.entityCleanupRequest.isEmpty());
        QVERIFY(local.entityCleanupToken.isEmpty());
        local.confirmEntityCleanup();
        QVERIFY(local.entityCleanupRequest.isEmpty());
        local.previewEntityCleanup();
        result = {{"request_id", local.entityCleanupRequest["id"]},
                  {"status", "error"},
                  {"count", 0},
                  {"message", "The selection is out of date. Request a fresh preview."}};
        local.handleEntityCleanupResult("ha-session", {{"id", "error-reply"}, {"args", result}});
        QVERIFY(!local.entityCleanupState["busy"].toBool());
        QCOMPARE(local.entityCleanupState["message"].toString(), result["message"].toString());
    }

    void entityCleanupOverLocalConnection() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        QTRY_VERIFY(local.server.isListening());
        QWebSocket socket;
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.addCaCertificate(local.tlsConfiguration.localCertificate());
        socket.setSslConfiguration(ssl);
        QJsonObject request, acknowledgement;
        connect(&socket, &QWebSocket::textMessageReceived, this, [&](const QString &text) {
            const auto frame = QJsonDocument::fromJson(text.toUtf8()).object();
            if (frame["entity_cleanup"].isObject())
                request = frame["entity_cleanup"].toObject();
            if (frame["type"] == "command_result")
                acknowledgement = frame;
        });
        socket.open(QUrl(QString("wss://127.0.0.1:%1").arg(local.activePort)));
        QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
        socket.sendTextMessage(QJsonDocument(QJsonObject{{"type", "auth"},
                                                         {"protocol", 2},
                                                         {"token", local.token},
                                                         {"commands_v1", true},
                                                         {"delta_v1", true}})
                                   .toJson(QJsonDocument::Compact));
        QTRY_COMPARE(local.connectionTransport, QString("local"));
        const QString previewToken(32, 'b');
        for (const auto &operation : {QString("preview"), QString("remove")}) {
            if (operation == "preview")
                local.previewEntityCleanup();
            else
                local.confirmEntityCleanup();
            QTRY_COMPARE(request["operation"].toString(), operation);
            QJsonObject result{{"request_id", request["id"]},
                               {"status", operation == "preview" ? "preview" : "removed"},
                               {"count", 1},
                               {"token", previewToken},
                               {"entities", QJsonArray{"sensor.what_old"}}};
            if (operation == "remove")
                QCOMPARE(request["token"].toString(), previewToken);
            socket.sendTextMessage(QJsonDocument(QJsonObject{{"type", "command"},
                                                             {"id", operation},
                                                             {"action", "entity_cleanup_result"},
                                                             {"args", result}})
                                       .toJson(QJsonDocument::Compact));
            QTRY_COMPARE(acknowledgement["id"].toString(), operation);
            QVERIFY(acknowledgement["ok"].toBool());
            QVERIFY(!local.entityCleanupState["busy"].toBool());
            QCOMPARE(local.entityCleanupState["status"].toString(), result["status"].toString());
        }
        socket.close();
    }

    void connectionChoicesArePublishedWithoutConnectionSecrets() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        QVERIFY(setRemote(local, true, "https://ha.example"));
        const auto frame = QJsonDocument(local.snapshot()).toJson();
        QVERIFY(!frame.contains("ha.example"));
        QVERIFY(!frame.contains(local.token.toUtf8()));
        QVERIFY(!frame.contains("connections/credentials"));
    }

    void cleanupSelectionPrivacy() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.selected = {"active", "disabled_module", "cloud_only"};
        auto frame = local.snapshot();
        QCOMPARE(frame["selected_sensor_ids"].toArray().size(), 3);
        PublishingPolicy policy;
        QVERIFY(policy.apply(frame, false, 0).contains("selected_sensor_ids"));
        QVERIFY(!policy.apply(frame, true, 0).contains("selected_sensor_ids"));
        QVERIFY(!policy.apply(frame, true, 0).contains("entity_cleanup"));
    }

    void qmlWholePageSlide() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.setUiTiming("pages", 800);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *stack = window->findChild<QQuickItem *>("pageStack");
        QVERIFY(stack);
        auto *container = stack->property("pageContainer").value<QQuickItem *>();
        QVERIFY(container);
        const auto pages = container->childItems();
        QCOMPARE(pages.size(), 10);
        QList<QQuickItem *> headings;
        for (auto *page : pages) {
            auto *header = page->property("header").value<QQuickItem *>();
            auto *body = page->property("contentItem").value<QQuickItem *>();
            QVERIFY(header && body);
            auto *heading = visualItem(header, "pageHeading");
            QVERIFY(heading);
            // The heading and body must inherit one transform and clipping area.
            QVERIFY(page->isAncestorOf(header));
            QVERIFY(page->isAncestorOf(body));
            QCOMPARE(heading->property("title"), page->property("title"));
            headings.append(heading);
        }
        const auto before = window->grabWindow();
        QVERIFY(!before.isNull());
        window->setProperty("previewPage", 1);
        QTRY_VERIFY(stack->property("moving").toBool());
        QVERIFY(pages[0]->isVisible() && pages[1]->isVisible());
        QCOMPARE(headings[0]->property("title").toString(), QString("Overview"));
        QCOMPARE(headings[1]->property("title").toString(), QString("Sensors"));
        QImage during;
        QTRY_VERIFY(!(during = window->grabWindow()).isNull() && during != before);
        QTRY_COMPARE(stack->property("shownIndex").toInt(), 1);
        QVERIFY(!pages[0]->isVisible());
        QVERIFY(pages[1]->isVisible());
        const auto preview = qEnvironmentVariable("WHAT_PAGE_SLIDE_PREVIEW");
        if (!preview.isEmpty()) {
            QVERIFY(before.save(preview + "/before.png"));
            QVERIFY(during.save(preview + "/during.png"));
            QVERIFY(window->grabWindow().save(preview + "/after.png"));
        }
        // A second click interrupts the slide and makes the latest page usable now.
        window->setProperty("previewPage", 5);
        QTRY_VERIFY(stack->property("moving").toBool());
        window->setProperty("previewPage", 2);
        QVERIFY(!stack->property("moving").toBool());
        QCOMPARE(stack->property("shownIndex").toInt(), 2);
        QVERIFY(pages[2]->isEnabled());
        QCOMPARE(headings[2]->property("title").toString(), QString("Settings"));
        window->setProperty("previewPage", 6);
        local.setAnimationsEnabled(false);
        QTRY_VERIFY(!stack->property("moving").toBool());
        QCOMPARE(stack->property("shownIndex").toInt(), 6);
        window->hide();
        QCOMPARE(warnings.count(), 0);
    }

    void expansionBlurRendersAndReleasesLayer() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"(
            import QtQuick
            import "qrc:/qml/controls"
            Window {
                id: window
                width: 240; height: 160; color: "steelblue"
                property real progress: 1
                property bool animations: true
                onAnimationsChanged: Theme.animationsEnabled = animations
                Item {
                    id: panel
                    objectName: "panel"
                    x: 20; y: 20; width: 200; height: 120
                    Rectangle {
                        id: frameBackground
                        anchors.fill: parent
                        color: "red"
                        border.width: 1
                        border.color: "white"
                    }
                    Rectangle {
                        x: 10; y: 10; width: 180; height: 100
                        color: "white"
                        Rectangle {
                            width: 90; height: parent.height; color: "black"
                            Rectangle {
                                x: 30; width: 1 / Screen.devicePixelRatio
                                height: parent.height; color: "white"
                            }
                        }
                    }
                }
                ShaderEffectSource {
                    id: backgroundSnapshot
                    sourceItem: blur.active ? frameBackground : null
                    live: false
                    hideSource: false
                    visible: false
                }
                ExpansionBlur {
                    id: blur
                    objectName: "blur"
                    target: panel
                    progress: window.progress
                    backgroundSource: backgroundSnapshot
                    backgroundRect: Qt.rect(0, 0, 200, 120)
                }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *blur = window->findChild<QObject *>("blur");
        auto *panel = window->findChild<QQuickItem *>("panel");
        QVERIFY(blur && panel);
        const auto sharp = window->grabWindow();
        const auto contrast = [&](const QImage &frame) {
            const auto ratio = window->devicePixelRatio();
            return frame.pixelColor(qRound(122 * ratio), qRound(80 * ratio)).red()
                - frame.pixelColor(qRound(118 * ratio), qRound(80 * ratio)).red();
        };
        QCOMPARE(contrast(sharp), 255);
        window->setProperty("progress", 0.15);
        QTRY_VERIFY(blur->property("active").toBool());
        const auto softened = window->grabWindow();
        QVERIFY(contrast(softened) < 220);
        // A one-pixel stroke should soften into one peak, not spaced copies of
        // itself. Sparse blur samples made fine text look pixelated in motion.
        const int strokeX = qRound(60 * window->devicePixelRatio());
        const int strokeY = qRound(80 * window->devicePixelRatio());
        int previous = 0;
        bool falling = false;
        for (int x = strokeX - 8; x <= strokeX + 8; ++x) {
            const int value = softened.pixelColor(x, strokeY).red();
            if (value < previous - 3)
                falling = true;
            QVERIFY2(!falling || value <= previous + 3, "Blur created duplicate edges around a thin stroke");
            previous = value;
        }
        // The content softens, but its surrounding frame stays pixel-identical.
        const auto frame = QRect(qRound(20 * window->devicePixelRatio()),
                                 qRound(20 * window->devicePixelRatio()),
                                 qRound(200 * window->devicePixelRatio()),
                                 qRound(4 * window->devicePixelRatio()));
        QCOMPARE(softened.copy(frame), sharp.copy(frame));
        window->setProperty("progress", 0.8);
        const auto clearing = window->grabWindow();
        QVERIFY(contrast(clearing) > contrast(softened));
        window->setProperty("progress", 1.0);
        QTRY_VERIFY(!blur->property("active").toBool());
        QVERIFY(!QQmlProperty::read(panel, "layer.enabled").toBool());
        QCOMPARE(window->grabWindow(), sharp);

        // Closing follows the same curve backwards. Turning animations off
        // midway must immediately restore normal rendering and release the layer.
        window->setProperty("progress", 0.15);
        QTRY_VERIFY(blur->property("active").toBool());
        QCOMPARE(window->grabWindow(), softened);
        window->setProperty("animations", false);
        QTRY_VERIFY(!blur->property("active").toBool());
        QVERIFY(!QQmlProperty::read(panel, "layer.enabled").toBool());
        QCOMPARE(window->grabWindow(), sharp);
        window->setProperty("animations", true);
        window->setProperty("progress", 0.0);
        QTRY_VERIFY(!blur->property("active").toBool());
        QVERIFY(!QQmlProperty::read(panel, "layer.enabled").toBool());
        QCOMPARE(warnings.count(), 0);
    }

    void openCloseRenderingAndIdle() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"(
            import QtQuick
            import "qrc:/qml/controls"
            Window {
                width: 400; height: 240; visible: true; color: "black"
                property real progress: 1
                property int mode: 7
                onModeChanged: Theme.transitionMode = mode
                Component.onCompleted: {
                    Theme.animationsEnabled = true;
                    Theme.transitionStyle = "refraction";
                    Theme.transitionMode = mode;
                }
                Rectangle {
                    id: panel
                    objectName: "lensPanel"
                    x: 20; y: 20; width: 360; height: 200
                    color: "steelblue"
                    Grid {
                        columns: 12
                        Repeater {
                            model: 72
                            Rectangle {
                                required property int index
                                width: 30; height: 30
                                color: index % 3 === 0 ? "lightskyblue" : "steelblue"
                            }
                        }
                    }
                    Text { anchors.centerIn: parent; text: "WHAT"; font.pixelSize: 32; color: "white" }
                }
                OpenCloseLayer {
                    objectName: "lens"
                    target: panel
                    allowed: true
                    progress: panel.Window.window.progress
                }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto *panel = window->findChild<QQuickItem *>("lensPanel");
        auto *lens = window->findChild<QObject *>("lens");
        QVERIFY(panel && lens);
        QVERIFY(!QQmlProperty::read(panel, "layer.enabled").toBool());
        const auto settled = window->grabWindow();
        window->setProperty("progress", 0.45);
        QTRY_VERIFY(lens->property("active").toBool());
        QVERIFY(QQmlProperty::read(panel, "layer.enabled").toBool());
        const auto moving = window->grabWindow();
        QVERIFY(moving != settled);
        // The middle is revealed while the corners remain closed. This also
        // catches missing shader resources, which can otherwise render blank.
        const qreal ratio = window->devicePixelRatio();
        QVERIFY(moving.pixelColor(qRound(200 * ratio), qRound(120 * ratio)) != QColor(Qt::black));
        QCOMPARE(moving.pixelColor(qRound(22 * ratio), qRound(22 * ratio)), QColor(Qt::black));
        if (!qEnvironmentVariableIsEmpty("WHAT_LENS_PREVIEW"))
            QVERIFY(moving.save(qEnvironmentVariable("WHAT_LENS_PREVIEW") + ".shader.png"));
        window->setProperty("progress", 1.0);
        QTRY_VERIFY(!lens->property("active").toBool());
        QVERIFY(!QQmlProperty::read(panel, "layer.enabled").toBool());
        QCOMPARE(window->grabWindow(), settled);
        // Every effect must actually render, close fully, and hand back to the
        // original scene at rest. Check wide and tall surfaces as well.
        for (const auto &style : UiMotion::styles) {
            if (style.mode < 0)
                continue;
            window->setProperty("mode", style.mode);
            for (const auto size : {QSize(360, 200), QSize(120, 200)}) {
                panel->setWidth(size.width());
                panel->setHeight(size.height());
                window->setProperty("progress", 1.0);
                const auto original = window->grabWindow();
                window->setProperty("progress", 0.45);
                QTRY_VERIFY(lens->property("active").toBool());
                const auto frame = window->grabWindow();
                QVERIFY2(frame != original, style.id);
                bool visiblePixels = false;
                for (int y = 25; y < 215 && !visiblePixels; y += 8)
                    for (int x = 25; x < 20 + size.width(); x += 8)
                        visiblePixels |= frame.pixelColor(qRound(x * ratio), qRound(y * ratio)) != QColor(Qt::black);
                QVERIFY2(visiblePixels, style.id);
                if (!qEnvironmentVariableIsEmpty("WHAT_MOTION_PREVIEW") && size.width() == 360)
                    QVERIFY(frame.save(qEnvironmentVariable("WHAT_MOTION_PREVIEW") + "/" + style.id + ".png"));
                window->setProperty("progress", 0.0);
                const auto closed = window->grabWindow();
                QCOMPARE(closed.pixelColor(qRound(70 * ratio), qRound(120 * ratio)), QColor(Qt::black));
                window->setProperty("progress", 1.0);
                QTRY_VERIFY(!lens->property("active").toBool());
                QVERIFY(!QQmlProperty::read(panel, "layer.enabled").toBool());
                QCOMPARE(window->grabWindow(), original);
            }
        }
        QCOMPARE(warnings.count(), 0);
    }

    void promptsAndWindowOpenImmediately() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        // Existing preferences still apply to menus, but must never
        // restore effects on the main window or any prompt.
        local.settings.setValue("ui/transition_style", "refraction");
        local.settings.setValue("ui/refraction_strength", 150);
        local.settings.setValue("ui/timing/windowEnter", 2000);
        local.settings.setValue("ui/timing/windowExit", 2000);
        local.setUiTiming("dialogEnter", 2000);
        local.setUiTiming("dialogExit", 2000);
        QCOMPARE(local.info()["transitionStyle"].toString(), QString("refraction"));
        QCOMPARE(local.info()["refractionStrength"].toInt(), 150);
        QVERIFY(!local.uiTimings().contains("windowEnter"));
        QVERIFY(!local.uiTimings().contains("windowExit"));

        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCOMPARE(window->opacity(), 1.0);
        QVERIFY(!window->findChild<QObject *>("windowRefraction"));
        QVERIFY(!window->findChild<QObject *>("transitionStyleSelector"));
        QVERIFY(!window->findChild<QObject *>("refractionStrengthInput"));
        QVERIFY(!window->findChild<QObject *>("timing-windowEnter"));
        QVERIFY(!window->findChild<QObject *>("timing-windowExit"));
        QVERIFY(!window->findChild<QObject *>("timing-tooltips"));

        // Also cover dialogs that do not inherit GlassDialog, such as the
        // installation editor, display manager and custom sensor library.
        int prompts = 0;
        for (auto *object : window->findChildren<QObject *>()) {
            if (!object->inherits("QQuickPopup")
                || (object->metaObject()->indexOfProperty("standardButtons") < 0
                    && !object->property("modal").toBool()))
                continue;
            ++prompts;
            QVERIFY2(!object->property("enter").value<QObject *>(), object->metaObject()->className());
            QVERIFY2(!object->property("exit").value<QObject *>(), object->metaObject()->className());
            QVERIFY(!object->property("popupMotion").isValid());
        }
        QVERIFY(prompts >= 10);
        auto *dialog = window->findChild<QObject *>("uiTimingDialog");
        QVERIFY(dialog);
        for (int cycle = 0; cycle < 3; ++cycle) {
            QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
            QCOMPARE(dialog->property("opened").toBool(), true);
            QCOMPARE(dialog->property("opacity").toDouble(), 1.0);
            QCOMPARE(dialog->property("scale").toDouble(), 1.0);
            QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
            QCOMPARE(dialog->property("visible").toBool(), false);
        }

        QVERIFY(QMetaObject::invokeMethod(window, "performWindowAction", Q_ARG(QVariant, "hide")));
        QVERIFY(!window->isVisible());
        WindowFrame frame(reinterpret_cast<HWND>(window->winId()));
        frame.openRequested = [window] { restoreWhatWindow(window); };
        restoreWhatWindow(window);
        QTRY_VERIFY(window->isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QSignalSpy visibilityChanged(window, &QWindow::visibilityChanged);
        restoreWhatWindow(window); // Tray double-click and a second launch.
        QVERIFY(notifyExistingWhatWindow(QCoreApplication::applicationPid()));
        QTest::qWait(40);
        QCOMPARE(visibilityChanged.count(), 0);
        QCOMPARE(window->opacity(), 1.0);
        QSignalSpy quit(window, SIGNAL(windowQuitReady()));
        QVERIFY(QMetaObject::invokeMethod(window, "performWindowAction", Q_ARG(QVariant, "quit")));
        QCOMPARE(quit.count(), 1);
        window->hide();
        QCOMPARE(warnings.count(), 0);
    }

    void qmlPageSlidesAndImmediatePrompts() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.setUiTiming("pages", 600);
        local.setUiTiming("dialogEnter", 400);
        local.setUiTiming("dialogExit", 300);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *stack = window->findChild<QQuickItem *>("pageStack");
        QVERIFY(stack);
        auto *container = qvariant_cast<QQuickItem *>(stack->property("pageContainer"));
        QVERIFY(container);
        const auto pages = container->childItems();
        QCOMPARE(pages.size(), 10);
        QCOMPARE(stack->property("shownIndex").toInt(), 0);
        // Pages and their controls are ready before the first navigation.
        auto *settingsSearch = window->findChild<QObject *>("settingsSearch");
        QVERIFY(settingsSearch);
        QVERIFY(window->findChild<QObject *>("moduleSearch"));

        window->setProperty("previewPage", 1);
        QTRY_VERIFY(stack->property("moving").toBool());
        QVERIFY(pages[1]->y() > 0);
        QVERIFY(pages[0]->isVisible());
        QVERIFY(pages[1]->isVisible());
        // Reverse an in-flight transition, then jump through several pages.
        window->setProperty("previewPage", 0);
        QTRY_VERIFY(!stack->property("moving").toBool());
        QCOMPARE(pages[0]->y(), 0.0);
        window->setProperty("previewPage", 5); // Jump over Commands and Home Assistant Web.
        QTRY_COMPARE(stack->property("shownIndex").toInt(), 5);
        window->setProperty("previewPage", 2); // Settings is last, despite its page ID.
        QTRY_VERIFY(stack->property("moving").toBool());
        // Long jumps must not show the pages between the endpoints.
        for (int i : {3, 4, 6, 7, 8, 9})
            QVERIFY(!pages[i]->isVisible());
        QTRY_COMPARE(stack->property("shownIndex").toInt(), 2);
        settingsSearch->setProperty("text", "animations");
        QVERIFY(!local.trayManager()->browsing);
        for (int i = 0; i < pages.size(); ++i)
            QCOMPARE(pages[i]->isVisible(), i == 2);

        window->setProperty("previewPage", 0);
        QTRY_VERIFY(stack->property("moving").toBool());
        local.setAnimationsEnabled(false);
        QTRY_VERIFY(!stack->property("moving").toBool());
        QCOMPARE(pages[0]->y(), 0.0);
        local.setAnimationsEnabled(true);
        QTRY_VERIFY(window->property("animationsEnabled").toBool());
        local.setUiTiming("pages", 0);
        QTRY_COMPARE(window->property("uiTimings").toMap()["pages"].toInt(), 0);
        window->setProperty("previewPage", 7);
        QTRY_COMPARE(pages[7]->y(), 0.0);
        QTRY_VERIFY(local.trayManager()->browsing);
        window->setProperty("previewPage", 0);
        QTRY_COMPARE(pages[0]->y(), 0.0);

        window->setProperty("previewPage", 2);
        QCOMPARE(settingsSearch->property("text").toString(), QString("animations"));
        local.setUiTiming("pages", 600);
        QTRY_COMPARE(window->property("uiTimings").toMap()["pages"].toInt(), 600);
        window->setProperty("previewPage", 1);
        QTRY_VERIFY(stack->property("moving").toBool());
        window->resize(window->width(), window->height() + 10);
        QTRY_VERIFY(!stack->property("moving").toBool());
        QCOMPARE(stack->property("shownIndex").toInt(), 1);
        window->setProperty("previewPage", 0);
        QTRY_VERIFY(stack->property("moving").toBool());
        window->hide();
        QTRY_VERIFY(!stack->property("moving").toBool());
        QCOMPARE(stack->property("shownIndex").toInt(), 0);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *dialog = window->findChild<QObject *>("uiTimingDialog");
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QCOMPARE(dialog->property("opened").toBool(), true);
        QCOMPARE(dialog->property("scale").toDouble(), 1.0);
        // Modal overlays are visually parented to the window, not QObject children of it.
        std::function<QQuickItem *(QQuickItem *, const QString &)> findItem =
            [&](QQuickItem *item, const QString &name) -> QQuickItem * {
            if (item->objectName() == name && item->isVisible())
                return item;
            for (auto *child : item->childItems())
                if (auto *found = findItem(child, name))
                    return found;
            return nullptr;
        };
        auto *backdrop = findItem(window->contentItem(), "glassBackdrop");
        QVERIFY(backdrop);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_COMPARE(backdrop->opacity(), 1.0);
        for (const auto &key : {"pages", "dialogEnter", "dialogExit"})
            QVERIFY(findItem(window->contentItem(), QString("timing-") + key));
        if (!qEnvironmentVariableIsEmpty("WHAT_TIMING_PREVIEW"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_TIMING_PREVIEW")));
        auto *scroll = findItem(window->contentItem(), "timingScroll");
        auto *reset = findItem(window->contentItem(), "resetAnimationTimings");
        QVERIFY(scroll);
        QVERIFY(reset);
        auto *flick = qvariant_cast<QQuickItem *>(scroll->property("contentItem"));
        QVERIFY(flick);
        const double bottom = flick->property("contentHeight").toDouble() - flick->height();
        QVERIFY(bottom > 0);
        flick->setProperty("contentY", bottom);
        QTRY_VERIFY(reset->mapToItem(flick, QPointF(0, reset->height())).y() <= flick->height() + 1);
        if (!qEnvironmentVariableIsEmpty("WHAT_TIMING_PREVIEW"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_TIMING_PREVIEW") + ".scrolled.png"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QCOMPARE(dialog->property("visible").toBool(), false);
        local.setAnimationsEnabled(false);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(dialog->property("opacity").toDouble(), 1.0);
        QCOMPARE(dialog->property("scale").toDouble(), 1.0);
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
    }

    void windowActionsAndNativeAnimationPreference() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QTRY_COMPARE(window->opacity(), 1.0);
        window->resize(1000, 824);
        for (int page = 0; page < 8; ++page) {
            window->setProperty("previewPage", page);
            QTest::qWait(40);
            QVERIFY(!window->findChild<QQuickItem *>("hideToTrayButton"));
            auto *button = window->findChild<QQuickItem *>("mainContentLayout");
            QVERIFY(button);
            const auto edge = button->mapToScene(QPointF(button->width(), 0));
            QVERIFY2(edge.x() <= window->width(), qPrintable(QString("Page %1 right edge %2 exceeds %3")
                                                                 .arg(page)
                                                                 .arg(edge.x())
                                                                 .arg(window->width())));
            QVERIFY(button->width() >= button->implicitWidth());
        }

        QVERIFY(QMetaObject::invokeMethod(window, "performWindowAction", Q_ARG(QVariant, "minimise")));
        QCOMPARE(window->opacity(), 1.0);
        QTRY_COMPARE(window->visibility(), QWindow::Minimized);
        window->showNormal();
        QTRY_COMPARE(window->opacity(), 1.0);
        QVERIFY(QMetaObject::invokeMethod(window, "toggleMaximised"));
        QTRY_COMPARE(window->visibility(), QWindow::Maximized);
        QTRY_COMPARE(window->opacity(), 1.0);
        QVERIFY(QMetaObject::invokeMethod(window, "toggleMaximised"));
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);
        QTRY_COMPARE(window->opacity(), 1.0);
        QVERIFY(QMetaObject::invokeMethod(window, "performWindowAction", Q_ARG(QVariant, "hide")));
        QVERIFY(!window->isVisible());
        local.setWindowAnimationsEnabled(false);
        QVERIFY(!window->isVisible());
        QCOMPARE(window->opacity(), 1.0);
        QCOMPARE(local.settings.value("ui/window_animations").toBool(), false);
        window->show();
        QCOMPARE(window->opacity(), 1.0);
        QVERIFY(QMetaObject::invokeMethod(window, "performWindowAction", Q_ARG(QVariant, "hide")));
        QVERIFY(!window->isVisible());
        local.setWindowAnimationsEnabled(true);
        window->show();
        QTRY_COMPARE(window->opacity(), 1.0);
        QSignalSpy quit(window, SIGNAL(windowQuitReady()));
        QVERIFY(QMetaObject::invokeMethod(window, "performWindowAction", Q_ARG(QVariant, "quit")));
        QTRY_COMPARE(quit.count(), 1);
        window->hide();
    }

    void trayQuitUsesGlassAndConfirmsActions() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->resize(1100, 824);
        // The tray path restores the hidden page before opening the overlay.
        restoreWhatWindow(window);
        QTRY_VERIFY(window->isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *dialog = window->findChild<QObject *>("trayQuitConfirmation");
        QVERIFY(dialog);
        QSignalSpy quit(window, SIGNAL(windowQuitReady()));
        QVERIFY(QMetaObject::invokeMethod(window, "openQuitConfirmation"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(window->property("quitConfirmationVisible").toBool());
        QVERIFY(dialog->property("modal").toBool());
        auto *backdrop = visualItem(window->contentItem(), "glassBackdrop");
        QVERIFY(backdrop && backdrop->isVisible());
        QTRY_COMPARE(backdrop->opacity(), 1.0);
        auto *source = qvariant_cast<QQuickItem *>(backdrop->property("sourceItem"));
        QVERIFY(source && source->window() == window);
        QCOMPARE(quit.count(), 0);
        // A second tray request reuses the same prompt and cannot confirm it.
        QVERIFY(QMetaObject::invokeMethod(window, "openQuitConfirmation"));
        QCOMPARE(window->findChildren<QObject *>("trayQuitConfirmation").size(), 1);
        QCOMPARE(quit.count(), 0);
        if (!qEnvironmentVariableIsEmpty("WHAT_REDESIGN_PREVIEW"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_REDESIGN_PREVIEW") + "/quit-glass.png"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(quit.count(), 0);
        QVERIFY(window->isVisible());

        auto click = [&](const QString &name) {
            auto *button = visualItem(window->contentItem(), name);
            if (!button || !button->isVisible())
                return false;
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
            return true;
        };
        QVERIFY(QMetaObject::invokeMethod(window, "openQuitConfirmation"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(click("keepRunning"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(quit.count(), 0);
        QVERIFY(window->isVisible());

        QVERIFY(QMetaObject::invokeMethod(window, "openQuitConfirmation"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(click("sendToTray"));
        QTRY_VERIFY(!window->isVisible());
        QVERIFY(!dialog->property("visible").toBool());
        QCOMPARE(quit.count(), 0);

        restoreWhatWindow(window);
        QTRY_VERIFY(window->isVisible());
        QVERIFY(QMetaObject::invokeMethod(window, "openQuitConfirmation"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(quit.count(), 1);
        QVERIFY(!dialog->property("visible").toBool());
        local.setAnimationsEnabled(false);
        QVERIFY(QMetaObject::invokeMethod(window, "openQuitConfirmation"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(click("confirmQuit"));
        QTRY_COMPARE(quit.count(), 2);
        QCOMPARE(warnings.count(), 0);
        window->hide();
    }

    void quitCancellationWhileStreaming() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!local.windowsJob.isRunning() && !local.inventoryJob.isRunning() &&
                                 !local.slowJob.isRunning(), 15000);
        local.settings.setValue("sources/hwinfo", false);
        QJsonArray rows;
        for (int i = 0; i < 3000; ++i) {
            const auto id = QString("win_cancel_%1").arg(i);
            auto reading = row(id, i * 0.01);
            reading["name"] = QString("Temperature %1 / ").arg(i) + QChar(0x00b0);
            rows.append(reading);
            local.selected.insert(id);
        }
        local.mergeReadings("windows", rows, 1);
        local.publish();
        QWebSocket client;
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        client.setSslConfiguration(ssl);
        int snapshots = 0;
        connect(&client, &QWebSocket::textMessageReceived, this, [&](const QString &text) {
            const auto frame = QJsonDocument::fromJson(text.toUtf8()).object();
            if (frame["type"] == "snapshot")
                ++snapshots;
        });
        client.open(QUrl(QString("wss://127.0.0.1:%1").arg(local.activePort)));
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        client.sendTextMessage(QJsonDocument(QJsonObject{
            {"type", "auth"}, {"protocol", 2}, {"token", local.token}
        }).toJson(QJsonDocument::Compact));
        QTRY_VERIFY(snapshots > 0);

        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                     {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        restoreWhatWindow(window);
        QTRY_VERIFY(window->isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *dialog = window->findChild<QObject *>("trayQuitConfirmation");
        auto *keepRunning = window->findChild<QObject *>("keepRunning");
        QVERIFY(dialog && keepRunning);
        QSignalSpy quit(window, SIGNAL(windowQuitReady()));
        for (int cycle = 0; cycle < 12; ++cycle) {
            const int previousFrames = snapshots;
            local.catalog["win_cancel_0"]["value"] = cycle + 1;
            local.publish();
            QVERIFY(QMetaObject::invokeMethod(window, "openQuitConfirmation"));
            QCOMPARE(dialog->property("opened").toBool(), true);
            QVERIFY(QMetaObject::invokeMethod(keepRunning, "clicked"));
            QTRY_VERIFY(!dialog->property("visible").toBool());
            QTRY_VERIFY(snapshots > previousFrames);
            QVERIFY(window->isVisible());
            QCOMPARE(quit.count(), 0);
            QCOMPARE(client.state(), QAbstractSocket::ConnectedState);
        }
        QCOMPARE(warnings.count(), 0);
        client.abort();
        local.stopService();
        window->hide();
    }

    void iconRenderingCoalescesAndRejectsRemovedResults() {
        QSemaphore entered, release;
        std::atomic_bool offGui{false};
        int completions = 0;
        IconRenderer renderer([&](const IconRenderer::Request &, int size) {
            offGui = QThread::currentThread() != qApp->thread();
            entered.release();
            release.tryAcquire(1, 5000);
            QImage pixels(size, size, QImage::Format_ARGB32);
            pixels.fill(Qt::red);
            return pixels;
        });
        auto guard = qScopeGuard([&] { release.release(10); });
        renderer.ready = [&] { ++completions; };
        IconRenderer::Request request{"removed", "1", {}, {16}, false};
        QVERIFY(!renderer.get(request));
        QTRY_VERIFY(entered.available());
        QVERIFY(offGui.load());
        renderer.retain({}); // Completion must not recreate a removed entry.
        release.release();
        QTRY_COMPARE(completions, 1);
        QVERIFY(!renderer.get(request));
        QTRY_COMPARE(entered.available(), 2);
        release.release();
        QTRY_COMPARE(completions, 2);
        QVERIFY(renderer.get(request).has_value());
        QVERIFY(renderer.get(request)->preview.isEmpty());
    }

    void queuedWritesDrainAfterOwnerIsDestroyed() {
        QSemaphore entered, release;
        std::atomic_int completed{0};
        auto worker = std::make_unique<BackgroundWorker>("Ordered shutdown fixture");
        auto guard = qScopeGuard([&] { release.release(); });
        worker->post([&] {
            entered.release();
            release.tryAcquire(1, 5000);
            completed = 1;
        });
        worker->post([&] {
            if (completed == 1)
                completed = 2;
        });
        QTRY_VERIFY(entered.available());
        QElapsedTimer elapsed;
        elapsed.start();
        worker.reset();
        QVERIFY(elapsed.elapsed() < 1000);
        QCOMPARE(completed.load(), 0);
        release.release();
        QTRY_COMPARE(completed.load(), 2);
    }

    void icueDelayedDisconnectStaysOffGui() {
        QLibrary fixture(QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(ICUE_TEST_DLL));
        QVERIFY(fixture.load());
        auto delay = reinterpret_cast<void (*)(int)>(fixture.resolve("IcueFixtureDisconnectDelay"));
        auto state = reinterpret_cast<int (*)()>(fixture.resolve("IcueFixtureDisconnectState"));
        auto thread = reinterpret_cast<DWORD (*)()>(fixture.resolve("IcueFixtureDisconnectThread"));
        QVERIFY(delay && state && thread);
        auto reset = qScopeGuard([&] { delay(0); });
        auto sdk = makeAsyncDisposed<IcueSdk>((QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(ICUE_TEST_DLL)));
        QVERIFY(sdk->poll(true)["connected"].toBool());
        delay(1000);
        QElapsedTimer elapsed;
        elapsed.start();
        auto inFlight = sdk;
        sdk.reset();
        QCOMPARE(state(), 0); // An outstanding job retains the SDK until it finishes.
        inFlight.reset();
        QVERIFY(elapsed.elapsed() < 250);
        QTRY_COMPARE(state(), 1);
        QVERIFY(thread() != GetCurrentThreadId());
        QTRY_COMPARE(state(), 2);
    }

    void sensorTrayRenderingAndPersistence() {
        QTemporaryDir dir;
        const auto path = dir.filePath("tray.yaml");
        QVariantMap r{
            {"id", "cpu"},      {"source", "hwinfo"}, {"group", "CPU"},    {"name", "Package temperature"},
            {"kind", "number"}, {"unit", "C"},        {"available", true}, {"value", 54.3}};
        QCOMPARE(SensorTrayManager::compact(r), QString("54"));
        QVERIFY(SensorTrayManager::tooltip(r).contains(QString::fromUtf8("54.3 °C")));
        QVERIFY(SensorTrayManager::tooltip(r).contains("HWiNFO / CPU"));
        {
            SensorTrayManager manager(path, true);
            manager.setReadings({r});
            manager.searchPicker("CPU temperature");
            QTRY_VERIFY(!manager.pickerBusy());
            QCOMPARE(manager.pickerModel()->rowCount(), 3);
            manager.add("cpu");
            manager.add("cpu");
            QCOMPARE(manager.entries().size(), 2);
            auto style = manager.entries()[1].toMap();
            auto image = SensorTrayManager::render("54", style, 32);
            QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
            style["background"] = "#80402010";
            auto tinted = SensorTrayManager::render("54", style, 32);
            QCOMPARE(tinted.pixelColor(0, 0).alpha(), 128);
            r["value"] = 55.2;
            manager.setReadings({r});
            QCOMPARE(manager.values()["cpu"].toMap()["text"].toString(), QString("55"));
            manager.setReadings({});
            QCOMPARE(manager.values()["cpu"].toMap()["text"].toString(), QString::fromUtf8("-"));
            QCOMPARE(manager.values()["cpu"].toMap()["module"].toString(), QString("HWiNFO"));
        }
        SensorTrayManager restored(path, true);
        QCOMPARE(restored.entries().size(), 2);
        QCOMPARE(restored.values()["cpu"].toMap()["module"].toString(), QString("HWiNFO"));
        QSettings saved(path, SettingsStore::format());
        const auto backup = ConfigurationFile::snapshot(saved);
        const auto imported = ConfigurationFile::decode(ConfigurationFile::encode(backup));
        QVERIFY2(imported.error.isEmpty(), qPrintable(imported.error));
        restored.remove("cpu");
        QCOMPARE(restored.entries().size(), 1);
        SensorTrayManager empty(path, true);
        QCOMPARE(empty.entries().size(), 1);
    }

    void primaryTrayIconPersistsAndPreservesBehavior() {
        QTemporaryDir dir;
        const auto file = dir.filePath("tray.yaml");
        QImage custom(32, 32, QImage::Format_ARGB32);
        custom.fill(Qt::blue);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        custom.save(&buffer, "PNG");
        {
            QSettings settings(file, SettingsStore::format());
            settings.setValue(
                "tray/main",
                QJsonDocument(QJsonObject{{"image", QString::fromLatin1(bytes.toBase64())}}).toJson());
        }
        {
            SensorTrayManager manager(file, false);
            QSystemTrayIcon primary;
            QMenu menu;
            menu.addAction("Open WHAT");
            primary.setContextMenu(&menu);
            manager.attachMainTray(&primary);
            QTRY_VERIFY(!primary.icon().isNull());
            QCOMPARE(primary.icon().pixmap(32, 32).toImage().pixelColor(16, 16), QColor(Qt::blue));
            QCOMPARE(manager.entries().size(), 1);
            manager.remove("main");
            QCOMPARE(manager.entries().size(), 1);
            manager.setMainToolTip("WHAT\nConnected");
            QCOMPARE(primary.toolTip(), QString("WHAT\nConnected"));
            const QVariantMap reading{
                {"id", "cpu"}, {"name", "CPU"}, {"kind", "number"}, {"value", 54.3}, {"available", true}};
            manager.setReadings({reading});
            const auto previous = primary.icon().cacheKey();
            manager.setSensor("main", "cpu");
            manager.resetStyle("main");
            QCOMPARE(manager.entries()[0].toMap()["sensor"].toString(), QString("cpu"));
            QVERIFY(manager.entries()[0].toMap()["image"].toString().isEmpty());
            QTRY_VERIFY(primary.icon().cacheKey() != previous);
            QVERIFY(primary.toolTip().contains("54.3"));
            manager.setMainToolTip("WHAT\nDisconnected");
            QVERIFY(primary.toolTip().contains("54.3"));
            QCOMPARE(primary.contextMenu(), &menu);
            for (auto *child : manager.children())
                QVERIFY(!dynamic_cast<NativeSensorIcon *>(child));
            manager.setReadings({});
            QVERIFY(primary.toolTip().contains("Unavailable"));
        }
        SensorTrayManager restored(file, true);
        QCOMPARE(restored.entries()[0].toMap()["sensor"].toString(), QString("cpu"));
        restored.restoreLogo();
        restored.setBrowsing(true);
        QTRY_VERIFY(!restored.values()["main"].toMap()["preview"].toString().isEmpty());
        const auto preview = QImage::fromData(QByteArray::fromBase64(
            restored.values()["main"].toMap()["preview"].toString().section(',', 1).toLatin1()));
        QCOMPARE(preview.size(),
                 QImage(":/assets/app-icon.png").size().scaled(192, 192, Qt::KeepAspectRatio));
        SensorTrayManager logo(file, true);
        QVERIFY(logo.entries()[0].toMap()["sensor"].toString().isEmpty());
        QVERIFY(logo.entries()[0].toMap()["image"].toString().isEmpty());
    }

    void primaryTrayCollectsWithPageClosed() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.sensorTrayTimer.stop();
        auto reading = row("tray-primary", 42);
        local.mergeReadings("windows", {reading}, 1);
        auto *manager = local.trayManager();
        manager->setReadings({reading.toVariantMap()});
        manager->setSensor("main", "tray-primary");
        QVERIFY(!manager->browsing);
        reading["value"] = 67;
        local.mergeReadings("windows", {reading}, 1);
        local.windowsClock.start();
        QVERIFY(QMetaObject::invokeMethod(&local.sensorTrayTimer, "timeout"));
        QTRY_COMPARE(manager->values()["main"].toMap()["text"].toString(), QString("67"));
        QVERIFY(!local.selected.contains("tray-primary"));
    }

    void sensorPickerCoalescesSearchesAndClosesSafely() {
        QTemporaryDir dir;
        SensorTrayManager manager(dir.filePath("tray.yaml"), true);
        auto *model = manager.pickerModel();
        QAbstractItemModelTester modelCheck(model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        QVariantList readings;
        for (int n = 0; n < 3000; ++n)
            readings.append(QVariantMap{{"id", QString::number(n)},
                                        {"source", "hwinfo"},
                                        {"group", "CPU"},
                                        {"name", QString("Temperature %1").arg(n, 4, 10, QChar('0'))},
                                        {"kind", "number"},
                                        {"value", n},
                                        {"available", true}});
        readings.append(QVariantMap{{"id", "offline"},
                                    {"source", "coretemp"},
                                    {"group", "CPU"},
                                    {"name", "Package temperature"},
                                    {"available", false}});
        auto row = [&](int index) { return model->data(model->index(index, 0), BrowserModel::Row).toMap(); };

        // Icon-only collection must not be mistaken for the full sensor catalogue.
        manager.setReadings({readings.first()}, false);
        manager.searchPicker("");
        QVERIFY(manager.pickerBusy());
        QCOMPARE(model->rowCount(), 0);
        manager.setReadings(readings);
        QCOMPARE(model->rowCount(), 0); // Preparation and delivery are asynchronous.
        manager.searchPicker("temperature 2998");
        manager.searchPicker("HWiNFO temperature 2999");
        QTRY_VERIFY(!manager.pickerBusy());
        QCOMPARE(resets.count(), 1); // Older queued searches must not reach the view.
        QCOMPARE(model->rowCount(), 3);
        QCOMPARE(row(2)["id"].toString(), QString("2999"));

        manager.searchPicker("");
        QTRY_VERIFY(!manager.pickerBusy());
        QCOMPARE(model->rowCount(), 3005);
        QCOMPARE(row(5)["id"].toString(), QString("0"));
        QCOMPARE(row(3004)["id"].toString(), QString("2999"));
        manager.searchPicker("", {{"[\"hwinfo\"]", true}});
        QTRY_VERIFY(!manager.pickerBusy());
        QCOMPARE(model->rowCount(), 4);
        manager.searchPicker("", {{"[\"hwinfo\",\"CPU\"]", true}});
        QTRY_VERIFY(!manager.pickerBusy());
        QCOMPARE(model->rowCount(), 5);

        // Search reveals matching readings even inside a collapsed group.
        manager.searchPicker("CORE TEMP package", {{"[\"coretemp\"]", true}});
        QTRY_VERIFY(!manager.pickerBusy());
        QCOMPARE(model->rowCount(), 3);
        QCOMPARE(row(2)["id"].toString(), QString("offline"));
        QCOMPARE(row(2)["reading"].toString(), QString("Unavailable"));

        resets.clear();
        manager.searchPicker("");
        manager.closePicker();
        QVERIFY(!manager.pickerBusy());
        QCOMPARE(model->rowCount(), 0);
        manager.searchPicker("temperature 0007");
        QTRY_VERIFY(!manager.pickerBusy());
        QCOMPARE(resets.count(), 2); // Clear on close, then only the reopened search.
        QCOMPARE(model->rowCount(), 3);
        QCOMPARE(row(2)["id"].toString(), QString("7"));
        manager.setReadings({QVariantMap{{"id", "ungrouped"}, {"source", ""}, {"name", "Voltage"}}});
        manager.searchPicker("Voltage");
        QTRY_VERIFY(!manager.pickerBusy());
        QCOMPARE(model->rowCount(), 3);
        QCOMPARE(row(0)["name"].toString(), QString("Other"));
        QCOMPARE(row(1)["name"].toString(), QString("General"));
        manager.searchPicker("");
        manager.closePicker(); // Destruction must also tolerate work still in flight.
    }

    void sensorTrayBatchSelection() {
        QTemporaryDir dir;
        SensorTrayManager manager(dir.filePath("settings.yaml"), true);
        QVariantList rows;
        QStringList ids;
        for (int i = 0; i < 150; ++i) {
            auto id = QString::number(i);
            ids << id;
            rows << QVariantMap{{"id", id}, {"name", "Temperature"}, {"source", "hwinfo"}, {"group", "CPU"}};
        }
        manager.setReadings(rows);
        manager.searchPicker("CPU");
        QTRY_VERIFY(!manager.pickerBusy());
        QCOMPARE(manager.pickerModel()->rowCount(), 152);
        QSignalSpy changed(&manager, &SensorTrayManager::configurationChanged);
        manager.addSelected({"missing", "0", "0", "1"});
        QCOMPARE(manager.entries().size(), 3);
        QCOMPARE(changed.size(), 1);
        manager.addSelected(ids);
        QCOMPARE(manager.entries().size(), 65);
        QCOMPARE(changed.size(), 2);
    }

    void sensorTrayNativeUpdates() {
        QTemporaryDir dir;
        SensorTrayManager manager(dir.filePath("settings.yaml"), false);
        QVariantMap row{{"id", "fixture"}, {"name", "Tray test"}, {"kind", "number"},
                        {"unit", "C"},     {"available", true},   {"value", 54.3}};
        manager.setReadings({row});
        manager.add("fixture");
        const auto sensorIcons = [&] {
            QList<NativeSensorIcon *> result;
            for (auto *child : manager.children())
                if (auto *icon = dynamic_cast<NativeSensorIcon *>(child))
                    result << icon;
            return result;
        };
        auto icons = sensorIcons();
        QCOMPARE(icons.size(), 1);
        QTRY_VERIFY(icons[0]->isVisible());
        QTRY_VERIFY(icons[0]->registeredWithShell());
        const auto identity = icons[0]->guid();
        QMenu menu;
        auto *action = menu.addAction("Open WHAT");
        QSignalSpy invoked(action, &QAction::triggered);
        QSystemTrayIcon primary(icons[0]->icon());
        styleWhatTrayMenu(&menu, QIcon(":/assets/app-icon.ico"));
        QObject::connect(&primary, &QSystemTrayIcon::activated, &menu, [&](auto reason) {
            if (reason == QSystemTrayIcon::Context)
                popupWhatTrayMenu(&menu);
        });
        primary.show();
        manager.setContextMenu(&menu);
        primary.activated(QSystemTrayIcon::Context);
        QTRY_VERIFY(menu.isVisible());
        menu.hide();
        QTest::qWait(100);
        QSignalSpy opened(&manager, &SensorTrayManager::openRequested);
        icons[0]->activated(QSystemTrayIcon::Context);
        QTRY_VERIFY(menu.isVisible());
        QCOMPARE(opened.size(), 0);
        menu.setActiveAction(action);
        QTest::keyClick(&menu, Qt::Key_Return);
        QCOMPARE(invoked.size(), 1);
        menu.hide();
        QVERIFY(icons[0]->toolTip().contains("54.3"));
        auto key = icons[0]->icon().cacheKey();
        row["value"] = 55.7;
        manager.setReadings({row});
        QVERIFY(icons[0]->toolTip().contains("55.7"));
        QTRY_VERIFY(key != icons[0]->icon().cacheKey());
        row["id"] = "second";
        manager.setReadings({row});
        manager.add("second");
        QTest::qWait(100);
        QTRY_COMPARE(sensorIcons().size(), 2);
        QTRY_VERIFY(sensorIcons()[1]->registeredWithShell());
        QVERIFY(sensorIcons()[1]->guid() != identity);
        for (auto *icon : sensorIcons()) {
            icon->activated(QSystemTrayIcon::Context);
            QTRY_VERIFY(menu.isVisible());
            menu.hide();
            QTest::qWait(100);
        }
        manager.remove("fixture");
        QVERIFY(!icons[0]->isVisible());
        QVERIFY(!icons[0]->registeredWithShell());
        // Simulate Explorer losing registration; the next update must re-add it.
        auto *remaining = sensorIcons()[1];
        NOTIFYICONDATAW lost{};
        lost.cbSize = sizeof(lost);
        lost.uFlags = NIF_GUID;
        lost.guidItem = remaining->guid();
        QVERIFY(Shell_NotifyIconW(NIM_DELETE, &lost));
        QVERIFY(!remaining->registeredWithShell());
        remaining->setToolTip("Recover this icon");
        QTRY_VERIFY(remaining->registeredWithShell());
    }

    void taskbarIconsPersistAndTrackReadings() {
        QTemporaryDir dir;
        const auto file = dir.path() + "/taskbar.yaml";
        QString extra;
        QVariantMap reading{
            {"id", "cpu"},   {"name", "CPU temperature"}, {"group", "CPU"}, {"kind", "number"}, {"unit", "C"},
            {"value", 54.3}, {"available", true}, {"source", "coretemp"}};
        {
            TaskbarIconManager manager(file, true);
            QQuickWindow main;
            manager.attachMainWindow(&main);
            QCOMPARE(manager.entries().size(), 1);
            manager.setReadings({reading});
            manager.setSensor("main", "cpu");
            QCOMPARE(manager.values()["main"].toMap()["text"].toString(), QString("54"));
            QTRY_VERIFY(!main.icon().isNull());
            auto unchangedIcon = main.icon().cacheKey();
            manager.setReadings({reading});
            QCOMPARE(main.icon().cacheKey(), unchangedIcon);
            manager.addSelected({"cpu"});
            QCOMPARE(manager.entries().size(), 2);
            extra = manager.entries()[1].toMap()["id"].toString();
            manager.rename(extra, "My CPU");
            reading["value"] = 72.1;
            manager.setReadings({reading});
            QCOMPARE(manager.values()[extra].toMap()["text"].toString(), QString("72"));
            QTRY_VERIFY(main.icon().cacheKey() != unchangedIcon);
            manager.setReadings({});
            QVERIFY(manager.values()[extra].toMap()["tooltip"].toString().contains("Unavailable"));
        }
        TaskbarIconManager restored(file, true);
        QCOMPARE(restored.entries().size(), 2);
        QCOMPARE(restored.entries()[0].toMap()["sensor"].toString(), QString("cpu"));
        QCOMPARE(restored.entries()[1].toMap()["name"].toString(), QString("My CPU"));
        QCOMPARE(restored.values()[extra].toMap()["module"].toString(), QString("Core Temp"));
        QCOMPARE(restored.values()["main"].toMap()["module"].toString(), QString("Core Temp"));
        restored.remove("main");
        QCOMPARE(restored.entries().size(), 2);
        restored.restoreLogo();
        restored.setBrowsing(true);
        QTRY_VERIFY(!restored.values()["main"].toMap()["preview"].toString().isEmpty());
        const auto preview = QImage::fromData(QByteArray::fromBase64(
            restored.values()["main"].toMap()["preview"].toString().section(',', 1).toLatin1()));
        const auto source =
            QImage(":/assets/app-icon.png").scaled(192, 192, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QVERIFY(source.height() == 192);
        QCOMPARE(preview.convertToFormat(QImage::Format_RGBA8888),
                 source.convertToFormat(QImage::Format_RGBA8888));
        QVERIFY(restored.entries()[0].toMap()["sensor"].toString().isEmpty());
        restored.remove(extra);
        QCOMPARE(restored.entries().size(), 1);
    }

    void appIconsCollectWithoutTrayOrOpenManager() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.sensorTrayTimer.stop();
        auto reading = row("app-only", 42);
        local.mergeReadings("windows", {reading}, 1);
        auto *manager = local.trayManager();
        manager->setReadings({reading.toVariantMap()});
        manager->taskbarIcons()->setSensor("main", "app-only");
        QCOMPARE(manager->entries().size(), 1);
        manager->browsing = false;
        reading["value"] = 67;
        local.mergeReadings("windows", {reading}, 1);
        local.windowsClock.start(); // The injected sample represents a successful collection.
        QVERIFY(QMetaObject::invokeMethod(&local.sensorTrayTimer, "timeout"));
        QTRY_COMPARE(manager->taskbarIcons()->values()["main"].toMap()["text"].toString(), QString("67"));
    }

    void taskbarIndicatorHasIndependentWindow() {
        QTemporaryDir dir;
        TaskbarIconManager manager(dir.path() + "/taskbar.yaml", false);
        manager.setReadings({QVariantMap{
            {"id", "cpu"}, {"name", "CPU"}, {"kind", "number"}, {"value", 42}, {"available", true}}});
        manager.addSelected({"cpu"});
        const auto id = manager.entries()[1].toMap()["id"].toString();
        QPointer<QWidget> panel;
        QTRY_VERIFY(([&] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (widget->objectName() == "taskbarPanel_" + id)
                    panel = widget;
            return !panel.isNull();
        })());
        const auto hwnd = reinterpret_cast<HWND>(panel->winId());
        const auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        QVERIFY(exStyle & WS_EX_APPWINDOW);
        QVERIFY(!(exStyle & WS_EX_TOOLWINDOW));
        QVERIFY(IsWindowVisible(hwnd));
        QVERIFY(GetWindow(hwnd, GW_OWNER) == nullptr);
        QVERIFY(panel->geometry().right() < 0);
        QVERIFY(panel->geometry().bottom() < 0);
        QVERIFY(panel->findChildren<QLabel *>().isEmpty());
        QVERIFY(manager.values()[id].toMap()["error"].toString().isEmpty());
        IPropertyStore *properties = nullptr;
        QVERIFY(SUCCEEDED(
            SHGetPropertyStoreForWindow(reinterpret_cast<HWND>(panel->winId()), IID_PPV_ARGS(&properties))));
        PROPVARIANT appId{};
        const auto result = properties->GetValue(PKEY_AppUserModel_ID, &appId);
        properties->Release();
        QVERIFY(SUCCEEDED(result));
        QCOMPARE(appId.vt, VARTYPE(VT_LPWSTR));
        const auto name = QString::fromWCharArray(appId.pwszVal);
        PropVariantClear(&appId);
        QVERIFY(name.startsWith("WHAT.Indicator."));
        QVERIFY(name.endsWith(id));
        QSignalSpy requested(&manager, &TaskbarIconManager::openRequested);
        manager.openPanel(id);
        QCOMPARE(requested.count(), 1);
        SendMessageW(reinterpret_cast<HWND>(panel->winId()), WM_SYSCOMMAND, SC_RESTORE, 0);
        QTRY_VERIFY(requested.count() >= 2);
        QVERIFY(panel->geometry().right() < 0);
        panel->close();
        QVERIFY(panel->isVisible());
        QVERIFY(panel->geometry().bottom() < 0);
        manager.remove(id);
        QVERIFY(panel.isNull());
    }

    void sensorTrayGuiAndLocalSelection() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.sensorTrayTimer.stop();
        QVariantMap reading{{"id", "tray-only"}, {"source", "hwinfo"}, {"name", "GPU Hot Spot Temperature"},
                            {"group", "dGPU [#1]: NVIDIA GeForce RTX 4090: GIGABYTE GeForce RTX 4090 WINDFORCE 24G (GV-N4090WF3-24GD)"},
                            {"kind", "number"},    {"value", 54.3375},
                            {"unit", "C"},       {"available", true}};
        auto second = reading;
        second["id"] = "second";
        second["name"] = "GPU load";
        local.trayManager()->setReadings({reading, second});
        auto selected = local.selected;
        local.trayManager()->add("tray-only");
        QCOMPARE(local.selected, selected);
        for (auto value : local.snapshot()["sensors"].toArray())
            QVERIFY(value.toObject()["id"] != "tray-only");
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        window->setProperty("previewPage", 7);
        QTest::qWait(600);
        QVERIFY(local.trayManager()->browsing);
        auto *title = visualItem(window->contentItem(), "iconTitle_tray:tray-only");
        QVERIFY(title);
        QVERIFY(title->property("text").toString().contains("GPU Hot Spot Temperature · HWiNFO · System Tray"));
        auto *detail = visualItem(window->contentItem(), "iconDetail_tray:tray-only");
        auto *remove = visualItem(window->contentItem(), "removeIcon_tray:tray-only");
        auto *header = visualItem(window->contentItem(), "iconHeader_tray:tray-only");
        QVERIFY(detail && remove && header);
        QVERIFY(!detail->property("text").toString().contains('\n'));
        QVERIFY(detail->property("text").toString().startsWith("54.3 °C"));
        const auto originalWidth = window->width();
        for (int width : {1024, 1600}) {
            window->setWidth(width);
            QTRY_VERIFY(detail->width() > 0);
            QTRY_VERIFY(detail->mapToItem(remove, QPointF(detail->width(), 0)).x() <= 0);
            QTRY_VERIFY(title->mapToItem(remove, QPointF(title->width(), 0)).x() <= 0);
            QTRY_VERIFY(detail->mapToItem(header, QPointF(0, detail->height())).y() <= header->height());
            if (const auto preview = qEnvironmentVariable("WHAT_ICON_LAYOUT_PREVIEW"); !preview.isEmpty()) {
                QSignalSpy frames(window, &QQuickWindow::frameSwapped);
                window->update();
                QTRY_VERIFY(!frames.isEmpty());
                QVERIFY(window->grabWindow().save(preview + QString("/icons-%1.png").arg(width)));
            }
        }
        window->setWidth(originalWidth);
        QVERIFY(window->findChild<QObject *>("addTraySensor"));
        auto *picker = window->findChild<QObject *>("traySensorPicker");
        QVERIFY(picker);
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        auto *choices = window->findChild<QObject *>("traySensorResults");
        QVERIFY(choices);
        QTRY_COMPARE(choices->property("count").toInt(), 4);
        selected = local.selected;
        picker->setProperty("selected", QVariantMap{{"second", true}});
        auto *confirm = window->findChild<QObject *>("confirmTraySensors");
        QVERIFY(confirm);
        QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
        QCOMPARE(local.trayManager()->entries().size(), 3);
        QCOMPARE(local.selected, selected);
        picker->setProperty("mode", "app");
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        picker->setProperty("selected", QVariantMap{{"second", true}});
        QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
        QCOMPARE(local.trayManager()->taskbarIcons()->entries().size(), 2);
        QCOMPARE(local.selected, selected);
        picker->setProperty("mode", "edit");
        picker->setProperty("targetId", "main");
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        picker->setProperty("selected", QVariantMap{{"tray-only", true}});
        QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
        QCOMPARE(local.trayManager()->taskbarIcons()->entries()[0].toMap()["sensor"].toString(),
                 QString("tray-only"));
        picker->setProperty("targetApp", false);
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        picker->setProperty("selected", QVariantMap{{"second", true}});
        QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
        QCOMPARE(local.trayManager()->entries()[0].toMap()["sensor"].toString(), QString("second"));
        QCOMPARE(local.selected, selected);
        auto *icons = window->findChild<QObject *>("managedIcons");
        QVERIFY(icons);
        const auto entries = icons->property("model").toList();
        QCOMPARE(entries.size(), 5);
        QCOMPARE(entries[0].toMap()["name"].toString(), QString("WHAT App Icon"));
        QCOMPARE(entries[1].toMap()["name"].toString(), QString("WHAT System Tray Icon"));
        auto *appHeader = visualItem(window->contentItem(), "iconHeader_app:main");
        auto *trayHeader = visualItem(window->contentItem(), "iconHeader_tray:main");
        QVERIFY(appHeader && trayHeader);
        auto *appEntry = appHeader->parentItem();
        auto *trayEntry = trayHeader->parentItem();
        const auto collapsedHeight = appEntry->height();
        QVERIFY(!appEntry->property("expanded").toBool());
        QVERIFY(!trayEntry->property("expanded").toBool());
        QVERIFY(QMetaObject::invokeMethod(appHeader, "clicked"));
        QTRY_VERIFY(appEntry->height() > collapsedHeight + 30);
        QVERIFY(QMetaObject::invokeMethod(trayHeader, "clicked"));
        QTRY_COMPARE(appEntry->height(), collapsedHeight);
        QVERIFY(trayEntry->property("expanded").toBool());
        QVERIFY(QMetaObject::invokeMethod(trayHeader, "clicked"));
        QTRY_COMPARE(trayEntry->height(), collapsedHeight);
        picker->setProperty("mode", "tray");
        if (qEnvironmentVariableIsSet("WHAT_TRAY_PREVIEW")) {
            QTest::qWait(300);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_TRAY_PREVIEW") + ".page.png"));
        }

        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QTest::qWait(400);
        if (qEnvironmentVariableIsSet("WHAT_TRAY_PREVIEW"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_TRAY_PREVIEW")));
        window->setProperty("previewPage", 0);
        QVERIFY(!local.trayManager()->browsing);
    }

    void sensorBrowserIconShortcuts() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        UpdateManager updater(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.sensorTrayTimer.stop();
        QTRY_VERIFY(!local.windowsJob.isRunning() && !local.inventoryJob.isRunning() &&
                    !local.slowJob.isRunning());
        auto reading = row("icon_context", 54.3);
        reading["name"] = "Icon context test";
        auto other = reading;
        other["id"] = "icon_context_other";
        // Equal names must not make the shortcut select the wrong reading.
        local.mergeReadings("windows", {reading, other}, 1);
        local.setBrowserFilters("Icon context test", "windows", false, false);
        QTRY_COMPARE(local.browserModel.rowCount(), 3);
        local.trayManager()->setReadings({reading, other});
        const auto shared = local.selected;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *pages = window->findChild<QObject *>("pageStack");
        auto *sensors = window->findChild<QObject *>("sensorsPage");
        auto *menu = window->findChild<QObject *>("sensorIconMenu");
        auto *picker = window->findChild<QObject *>("traySensorPicker");
        auto *search = window->findChild<QObject *>("traySensorSearch");
        auto *confirm = window->findChild<QQuickItem *>("confirmTraySensors");
        QVERIFY(pages && sensors && menu && picker && search && confirm);
        const QStringList actions{"setReadingAppIcon", "setReadingTrayIcon", "addReadingAppIcon",
                                  "addReadingTrayIcon"};
        for (int i = 0; i < actions.size(); ++i) {
            const auto trayBefore = local.trayManager()->entries();
            const auto appsBefore = local.trayManager()->taskbarIcons()->entries();
            window->setProperty("previewPage", 1);
            QTRY_COMPARE(pages->property("shownIndex").toInt(), 1);
            QQuickItem *entry = nullptr;
            QTRY_VERIFY((entry = visualItem(window->contentItem(), "expand_icon_context")));
            QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                              entry->mapToScene(QPointF(220, entry->height() / 2)).toPoint());
            QTRY_VERIFY(menu->property("opened").toBool());
            QCOMPARE(menu->property("count").toInt(), 4);
            QVERIFY(sensors->property("expandedId").toString().isEmpty());
            if (i == 0 && !qEnvironmentVariableIsEmpty("WHAT_ICON_MENU_PREVIEW"))
                QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_ICON_MENU_PREVIEW")));
            auto *action = menu->findChild<QQuickItem *>(actions[i]);
            QVERIFY(action);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              action->mapToScene(QPointF(action->width() / 2, action->height() / 2)).toPoint());
            QTRY_VERIFY(picker->property("opened").toBool());
            QTRY_COMPARE(pages->property("shownIndex").toInt(), 7);
            QCOMPARE(picker->property("mode").toString(), i < 2 ? "edit" : i == 2 ? "app" : "tray");
            QCOMPARE(picker->property("targetApp").toBool(), i % 2 == 0);
            QCOMPARE(picker->property("targetId").toString(), QString("main"));
            QCOMPARE(search->property("text").toString(), QString("Icon context test"));
            QCOMPARE(picker->property("selected").toMap(), QVariantMap({{"icon_context", true}}));
            QCOMPARE(local.trayManager()->entries(), trayBefore);
            QCOMPARE(local.trayManager()->taskbarIcons()->entries(), appsBefore);
            QTRY_VERIFY(confirm->isEnabled());
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              confirm->mapToScene(QPointF(confirm->width() / 2, confirm->height() / 2)).toPoint());
            QTRY_VERIFY(!picker->property("visible").toBool());
            const auto entries = i % 2 == 0 ? local.trayManager()->taskbarIcons()->entries()
                                            : local.trayManager()->entries();
            QCOMPARE(entries.size(), i < 2 ? 1 : 2);
            const auto icon = entries[i < 2 ? 0 : 1].toMap();
            QCOMPARE(icon.value("sensor", icon["id"]).toString(), QString("icon_context"));
            QCOMPARE(local.selected, shared);
        }
        // A normal picker visit must not retain the context-menu selection.
        QVERIFY(QMetaObject::invokeMethod(picker, "open"));
        QVERIFY(picker->property("selected").toMap().isEmpty());
        QVERIFY(search->property("text").toString().isEmpty());
        window->hide();
    }

    void wallpaperGui() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        UpdateManager updater(dir.path(), true);
        local.stopService();
        local.setLicense("pro");
        local.saveWallpaper(true, false, false, false, "");
        QTRY_VERIFY(!local.wallpaperJob.isRunning());
        local.wallpaperData = {}; // Before the first asynchronous status result.
        QStringList bindingWarnings;
        QQmlApplicationEngine engine;
        connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &warnings) {
            for (const auto &warning : warnings)
                if (warning.description().contains("Unable to assign [undefined] to bool"))
                    bindingWarnings.append(warning.toString());
        });
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 5);
        auto *card = window->findChild<QObject *>("wallpaperModuleCard");
        QVERIFY(card);
        card->setProperty("collapsed", false);
        QTRY_VERIFY(card->property("expanded").toBool());
        QVERIFY(card->property("moduleOn").toBool());
        local.setBrowserFilters("", "wallpaper", false, false, false);
        const auto wallpaperRows = local.browser();
        QVERIFY(std::any_of(wallpaperRows.begin(), wallpaperRows.end(), [](const QVariant &v) {
            return v.toMap().value("id").toString() == "wallpaper_controls";
        }));
        window->setProperty("previewPage", 1);
        auto *controls = window->findChild<QObject *>("wallpaperControlsDialog");
        QVERIFY(controls);
        QVERIFY(QMetaObject::invokeMethod(controls, "open"));
        QTRY_VERIFY(controls->property("opened").toBool());

        if (qEnvironmentVariableIsSet("WHAT_WALLPAPER_PREVIEW"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_WALLPAPER_PREVIEW")));
        QVERIFY(QMetaObject::invokeMethod(controls, "close"));
        QTRY_VERIFY(!controls->property("visible").toBool());
        // Opening Sensors can leave asynchronous row creation in flight.
        // Let it finish while the window still drives QML incubation.
        if (auto *incubation = engine.incubationController())
            QTRY_COMPARE_WITH_TIMEOUT(incubation->incubatingObjectCount(), 0, 5000);
        QVERIFY2(bindingWarnings.isEmpty(), qPrintable(bindingWarnings.join("\n")));
        window->hide();
    }

    void wallpaperValidationAndPrivacy() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        for (const auto &op : {"play", "pause", "stop", "mute", "unmute", "nextWallpaper", "closeWallpaper",
                               "hideIcons", "showIcons", "getWallpaper"})
            QVERIFY(WallpaperEngine::validate({{"operation", op}}).isEmpty());
        QVERIFY(!WallpaperEngine::validate({{"operation", "run_command"}}).isEmpty());
        QVERIFY(!WallpaperEngine::validate({{"operation", "play"}, {"file", "C:/anything"}}).isEmpty());
        QVERIFY(!WallpaperEngine::validate({{"operation", "nextWallpaper"}, {"monitor", -1}}).isEmpty());
        QVERIFY(!WallpaperEngine::validate({{"operation", "nextWallpaper"}, {"monitor", 1.5}}).isEmpty());
        QVERIFY(
            !WallpaperEngine::validate({{"operation", "nextWallpaper"}, {"monitor", 0}, {"location", "left"}})
                 .isEmpty());
        QVERIFY(!WallpaperEngine::validate({{"operation", "openProfile"}}).isEmpty());
        QVERIFY(WallpaperEngine::validate({{"operation", "openProfile"}, {"profile", "My profile & name"}})
                    .isEmpty());
        QCOMPARE(WallpaperEngine::arguments({{"operation", "openProfile"}, {"profile", "My profile & name"}}),
                 QStringList({"-control", "openProfile", "-profile", "My profile & name"}));
        QVERIFY(WallpaperEngine::validate(
                    {{"operation", "applyProperties"}, {"properties", QJsonObject{{"rate", 10}}}})
                    .isEmpty());
        QVERIFY(!WallpaperEngine::validate(
                     {{"operation", "applyProperties"}, {"properties", QJsonObject{{"bad", ")~END"}}}})
                     .isEmpty());
        QTemporaryDir temp;
        QFile file(temp.filePath("wallpaper64.exe"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("fixture");
        file.close();
        QCOMPARE(WallpaperEngine::discover(file.fileName()), file.fileName());
        QVERIFY(WallpaperEngine::discover(temp.filePath("missing.exe")).isEmpty());
        QVERIFY(!WallpaperEngine::execute(file.fileName(), {{"operation", "pause"}})["error"]
                     .toString()
                     .isEmpty());
        Controller local(temp.path(), true);
        local.stopService();
        QVERIFY(!local.moduleEnabled("wallpaper"));
        local.saveWallpaper(true, false, false, false, "");
        QTRY_VERIFY(!local.wallpaperJob.isRunning());
        local.wallpaperData["current_wallpaper"] = "private-path";
        QVERIFY(!local.wallpaperShared().contains("current_wallpaper"));
        local.handleWallpaperCommand("remote", {{"args", QJsonObject{{"operation", "play"}}}});
        QVERIFY(!local.wallpaperJob.isRunning());
        local.saveWallpaper(true, true, true, false, "");
        QTRY_VERIFY(!local.wallpaperJob.isRunning());
        local.wallpaperData["current_wallpaper"] = "private-path";
        PublishingPolicy policy;
        auto frame = QJsonObject{{"wallpaper", local.wallpaperShared()}, {"sensors", QJsonArray{}}};
        QVERIFY(policy.apply(frame, false, 0)["wallpaper"].toObject().contains("current_wallpaper"));
        QVERIFY(!policy.apply(frame, true, 0)["wallpaper"].toObject().contains("current_wallpaper"));
        local.setModuleEnabled("wallpaper", false);
        QVERIFY(!local.wallpaperShared()["enabled"].toBool());
    }

    void diagnosticReportPrivacyAndWorker() {
        const QString secret(64, 'a');
        QJsonObject input{
            {"private", QJsonArray{secret, "private-user"}},
            {"versions", QJsonObject{{"BEACN app", "1.4.2.12"},
                                     {"iCUE app", "PRIVATE-VERSION"},
                                     {"unexpected field", "192.168.4.7"}}},
            {"details",
             "C:/Users/private-user/private.txt\n192.168.4.7 https://private.example/secret\n" + secret},
            {"readings", QJsonArray{QJsonObject{{"source", "nvidia"},
                                                {"group", "GPU fixture"},
                                                {"name", "Temperature"},
                                                {"unit", "C"},
                                                {"available", true},
                                                {"shared", true},
                                                {"value", "VALUE-MUST-NOT-LEAK"}}}},
            {"logs",
             QJsonArray{"2026-09-13T12:45:30  command: PRIVATE-COMMAND", "2026-09-13T12:45:30  Connected"}}};
        const auto text = DiagnosticReport::build(input);
        for (const auto &blocked :
             {secret, QString("private-user"), QString("192.168.4.7"), QString("private.example"),
              QString("VALUE-MUST-NOT-LEAK"), QString("PRIVATE-COMMAND")})
            QVERIFY2(!text.contains(blocked), qPrintable(blocked));
        QVERIFY(text.contains("BEACN app: 1.4.2.12"));
        QVERIFY(!text.contains("PRIVATE-VERSION"));
        QVERIFY(!text.contains("unexpected field"));
        QVERIFY(text.contains("GPU fixture"));
        QVERIFY(text.contains("Temperature | available | shared"));
        QVERIFY(text.contains("12:45:30"));
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.prepareDiagnosticReport();
        QTRY_VERIFY_WITH_TIMEOUT(!local.diagnosticReportBusy(), 5000);
        QVERIFY(local.diagnosticReport().contains("DISCOVERED SENSOR TREE"));
        QVERIFY(!local.diagnosticReport().contains(local.token));
        QVERIFY2(writeClipboardWithRetry([&] { local.copyDiagnostics(); }),
                 "Windows clipboard remained unavailable for diagnostic copying");
        QCOMPARE(QApplication::clipboard()->text(), local.diagnosticReport());
    }

    void nvidiaTelemetryAndLifecycle() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        Nvidia::Api api;
        api.count = +[](unsigned *v) {
            *v = 2;
            return 0;
        };
        api.handle = +[](unsigned i, Nvidia::Device *v) {
            *v = reinterpret_cast<void *>(quintptr(i + 1));
            return 0;
        };
        api.uuid = +[](Nvidia::Device d, char *v, unsigned size) {
            const auto id = d == reinterpret_cast<void *>(1) ? "GPU-A" : "GPU-B";
            strncpy_s(v, size, id, _TRUNCATE);
            return 0;
        };
        api.name = +[](Nvidia::Device, char *v, unsigned size) {
            strncpy_s(v, size, "NVIDIA Fixture", _TRUNCATE);
            return 0;
        };
        api.temperature = +[](Nvidia::Device, unsigned, unsigned *v) {
            *v = 65;
            return 0;
        };
        api.power = +[](Nvidia::Device, unsigned *v) {
            *v = 125500;
            return 0;
        };
        api.memory = +[](Nvidia::Device, Nvidia::Memory *v) {
            *v = {12000000000ull, 8000000000ull, 4000000000ull};
            return 0;
        };
        api.fan = +[](Nvidia::Device, unsigned *) { return 3; };
        auto data = Nvidia::collect(api);
        QVERIFY(data["connected"].toBool());
        QCOMPARE(data["devices"].toArray().size(), 2);
        QSet<QString> ids;
        bool power = false, memory = false;
        for (auto item : data["readings"].toArray()) {
            auto row = item.toObject();
            const auto id = row["id"].toString();
            QVERIFY(!ids.contains(id));
            ids.insert(id);
            QVERIFY(!id.endsWith("_fan"));
            if (id.endsWith("_power")) {
                QCOMPARE(row["value"].toDouble(), 125.5);
                power = true;
            }
            if (id.endsWith("_memory_used")) {
                QCOMPARE(row["value"].toDouble(), 4000000000.0);
                memory = true;
            }
        }
        QVERIFY(power && memory);
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.stopService();
        QVERIFY(!local.moduleEnabled("nvidia"));
        local.setModuleEnabled("hwinfo", false);
        local.setModuleEnabled("windows", false);
        local.setModuleEnabled("nvidia", true);
        local.acceptNvidia(data);
        QVERIFY(local.nvidiaHealthy());
        auto nvidiaRows = [&] {
            int total = 0;
            for (auto row : local.browser())
                if (row.toMap()["source"] == "nvidia")
                    ++total;
            return total;
        };
        QVERIFY(nvidiaRows() > 0);
        QVERIFY(!local.snapshot()["sensors"].toArray().isEmpty());
        local.setPollingInterval("nvidia", 1000);
        QCOMPARE(local.settings.value("modules/nvidia/poll_ms").toInt(), 1000);
        local.setModuleEnabled("nvidia", false);
        QCOMPARE(nvidiaRows(), 0);
        QVERIFY(local.snapshot()["sensors"].toArray().isEmpty());
        local.setModuleEnabled("nvidia", true);
        local.acceptNvidia(data);
        QVERIFY(nvidiaRows() > 0);
        api.temperature = +[](Nvidia::Device, unsigned, unsigned *) { return 15; };
        data = Nvidia::collect(api);
        for (auto item : data["readings"].toArray()) {
            const auto row = item.toObject();
            if (row["id"].toString().endsWith("_temperature")) {
                QVERIFY(!row["available"].toBool());
                QVERIFY(row["value"].isNull());
            }
        }
        api.count = +[](unsigned *) { return 9; };
        QVERIFY(!Nvidia::collect(api)["connected"].toBool());
        QVERIFY(!Nvidia::collect({})["connected"].toBool());
    }

    void outboundRejectsUntrustedCertificate() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::renew(dir.path(), true);
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setLocalCertificate(identity.certificate);
        ssl.setPrivateKey(identity.key);
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        QWebSocketServer endpoint("untrusted fixture", QWebSocketServer::SecureMode);
        endpoint.setSslConfiguration(ssl);
        QVERIFY(endpoint.listen(QHostAddress::LocalHost, 0));
        QSignalSpy accepted(&endpoint, &QWebSocketServer::newConnection);
        NetworkService service;
        service.configure("remote-fixture", QString(64, 'a'), ssl);
        QVERIFY(service.listen(QHostAddress::LocalHost, 0));
        QSignalSpy diagnostics(&service, &NetworkService::diagnostic);
        service.setRemote(QString("wss://localhost:%1/api/what/connect").arg(endpoint.serverPort()));
        auto failed = [&] {
            for (const auto &args : diagnostics)
                if (args[0].toString().contains("Remote TLS handshake failed"))
                    return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(failed(), 6000);
        QCOMPARE(accepted.size(), 0);
        service.close();
    }

    void remoteUrlValidation() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        for (const QString &url : {"http://example.com", "https://user:pass@example.com",
                                   "https://example.com/path", "https://example.com?token=x", "https://example.com/#part"})
            QVERIFY(!setRemote(local, true, url));
        QVERIFY(setRemote(local, true, "https://example.com:8443"));
        QCOMPARE(local.installations().first().toMap()["url"].toString(), QString("https://example.com:8443"));
        QVERIFY(setRemote(local, false, ""));
        QCOMPARE(local.installations().first().toMap()["url"].toString(), QString());
    }

    void remoteDraftAndConnectionFooter() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *editor = window->findChild<QObject *>("installationEditor");
        QVERIFY(editor);
        QVERIFY(QMetaObject::invokeMethod(editor, "edit", Q_ARG(QVariant, local.installations().first())));
        auto *input = editor->findChild<QObject *>("installationUrl");
        auto *mode = window->findChild<QObject *>("connectionModeFooter");
        QVERIFY(input && mode);
        input->setProperty("text", "https://house.example");
        emit local.changed();
        QCoreApplication::processEvents();
        QCOMPARE(input->property("text").toString(), QString("https://house.example"));
        QVERIFY(QMetaObject::invokeMethod(editor, "save"));
        QTRY_VERIFY(mode->property("text").toString().startsWith("LOCAL + CLOUD ENABLED"));
        QVERIFY(QMetaObject::invokeMethod(editor, "edit", Q_ARG(QVariant, local.installations().first())));
        input->setProperty("text", "invalid");
        QVERIFY(QMetaObject::invokeMethod(editor, "save"));
        QVERIFY(editor->property("visible").toBool());
        emit local.installationsChanged();
        QCoreApplication::processEvents();
        QCOMPARE(input->property("text").toString(), QString("invalid"));
        QCOMPARE(local.installations().first().toMap()["url"].toString(), QString("https://house.example"));
    }

    void negotiatedLocalTlsVersions() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        for (auto protocol : {QSsl::TlsV1_2, QSsl::TlsV1_3}) {
            QWebSocket socket;
            auto ssl = QSslConfiguration::defaultConfiguration();
            ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
            ssl.setProtocol(protocol);
            socket.setSslConfiguration(ssl);
            socket.open(QUrl(QString("wss://127.0.0.1:%1").arg(local.activePort)));
            QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
            socket.sendTextMessage(
                QJsonDocument(QJsonObject{{"type", "auth"}, {"protocol", 2}, {"token", local.token}})
                    .toJson(QJsonDocument::Compact));
            QTRY_COMPARE(local.info()["connectionTransport"].toString(), QString("local"));
            QCOMPARE(socket.sslConfiguration().sessionProtocol(), protocol);
            QCOMPARE(local.info()["connectionTls"].toString(),
                     protocol == QSsl::TlsV1_2 ? QString("TLS 1.2 encrypted") : QString("TLS 1.3 encrypted"));
            socket.close();
            QTRY_COMPARE(local.info()["connectionTransport"].toString(), QString("none"));
            QVERIFY(local.info()["connectionTls"].toString().isEmpty());
        }
        local.stopService();
    }

    void publishingScopesRoundingAndRates() {
        PublishingPolicy local, cloud;
        auto frame = [](double value, int scope = 3, bool high = false) {
            return QJsonObject{
                {"sensors", QJsonArray{QJsonObject{{"id", "load"},
                                                   {"value", value},
                                                   {"kind", "number"},
                                                   {"unit", "%"},
                                                   {"available", true}}}},
                {"_publishing", QJsonObject{{"load", QJsonObject{{"scope", scope}, {"high", high}}}}},
                {"_normal_ms", 1000},
                {"_fast_ms", 100}};
        };
        const auto first = local.apply(frame(44.903315), false, 1000);
        QCOMPARE(first["sensors"].toArray()[0].toObject()["value"].toDouble(), 44.9);
        QVERIFY(!first.contains("_publishing"));
        QCOMPARE(
            local.apply(frame(45.903315), false, 1050)["sensors"].toArray()[0].toObject()["value"].toDouble(),
            44.9);
        QCOMPARE(local.nextWake(), 950);
        auto unrelated = frame(50);
        unrelated["_event_ids"] = QJsonArray{"win_locked"};
        QCOMPARE(
            local.apply(unrelated, false, 1100, true)["sensors"].toArray()[0].toObject()["value"].toDouble(),
            44.9);
        QVERIFY(!local.apply(unrelated, false, 1100, true).contains("_event_ids"));
        QCOMPARE(
            local.apply(frame(45.903315), false, 2000)["sensors"].toArray()[0].toObject()["value"].toDouble(),
            45.9);
        QVERIFY(cloud.apply(frame(1, 1), true, 2000)["sensors"].toArray().isEmpty());
        QVERIFY(local.apply(frame(1, 2), false, 2000)["sensors"].toArray().isEmpty());
        QCOMPARE(cloud.apply(frame(1, 2, true), true, 2000)["sensors"].toArray().size(), 1);
        QCOMPARE(
            cloud.apply(frame(2, 2, true), true, 2100)["sensors"].toArray()[0].toObject()["value"].toInt(),
            2);
        auto unavailable = frame(3, 2, true);
        auto rows = unavailable["sensors"].toArray();
        auto row = rows[0].toObject();
        row["available"] = false;
        row["value"] = QJsonValue::Null;
        rows[0] = row;
        unavailable["sensors"] = rows;
        QVERIFY(
            !cloud.apply(unavailable, true, 2110)["sensors"].toArray()[0].toObject()["available"].toBool());
        // Revocation removes even a previously cached cloud value immediately.
        QVERIFY(cloud.apply(frame(9, 1), true, 2120)["sensors"].toArray().isEmpty());
    }

    void mqttConfigurationAndPublishingPersistence() {
        QTemporaryDir dir;
        {
            Controller local(dir.path(), true);
            local.stopService();
            QVERIFY(local.installations().first().toMap()["type"] != "mqtt" || !local.installations().first().toMap()["enabled"].toBool());
            QVERIFY(local.installations().first().toMap()["type"] != "mqtt" || !local.installations().first().toMap()["enabled"].toBool());
            QVERIFY(!saveMqtt(local,
                {{"host", "example.com"}, {"port", 1883}, {"scope", "cloud"}, {"tls", false}}, ""));
            QVERIFY(saveMqtt(local, {{"host", "localhost"},
                                    {"port", 1883},
                                    {"scope", "local"},
                                    {"tls", false},
                                    {"enabled", false}},
                                   ""));
            QVERIFY(!local.installations().first().toMap().contains("password"));
            local.mergeReadings("windows", {row("publishing-fixture", 44.903315)}, 1);
            local.setSensorPublishing("publishing-fixture", 3, true, 2);
            QCOMPARE(local.settings.value("publishing/publishing-fixture").toMap()["scope"].toInt(), 3);
        }
        Controller restored(dir.path(), true);
        restored.stopService();
        QVERIFY(restored.selected.contains("publishing-fixture"));
        QCOMPARE(restored.settings.value("publishing/publishing-fixture").toMap()["decimals"].toInt(), 2);
    }

    void mqttPasswordProtection() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        const QVariantMap options{
            {"host", "localhost"}, {"port", 1883}, {"scope", "local"}, {"tls", false}, {"enabled", false}};
        try {
            protectSecret("probe");
        } catch (const std::exception &) {
            QVERIFY(!saveMqtt(local, options, "TEST-MQTT-PASSWORD"));
            QVERIFY(!local.settings.contains("mqtt/password"));
            QSKIP("DPAPI unavailable for this Windows test account; fail-closed behavior verified");
        }
        QVERIFY(saveMqtt(local, options, "TEST-MQTT-PASSWORD"));
        QVERIFY(local.installations().first().toMap()["hasPassword"].toBool());
        QVERIFY(!local.installations().first().toMap().contains("password"));
        QVERIFY(!local.settings.value("connections/credentials/" + local.installations().first().toMap()["id"].toString()).toMap()["password"].toByteArray().contains("TEST-MQTT-PASSWORD"));
        QCOMPARE(unprotectSecret(local.settings.value("connections/credentials/" + local.installations().first().toMap()["id"].toString()).toMap()["password"].toByteArray()),
                 QByteArray("TEST-MQTT-PASSWORD"));
        QVERIFY(saveMqtt(local, options, "", true));
        QVERIFY(!local.installations().first().toMap()["hasPassword"].toBool());
    }

    void categorySharingUsesUnfilteredReadingsAndCurrentPolicies() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.stopService();
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.setModuleEnabled("windows", false);
        QJsonArray rows;
        for (const auto &id : {"alpha", "beta"}) {
            auto item = row(id, 42);
            item["source"] = "hwinfo";
            item["group"] = "Fixture";
            item["name"] = id;
            item["available"] = QString(id) != "beta";
            rows.append(item);
        }
        local.mergeReadings("hwinfo", rows);
        local.setBrowserFilters("alpha", "hwinfo", false, false);
        local.setBrowserVisible(true);
        const QString category = "hwinfo / Fixture";
        QTRY_COMPARE(local.browserCategoryReadings.value(category).size(), 2);
        QTRY_COMPARE(local.browserModel.rowCount(), 2);
        local.setCategoryPublishing(category, 3, true);
        local.setCategoryPublishing(category, 2, false);
        local.setCategoryPublishing(category, 4, true);
        for (const auto &id : {"alpha", "beta"}) {
            QVERIFY(local.selected.contains(id));
            QCOMPARE(local.publishingPolicies[id].toObject()["scope"].toInt(), 1);
            QVERIFY(local.publishingPolicies[id].toObject()["high"].toBool());
            QCOMPARE(local.settings.value("publishing/" + QString(id)).toMap(),
                     local.publishingPolicies[id].toObject().toVariantMap());
        }
        // A newly discovered row must invalidate the category index, even while
        // scrolling prevents the browser worker from preparing another frame.
        local.setBrowserInteracting(true);
        auto added = rows[0].toObject();
        added["id"] = "gamma";
        rows.append(added);
        local.mergeReadings("hwinfo", rows);
        local.setCategoryEnabled(category, true);
        QVERIFY(local.selected.contains("gamma"));
        local.setCategoryEnabled(category, false);
        for (const auto &id : {"alpha", "beta", "gamma"})
            QVERIFY(!local.selected.contains(id));
    }

    void publishingBulkAndLargeSnapshot() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        QJsonArray rows;
        for (int i = 0; i < 3000; ++i) {
            auto value = row("bulk_" + QString::number(i), 44.903315);
            value["unit"] = "%";
            rows.append(value);
        }
        local.mergeReadings("windows", rows, 1);
        local.windowsClock.restart();
        local.setCategoryPublishing("windows / Storage / Fixture", 3, true);
        local.setCategoryPublishing("windows / Storage / Fixture", 4, true);
        QElapsedTimer preparation;
        preparation.start();
        auto snapshot = local.snapshot();
        qInfo("3000-reading snapshot preparation: %lld ms", preparation.elapsed());
        PublishingPolicy policy;
        QElapsedTimer timer;
        timer.start();
        auto result = policy.apply(snapshot, true, 0);
        const auto elapsed = timer.elapsed();
        int count = 0;
        for (auto v : result["sensors"].toArray())
            if (v.toObject()["id"].toString().startsWith("bulk_")) {
                ++count;
                QCOMPARE(v.toObject()["value"].toDouble(), 44.9);
            }
        QCOMPARE(count, 3000);
        QVERIFY(!result.contains("_publishing"));
        qInfo("3000-reading filtering and rounding: %lld ms; full frame: %lld bytes", elapsed,
              QJsonDocument(result).toJson(QJsonDocument::Compact).size());
        local.setCategoryPublishing("windows / Storage / Fixture", 2, false);
        result = policy.apply(local.snapshot(), true, 1000);
        for (auto v : result["sensors"].toArray())
            QVERIFY(!v.toObject()["id"].toString().startsWith("bulk_"));
        QCOMPARE(local.settings.value("publishing/bulk_0").toMap()["scope"].toInt(), 1);
        QVERIFY(local.settings.value("publishing/bulk_0").toMap()["high"].toBool());
    }

    void moduleArchitectureAvailability() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        for (const auto *key : {"beacn", "icue", "nvidia", "coretemp", "wallpaper"}) {
            // Profiles moved from x64 must retain selections, but cannot start
            // an unsupported native SDK through settings or the public setter.
            local.settings.setValue(QString("modules/%1/enabled").arg(key), true);
            QCOMPARE(local.moduleEnabled(key), !Architecture::arm64);
            local.setModuleEnabled(key, true);
            QCOMPARE(local.moduleEnabled(key), !Architecture::arm64);
            QVERIFY(local.settings.value(QString("modules/%1/enabled").arg(key)).toBool());
            QCOMPARE(Architecture::unavailableModules().contains(key), Architecture::arm64);
        }
        QVERIFY(local.moduleEnabled("windows"));
        QVERIFY(local.moduleEnabled("hwinfo"));
        if (Architecture::arm64) {
            UpdateManager updater(dir.path(), true);
            QQmlApplicationEngine engine;
            engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                         {"updater", QVariant::fromValue(&updater)}});
            engine.load(QUrl("qrc:/qml/Main.qml"));
            QVERIFY(!engine.rootObjects().isEmpty());
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            window->setProperty("previewPage", 3);
            for (const auto *key : {"beacn", "icue", "nvidia", "coretemp", "wallpaper"}) {
                auto *toggle = visualItem(window->contentItem(), QString(key) + "ModuleToggle");
                auto *warning = visualItem(window->contentItem(), QString(key) + "ModuleWarning");
                QVERIFY(toggle && warning);
                QVERIFY(!toggle->isEnabled());
                QVERIFY(!toggle->property("checked").toBool());
                QVERIFY(warning->property("detail").toString().contains("ARM64"));
            }
        }
    }

    void navigationSidebarResizing() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.setAnimationsEnabled(false);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                     {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->resize(1280, 884);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *sidebar = visualItem(window->contentItem(), "navigationSidebar");
        auto *grip = visualItem(window->contentItem(), "sidebarResizeGrip");
        auto *edge = visualItem(window->contentItem(), "sidebarEdge");
        QVERIFY(sidebar && grip && edge);
        QVERIFY(!visualItem(window->contentItem(), "sidebarToggle"));
        QCOMPARE(sidebar->width(), 204.0);
        QTest::mouseMove(window, QPoint(500, 400));
        QTRY_COMPARE(edge->width(), 4.0);
        QTest::mouseMove(window, grip->mapToScene(QPointF(3, 200)).toPoint());
        QTRY_COMPARE(edge->width(), 8.0);
        auto point = grip->mapToScene(QPointF(3, 200)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseMove(window, point + QPoint(90, 0));
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point + QPoint(90, 0));
        QTRY_VERIFY(sidebar->width() >= 290 && sidebar->width() <= 300);
        const int width = local.sidebarWidth();
        auto *pcStatus = visualItem(sidebar, "sidebarPcStatus");
        QVERIFY(pcStatus && pcStatus->isVisible());
        auto *footer = visualItem(window->contentItem(), "connectionFooter");
        QVERIFY(footer);
        QCOMPARE(footer->height(), 16.0);
        QVERIFY(!visualItem(window->contentItem(), "connectionFooterToggle"));
        for (const auto *name : {"connectionModeFooter", "connectionTlsFooter", "developerCreditFooter"}) {
            auto *label = visualItem(footer, name);
            QVERIFY(label && label->isVisible());
            QVERIFY(!label->property("text").toString().isEmpty());
            const auto top = label->mapToItem(footer, QPointF(0, 0)).y();
            QVERIFY(top >= 0 && top + label->height() <= footer->height());
        }
        if (qEnvironmentVariableIsSet("WHAT_WEBVIEW_SCREENSHOTS")) {
            QTest::qWait(50);
            window->grabWindow().save(qEnvironmentVariable("WHAT_WEBVIEW_SCREENSHOTS") + "/navigation-wide.png");
        }
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          grip->mapToScene(QPointF(3, 200)).toPoint());
        QTRY_COMPARE(sidebar->width(), 64.0);
        QVERIFY(local.sidebarCollapsed());
        QVERIFY(!pcStatus->isVisible());
        if (qEnvironmentVariableIsSet("WHAT_WEBVIEW_SCREENSHOTS")) {
            QTest::qWait(50);
            window->grabWindow().save(qEnvironmentVariable("WHAT_WEBVIEW_SCREENSHOTS") + "/navigation-collapsed.png");
        }
        QCOMPARE(local.sidebarWidth(), width);
        auto *settings = visualItem(window->contentItem(), "settingsNavigation");
        QVERIFY(settings);
        QVERIFY(settings->property("iconOnly").toBool());
        QQmlExpression tooltip(qmlContext(settings), settings, "ToolTip.text");
        QVERIFY(tooltip.evaluate().toString().startsWith("Settings: "));
        QVERIFY2(!tooltip.hasError(), qPrintable(tooltip.error().toString()));
        QVERIFY(QMetaObject::invokeMethod(settings, "clicked"));
        QCOMPARE(window->property("previewPage").toInt(), 2);
        grip->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_COMPARE(sidebar->width(), double(width));
        QVERIFY(pcStatus->isVisible());
        local.settings.sync();
        QSettings disk(local.settings.fileName(), SettingsStore::format());
        QCOMPARE(disk.value("ui/sidebar_width").toInt(), width);
        QVERIFY(!disk.value("ui/sidebar_collapsed").toBool());
        const auto exported = ConfigurationFile::decode(ConfigurationFile::encode(ConfigurationFile::snapshot(disk)));
        QVERIFY2(exported.error.isEmpty(), qPrintable(exported.error));
        QCOMPARE(exported.values["ui/sidebar_width"].toInt(), width);
        QCOMPARE(warnings.size(), 0);
    }

    void sidebarAnimationKeepsHiddenPagesSized() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.setUiTiming("panels", 500);
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                     {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->resize(1280, 884);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *sidebar = visualItem(window->contentItem(), "navigationSidebar");
        auto *stack = visualItem(window->contentItem(), "pageStack");
        QVERIFY(sidebar && stack);
        auto *container = stack->property("pageContainer").value<QQuickItem *>();
        QVERIFY(container);
        const auto pages = container->childItems();
        QCOMPARE(pages.size(), 10);
        const auto initialWidth = pages[1]->width();
        QSignalSpy visibleWidth(pages[0], &QQuickItem::widthChanged);
        QSignalSpy hiddenWidth(pages[1], &QQuickItem::widthChanged);
        QVERIFY(QMetaObject::invokeMethod(sidebar, "toggleCollapsed"));
        QTRY_COMPARE(sidebar->width(), 64.0);
        QTRY_COMPARE(pages[0]->width(), stack->width());
        qInfo("Sidebar collapse: %lld visible-page resizes, %lld hidden Sensors-page resizes",
              static_cast<long long>(visibleWidth.size()), static_cast<long long>(hiddenWidth.size()));
        QVERIFY(visibleWidth.size() > 2);
        QCOMPARE(hiddenWidth.size(), 0);
        for (int i = 1; i < pages.size(); ++i)
            QCOMPARE(pages[i]->width(), initialWidth);

        // Preloaded pages catch up before being shown, without being recreated.
        window->setProperty("previewPage", 1);
        QTRY_COMPARE(stack->property("shownIndex").toInt(), 1);
        QCOMPARE(pages[1]->width(), stack->width());
        QCOMPARE(pages[1]->height(), stack->height());
        visibleWidth.clear();
        hiddenWidth.clear();
        QVERIFY(QMetaObject::invokeMethod(sidebar, "toggleCollapsed"));
        QTRY_COMPARE(sidebar->width(), 204.0);
        QTRY_COMPARE(pages[1]->width(), stack->width());
        QCOMPARE(visibleWidth.size(), 0);
        QVERIFY(hiddenWidth.size() > 2);
        window->setProperty("previewPage", 2);
        QTRY_COMPARE(stack->property("shownIndex").toInt(), 2);
        QCOMPARE(pages[2]->width(), stack->width());
        // Sensors can still be creating rows after the page transition ends.
        // Keep the window alive until its incubation work has finished.
        if (auto *incubation = engine.incubationController())
            QTRY_COMPARE_WITH_TIMEOUT(incubation->incubatingObjectCount(), 0, 5000);
        QCOMPARE(warnings.size(), 0);
        window->hide();
    }

    void navigationSidebarAnimation() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"(
            import QtQuick
            import "qrc:/qml/controls"
            import "qrc:/qml/components"
            Window {
                id: navigationWindow
                width: 700; height: 850
                property bool animations: true
                onAnimationsChanged: Theme.animationsEnabled = animations
                Component.onCompleted: {
                    Theme.animationsEnabled = true;
                    Theme.timings = Object.assign({}, Theme.timings, {panels: 800});
                }
                property string updateVersion: "test"
                NavigationBar {
                    expandedWidth: 280; pcName: "TEST-PC"; version: "test"
                    availableUpdate: navigationWindow.updateVersion
                }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *sidebar = visualItem(window->contentItem(), "navigationSidebar");
        auto *grip = visualItem(window->contentItem(), "sidebarResizeGrip");
        QVERIFY(sidebar && grip);
        QCOMPARE(sidebar->width(), 280.0); // Saved geometry does not animate on startup.
        const auto clickEdge = [&] {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              grip->mapToScene(QPointF(3, 200)).toPoint());
        };
        clickEdge();
        QTRY_VERIFY(sidebar->width() < 260 && sidebar->width() > 64);
        // Reversing a moving sidebar must continue from its current width.
        clickEdge();
        QTRY_COMPARE(sidebar->width(), 280.0);
        QVERIFY(!sidebar->property("collapsed").toBool());
        auto *logo = visualItem(sidebar, "animatedSidebarLogo");
        auto *settings = visualItem(sidebar, "settingsNavigation");
        auto *icon = visualItem(settings, "navigationIcon");
        auto *label = visualItem(settings, "navigationLabel");
        auto *brand = visualItem(sidebar, "sidebarBrandTitle");
        auto *version = visualItem(sidebar, "sidebarVersion");
        QVERIFY(logo && icon && label && brand && version);
        const QList<QQuickItem *> textGroups{label, brand->parentItem(), version->parentItem()};
        const auto labelSize = label->size();
        const auto brandSize = brand->size();
        QTRY_COMPARE(logo->height(), 120.0);
        const qreal expandedLogoHeight = logo->height();
        const auto iconCentre = [&] {
            return icon->mapToItem(sidebar, QPointF(icon->width() / 2, 0)).x();
        };
        const qreal expandedIconCentre = iconCentre();
        const QString preview = qEnvironmentVariable("WHAT_SIDEBAR_PREVIEW");
        if (!preview.isEmpty())
            window->grabWindow().save(preview + "/expanded.png");
        for (const bool opening : {false, true}) {
            const qreal target = opening ? 280 : 64;
            qreal previousLogoHeight = logo->height();
            bool captured = false;
            int samples = 0;
            int effectFrames = 0;
            QElapsedTimer elapsed;
            elapsed.start();
            if (opening)
                QTest::keyClick(window, Qt::Key_Return);
            else
                clickEdge();
            while (sidebar->width() != target && elapsed.elapsed() < 2000) {
                QTest::qWait(16);
                ++samples;
                // Text keeps its full geometry. Icons and the logo travel
                // directly between their endpoints, even halfway through.
                QCOMPARE(label->size(), labelSize);
                QCOMPARE(brand->size(), brandSize);
                const qreal reveal = settings->property("textReveal").toReal();
                if (reveal > 0 && reveal < 1) {
                    ++effectFrames;
                    for (auto *textGroup : textGroups)
                        QVERIFY(QQmlProperty::read(textGroup, "layer.enabled").toBool());
                }
                // Only text is captured; icons and the sidebar remain sharp.
                QVERIFY(!QQmlProperty::read(icon, "layer.enabled").toBool());
                QVERIFY(!QQmlProperty::read(sidebar, "layer.enabled").toBool());
                QVERIFY(logo->height() >= 44 && logo->height() <= expandedLogoHeight);
                QVERIFY(opening ? logo->height() >= previousLogoHeight
                                : logo->height() <= previousLogoHeight);
                QVERIFY(iconCentre() >= qMin(30.0, expandedIconCentre) - 0.1);
                QVERIFY(iconCentre() <= qMax(30.0, expandedIconCentre) + 0.1);
                previousLogoHeight = logo->height();
                if (!preview.isEmpty() && !captured && sidebar->width() > 170 && sidebar->width() < 225) {
                    window->grabWindow().save(preview + (opening ? "/opening.png" : "/closing.png"));
                    captured = true;
                }
            }
            QCOMPARE(sidebar->width(), target);
            // Layout polish follows width bindings; wait for its final geometry
            // before reversing the next transition.
            QTRY_COMPARE(logo->height(), opening ? expandedLogoHeight : 44.0);
            QVERIFY(samples > 2);
            QVERIFY(effectFrames > 2);
            for (auto *textGroup : textGroups)
                QVERIFY(!QQmlProperty::read(textGroup, "layer.enabled").toBool());
            if (!opening) {
                // Measure against the visible divider, not its wider invisible
                // hit area or the outer edge of the whole sidebar item.
                auto *edge = visualItem(sidebar, "sidebarEdge");
                QVERIFY(edge);
                QTest::mouseMove(window, QPoint(600, 400));
                QTRY_COMPARE(edge->width(), 4.0);
                const qreal surfaceWidth = edge->mapToItem(sidebar, QPointF()).x();
                const qreal centre = surfaceWidth / 2;
                QList<QQuickItem *> icons;
                QList<QQuickItem *> pending{sidebar};
                while (!pending.isEmpty()) {
                    auto *item = pending.takeLast();
                    if (item->objectName() == "navigationIcon")
                        icons.append(item);
                    pending.append(item->childItems());
                }
                QCOMPARE(icons.size(), 10);
                for (auto *navIcon : icons) {
                    QCOMPARE(navIcon->mapToItem(sidebar, QPointF(navIcon->width() / 2, 0)).x(), centre);
                    const QFontMetricsF metrics(navIcon->property("font").value<QFont>());
                    const auto ink = metrics.tightBoundingRect(navIcon->property("text").toString());
                    const qreal inkCentre = navIcon->property("leftPadding").toReal() + ink.center().x();
                    QCOMPARE(navIcon->mapToItem(sidebar, QPointF(inkCentre, 0)).x(), centre);
                    auto *button = navIcon->parentItem()->parentItem();
                    QCOMPARE(button->mapToItem(sidebar, QPointF(button->width() / 2, 0)).x(), centre);
                    const auto buttonRect = button->mapRectToItem(sidebar, button->boundingRect());
                    QCOMPARE(buttonRect.left(), surfaceWidth - buttonRect.right());
                    const qreal inkY = navIcon->property("baselineOffset").toReal() + ink.center().y();
                    const qreal centreY = navIcon->mapToItem(button, QPointF(0, inkY)).y();
                    QVERIFY2(qAbs(centreY - button->height() / 2) < 0.6,
                             qPrintable(QString("%1 ink is not vertically centred: %2 vs %3")
                                 .arg(navIcon->property("text").toString()).arg(centreY).arg(button->height() / 2)));
                }
                QCOMPARE(logo->mapToItem(sidebar, QPointF(logo->width() / 2, 0)).x(), centre);
                auto *badge = visualItem(sidebar, "updateBadge");
                QVERIFY(badge && badge->isVisible());
                QCOMPARE(badge->mapToItem(sidebar, QPointF(badge->width() / 2, 0)).x(), centre);
                const auto logoRect = logo->mapRectToItem(sidebar, logo->boundingRect());
                const auto badgeRect = badge->mapRectToItem(sidebar, badge->boundingRect());
                QVERIFY(badgeRect.top() >= logoRect.bottom() + 8);
                window->setProperty("updateVersion", QString());
                QVERIFY(!badge->isVisible());
                QTRY_COMPARE(logo->height(), 44.0);
                QCOMPARE(logo->mapToItem(sidebar, QPointF(logo->width() / 2, 0)).x(), centre);
                window->setProperty("updateVersion", "test");
                if (!preview.isEmpty())
                    window->grabWindow().save(preview + "/collapsed.png");
            }
        }

        // Crossing the compact threshold while dragging must follow the mouse,
        // then animate the remaining distance when the button is released.
        auto point = grip->mapToScene(QPointF(3, 200)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseMove(window, point - QPoint(160, 0));
        QTRY_COMPARE(sidebar->width(), 120.0);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point - QPoint(160, 0));
        QTRY_VERIFY(sidebar->width() < 120 && sidebar->width() > 64);
        QTRY_COMPARE(sidebar->width(), 64.0);
        point = grip->mapToScene(QPointF(3, 200)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseMove(window, point + QPoint(96, 0));
        QTRY_COMPARE(sidebar->width(), 160.0);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point + QPoint(96, 0));
        QTRY_VERIFY(sidebar->width() > 160 && sidebar->width() < 180);
        QTRY_COMPARE(sidebar->width(), 180.0);
        window->setProperty("animations", false);
        clickEdge();
        QCOMPARE(sidebar->width(), 64.0);
        QCOMPARE(warnings.size(), 0);
    }

    void sidebarLogoPlaysAfterResize() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"(
            import QtQuick
            import "qrc:/qml/controls"
            import "qrc:/qml/components"
            Window {
                width: 700; height: 850
                property bool animations: true
                onAnimationsChanged: Theme.animationsEnabled = animations
                Component.onCompleted: {
                    Theme.animationsEnabled = true;
                    Theme.timings = Object.assign({}, Theme.timings,
                        {panels: 350, logo: 180, logoPause: 20});
                }
                NavigationBar { expandedWidth: 280; collapsed: true }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::mouseMove(window, QPoint(600, 400));
        auto *sidebar = visualItem(window->contentItem(), "navigationSidebar");
        auto *logo = visualItem(sidebar, "animatedSidebarLogo");
        QVERIFY(sidebar && logo);
        QSignalSpy playing(logo, SIGNAL(animatingChanged()));
        QVERIFY(playing.isValid());
        QCOMPARE(sidebar->width(), 64.0);
        QTest::qWait(100);
        QVERIFY(!logo->property("animating").toBool());
        QCOMPARE(playing.size(), 0); // Restoring a collapsed sidebar is not a gesture.

        const auto waitForOneCycle = [&](qreal width) {
            QTRY_COMPARE(sidebar->width(), width);
            QTRY_VERIFY(logo->property("animating").toBool());
            QTRY_VERIFY(!logo->property("animating").toBool());
            QCOMPARE(logo->property("progress").toReal(), 0.0);
            QCOMPARE(playing.size(), 2); // One start and one stop.
            QTest::qWait(250);
            QCOMPARE(playing.size(), 2);
        };
        sidebar->setProperty("collapsed", false);
        QTRY_VERIFY(sidebar->width() > 64 && sidebar->width() < 280);
        QVERIFY(!logo->property("animating").toBool());
        waitForOneCycle(280);

        // An existing hover cycle stops while resizing. The finishing gesture
        // still plays only once when the pointer stays over the logo.
        logo->setProperty("hovered", true);
        QTRY_VERIFY(logo->property("animating").toBool());
        sidebar->setProperty("collapsed", true);
        QTRY_VERIFY(sidebar->width() > 64 && sidebar->width() < 280);
        QVERIFY(!logo->property("animating").toBool());
        playing.clear();
        waitForOneCycle(64);
        logo->setProperty("hovered", false);

        // Several clicks during one transition coalesce into one final cycle.
        playing.clear();
        sidebar->setProperty("collapsed", false);
        QTRY_VERIFY(sidebar->width() > 64 && sidebar->width() < 280);
        sidebar->setProperty("collapsed", true);
        waitForOneCycle(64);

        playing.clear();
        window->setProperty("animations", false);
        sidebar->setProperty("collapsed", false);
        QCOMPARE(sidebar->width(), 280.0);
        QTest::qWait(100);
        QCOMPARE(playing.size(), 0);
        QVERIFY(!logo->property("animating").toBool());
        QCOMPARE(warnings.size(), 0);
    }

    void customWindowFrame() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(window->flags().testFlag(Qt::FramelessWindowHint));
        QVERIFY((window->flags() & Qt::WindowType_Mask) != Qt::Tool);
        std::unique_ptr<WindowFrame> frame;
        if (QGuiApplication::platformName() == "windows")
            frame = std::make_unique<WindowFrame>(reinterpret_cast<HWND>(window->winId()));
        window->show();
        QTest::qWait(150);
        QList<QQuickItem *> handles;
        QList<QQuickItem *> pending{window->contentItem()};
        while (!pending.isEmpty()) {
            auto *item = pending.takeLast();
            if (item->objectName().startsWith("windowResizeHandle_"))
                handles.append(item);
            pending.append(item->childItems());
        }
        QCOMPARE(handles.size(), 8);
        for (auto *edge : handles) {
            QVERIFY(edge->width() > 0);
            QVERIFY(edge->height() > 0);
        }
        auto *title = window->findChild<QQuickItem *>("customTitleBar");
        QVERIFY(title);
        QCOMPARE(title->height(), 44.0);

        auto *version = window->findChild<QQuickItem *>("sidebarVersion");
        QVERIFY(version);
        auto *sidebar = visualItem(window->contentItem(), "navigationSidebar");
        QVERIFY(sidebar);
        auto *logo = sidebar->findChild<QQuickItem *>("animatedSidebarLogo");
        QVERIFY(logo);
        // Check the default height and both sides of each sidebar breakpoint.
        // Previously the full sidebar overflowed between 800 and 860 pixels.
        const int minimumHeight = window->minimumHeight();
        window->setMinimumHeight(0);
        for (const int height : {700, 743, 744, 824, 843, 844, 884, 943, 944, 1000, 1043, 1044, 1100}) {
            window->resize(1160, height);
            QTest::qWait(50);
            const auto versionBottom = version->mapToItem(sidebar, QPointF(0, version->height())).y();
            QVERIFY2(versionBottom <= sidebar->height() - 19,
                     qPrintable(QString("Sidebar footer exceeds its bottom margin at window height %1")
                                    .arg(height)));
            if (height >= 744)
                QCOMPARE(logo->height(), 120.0);
        }
        window->setMinimumHeight(minimumHeight);
        window->resize(1160, 884);
        auto *maximise = window->findChild<QObject *>("windowMaximise");
        QVERIFY(maximise);
        QVERIFY(QMetaObject::invokeMethod(maximise, "clicked"));
        QTRY_COMPARE(window->visibility(), QWindow::Maximized);
        QVERIFY(window->minimumWidth() <= window->property("availableWindowWidth").toInt());
        QVERIFY(window->minimumHeight() <= window->property("availableWindowHeight").toInt());
        if (frame) {
            auto handle = reinterpret_cast<HWND>(window->winId());
            // QWindow visibility can change before the native resize completes.
            // Check screen coordinates too, so a taskbar overlap cannot pass.
            auto fitsWorkArea = [&]() {
                RECT rect{};
                MONITORINFO monitor{sizeof(MONITORINFO)};
                if (!GetClientRect(handle, &rect) ||
                    !GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor))
                    return false;
                POINT origin{};
                if (!ClientToScreen(handle, &origin))
                    return false;
                return rect.right > 0 && rect.bottom > 0 && origin.x >= monitor.rcWork.left &&
                       origin.y >= monitor.rcWork.top && origin.x + rect.right <= monitor.rcWork.right &&
                       origin.y + rect.bottom <= monitor.rcWork.bottom;
            };
            QTRY_VERIFY_WITH_TIMEOUT(fitsWorkArea(), 5000);
        }
        QVERIFY(QMetaObject::invokeMethod(maximise, "clicked"));
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);
        auto *minimise = window->findChild<QObject *>("windowMinimise");
        QVERIFY(minimise);
        QVERIFY(QMetaObject::invokeMethod(minimise, "clicked"));
        QTRY_COMPARE(window->visibility(), QWindow::Minimized);
        window->showNormal();
        QTest::qWait(100);
        if (!qEnvironmentVariableIsEmpty("WHAT_FRAME_PREVIEW"))
            window->grabWindow().save(qEnvironmentVariable("WHAT_FRAME_PREVIEW"));
        auto *close = window->findChild<QObject *>("windowClose");
        QVERIFY(close);
        QVERIFY(QMetaObject::invokeMethod(close, "clicked"));
        QTRY_VERIFY(!window->isVisible());
    }

    void publishingBrowser() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.stopService();
        UpdateManager updater(dir.path(), true);
        auto value = row("sharing_preview", 44.903315);
        value["unit"] = "%";
        value["name"] = "GPU usage";
        local.mergeReadings("windows", {value}, 1);
        local.setSensorPublishing("sharing_preview", 3, true);
        local.setBrowserFilters("GPU usage", "all", false, false);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *pages = window->findChild<QObject *>("pageStack");
        QVERIFY(pages);
        window->setProperty("previewPage", 1);
        QTRY_COMPARE(pages->property("shownIndex").toInt(), 1);
        auto *list = window->findChild<QQuickItem *>("sensorList");
        QVERIFY(list);
        QTRY_VERIFY(list->width() > 400);
        if (!qEnvironmentVariableIsEmpty("WHAT_SHARING_PREVIEW"))
            window->grabWindow().save(qEnvironmentVariable("WHAT_SHARING_PREVIEW"));
        window->setProperty("previewPage", 2);
        QTRY_COMPARE(pages->property("shownIndex").toInt(), 2);
        auto *shortcut = window->findChild<QObject *>("mqttSettingsShortcut");
        QVERIFY(shortcut);
        QVERIFY(QMetaObject::invokeMethod(shortcut, "clicked"));
        auto *editor = window->findChild<QObject *>("installationEditor");
        QVERIFY(editor);
        QVERIFY(QMetaObject::invokeMethod(editor, "edit", Q_ARG(QVariant, QVariantMap{})));
        auto *type = window->findChild<QObject *>("installationType");
        auto *host = window->findChild<QObject *>("installationMqttHost");
        QVERIFY(type && host);
        type->setProperty("currentIndex", 1);
        host->setProperty("text", "draft-broker.example");
        emit local.installationsChanged();
        QCoreApplication::processEvents();
        QCOMPARE(host->property("text").toString(), QString("draft-broker.example"));
        QCOMPARE(local.installations().size(), 1); // Merely opening the editor never creates a connection.
        QVERIFY(QMetaObject::invokeMethod(editor, "close"));
        auto *logo = window->findChild<QObject *>("logoOverviewButton");
        QVERIFY(logo);
        QVERIFY(QMetaObject::invokeMethod(logo, "clicked"));
        QCOMPARE(window->property("previewPage").toInt(), 0);
        window->hide();
    }

    void unsharedBrowserBulkSelection() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.stopService();
        local.mergeReadings("windows", {row("unshared_fixture_a", 1), row("unshared_fixture_b", 2)}, 1);
        local.setSensorPublishing("unshared_fixture_a", 1, false);
        local.setSensorPublishing("unshared_fixture_b", 0, false);
        local.setBrowserFilters("Fixture", "windows", false, false, true);
        auto visible = local.browser();
        int sensors = 0;
        for (const auto &entry : visible) {
            auto item = entry.toMap();
            if (!item["header"].toBool()) {
                ++sensors;
                QCOMPARE(item["id"].toString(), QString("unshared_fixture_b"));
            }
        }
        QCOMPARE(sensors, 1);
        local.collapseCategories(true);
        local.selectFiltered(true);
        QVERIFY(local.selected.contains("unshared_fixture_a"));
        QVERIFY(local.selected.contains("unshared_fixture_b"));
        QVERIFY(local.browser().isEmpty());
        local.setBrowserFilters("Fixture", "windows", true, false);
        local.selectFiltered(false);
        QVERIFY(!local.selected.contains("unshared_fixture_a"));
        QVERIFY(!local.selected.contains("unshared_fixture_b"));
    }

    void mqttLiveFixture() {
        const auto fixture = qEnvironmentVariable("WHAT_MQTT_FIXTURE");
        if (fixture.isEmpty())
            QSKIP("Opt-in MQTT broker fixture");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.icueTimer.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!local.windowsJob.isRunning() && !local.inventoryJob.isRunning() &&
                                     !local.slowJob.isRunning(),
                                 15000);
        QJsonArray rows;
        for (const auto &id : {"mqtt_local", "mqtt_cloud", "mqtt_full"}) {
            auto value = row(id, 44.903315);
            value["unit"] = "%";
            rows.append(value);
        }
        local.mergeReadings("windows", rows, 1);
        local.setSensorPublishing("mqtt_local", 1, false);
        local.setSensorPublishing("mqtt_cloud", 2, false);
        local.setSensorPublishing("mqtt_full", 3, true);
        local.publishTimer.setInterval(1000);
        local.publishTimer.start();
        local.publish();
        const auto ca = qEnvironmentVariable("WHAT_MQTT_CA");
        auto installation = local.installations().first().toMap();
        installation["mqtt"] = QVariantMap{{"host", "localhost"},
            {"port", qEnvironmentVariableIntValue("WHAT_MQTT_PORT")},
            {"scope", ca.isEmpty() ? "local" : "cloud"}, {"tls", !ca.isEmpty()},
            {"ca", ca}, {"username", ""}, {"certificate", ""}, {"key", ""}};
        QVERIFY(local.saveInstallation(installation));
        local.publish();
        QFile info(fixture + ".json");
        QVERIFY(info.open(QIODevice::WriteOnly));
        info.write(QJsonDocument(QJsonObject{{"port", local.activePort},
                                             {"token", local.token},
                                             {"device_id", local.deviceId},
                                             {"fingerprint", local.fingerprint}})
                       .toJson());
        info.close();
        QString previous;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 90000) {
            QFile commands(fixture + ".command");
            if (commands.open(QIODevice::ReadOnly)) {
                const auto command = QString::fromUtf8(commands.readAll()).trimmed();
                if (command == "done")
                    break;
                if (command != previous) {
                    previous = command;
                    if (command == "mqtt") {
                        auto options = local.installations().first().toMap()["mqtt"].toMap();
                        options["enabled"] = true;
                        QVERIFY(saveMqtt(local, options, {}));
                    }
                    else if (command == "direct")
                        setRemote(local, false, {});
                    else if (command == "revoke")
                        local.setSensorPublishing("mqtt_full", 1, true);
                }
            }
            QTest::qWait(50);
        }
        local.stopService();
    }

    void trayConnectionControlsFollowTransport() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        QCOMPARE(local.trayConnectionInfo()["start"].toString(), "Start Home Assistant Connections");
        QVERIFY(!local.trayConnectionInfo()["active"].toBool());
        local.startTrayConnection();
        QVERIFY(local.server.isListening());
        QVERIFY(local.trayConnectionInfo()["active"].toBool());
        local.stopTrayConnection();
        QVERIFY(!local.server.isListening());
        QVERIFY(saveMqtt(local, {{"host", "127.0.0.1"}, {"enabled", false}}, {}));
        const auto before = local.installations().first().toMap();
        local.startTrayConnection();
        QVERIFY(local.server.isListening());
        local.restartTrayConnection();
        QVERIFY(local.server.isListening());
        QCOMPARE(local.installations().first().toMap()["enabled"], before["enabled"]);
        local.stopTrayConnection();
        QVERIFY(!local.server.isListening());
    }

    void sensorBrowserCollapsesSearchResults() {
        const QVariantList readings{
            QVariantMap{{"id", "device"}, {"name", "Device colour"}, {"source", "icue"},
                        {"categoryKey", "icue / device"}, {"categoryName", "Device"}, {"controlOnly", true}},
            QVariantMap{{"id", "led"}, {"name", "LED colour"}, {"source", "icue"},
                        {"categoryKey", "icue / device / leds"}, {"categoryName", "Individual LEDs"},
                        {"parentCategory", "icue / device"}, {"controlOnly", true}},
            QVariantMap{{"id", "other"}, {"name", "Other colour"}, {"source", "icue"},
                        {"categoryKey", "icue / other"}, {"categoryName", "Other Device"}, {"controlOnly", true}}
        };
        for (const QString &query : {QString(), QString("colour"), QString("LED")}) {
            const QVariantMap filters{{"query", query}, {"source", "all"}};
            const auto folded = Controller::formatBrowser(readings, filters, {"icue / device"});
            bool parentFound = false;
            for (const auto &value : folded) {
                const auto row = value.toMap();
                QVERIFY(row["id"] != "device" && row["id"] != "led");
                QVERIFY(row["category"] != "icue / device / leds");
                if (row["category"] == "icue / device") {
                    QVERIFY(row["header"].toBool());
                    QVERIFY(row["collapsed"].toBool());
                    parentFound = true;
                }
            }
            QVERIFY(parentFound);
            const auto reopened = Controller::formatBrowser(readings, filters, {});
            QVERIFY(std::any_of(reopened.begin(), reopened.end(), [](const QVariant &value) {
                return value.toMap()["id"] == "led";
            }));
        }
    }

    void denseSensorBrowser() {
        QTest::failOnWarning(QRegularExpression(".*Binding loop.*"));
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        for (const auto *module : {"beacn", "wallpaper", "coretemp", "icue", "nvidia"})
            local.settings.setValue(QString("modules/%1/enabled").arg(module), true);
        local.setBrowserVisible(false);
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        local.acceptBeacn({{"ready", true}, {"connected", true}, {"controls", QVariantList{}}});
        QVariantList rows;
        const QStringList names = {
            "CPU Package",          "Core Temperatures", "CPU Package Power",    "Core Effective Clock",
            "Total CPU Usage",      "GPU Temperature",   "GPU Hot Spot",         "GPU Power",
            "GPU Memory Allocated", "GPU Fan",           "Physical Memory Load", "Drive Temperature"};
        const QStringList units = {"°C", "°C", "W", "MHz", "%", "°C", "°C", "W", "MiB", "RPM", "%", "°C"};
        const QList<double> values = {59.5, 62.0, 115.4, 5400, 12.6, 42.0, 51.0, 38.7, 2810, 900, 39, 37};
        for (int n = 0; n < 3010; ++n) {
            QVariantMap row{{"id", QString("dense_%1").arg(n)},
                            {"name", names[n % names.size()]},
                            {"header", false},
                            {"kind", "number"},
                            {"unit", units[n % units.size()]},
                            {"source", "hwinfo"},
                            {"value", values[n % values.size()]},
                            {"available", true},
                            {"shareScope", n % 4},
                            {"highFrequency", n % 5 == 0},
                            {"favourite", n % 7 == 0},
                            {"decimals", -1}};
            if (n == 0 || n == 13 || n == 17) {
                row = {{"header", true},
                       {"category", QString("group_%1").arg(n)},
                       {"name", n == 0    ? "HWiNFO / Intel Core i9-14900K"
                                : n == 13 ? "BEACN / Mix Create"
                                          : "NVIDIA / GeForce RTX"},
                       {"selected", 8},
                       {"total", 12},
                       {"writable", n == 13},
                       {"sharingCounts", QVariantMap{{"1", 8}, {"2", 4}, {"high", 2}}}};
            } else if (n == 14) {
                row["name"] = "Game Volume";
                row["source"] = "beacn";
                row["control"] = "beacn";
                row["controlOnly"] = true;
                row["beacn"] = QVariantMap{{"id", "volume_2"}, {"name", "Game Volume"},
                                           {"kind", "number"}, {"value", 65},
                                           {"min", 0},         {"max", 100},
                                           {"available", true}};
            } else if (n == 15) {
                row["name"] = "Keyboard Lighting";
                row["source"] = "icue";
                row["control"] = "rgb";
                row["controlOnly"] = true;
                row["value"] = "#28b3d1";
            } else if (n == 16) {
                row["name"] = "Wallpaper Playback";
                row["source"] = "wallpaper";
                row["control"] = "wallpaper";
                row["controlOnly"] = true;
            }
            rows.append(row);
        }
        local.browserModel.replace(rows);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)}});
        engine.loadData(R"(
            import QtQuick
            import "qrc:/qml/pages"
            import "qrc:/qml/controls"
            Window {
                id: root
                required property var controller
                width: 1250; height: 850; color: Qt.rgba(0.06, 0.09, 0.12, 1)
                property bool animate: true
                onAnimateChanged: Theme.animationsEnabled = animate
                SensorsPage { anchors.fill: parent; anchors.margins: 16; controller: root.controller }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *list = window->findChild<QQuickItem *>("sensorList");
        auto *page = window->findChild<QObject *>("sensorsPage");
        QVERIFY(list && page);
        QTRY_COMPARE(list->property("count").toInt(), 3010);
        for (const auto &unit : {QString("C"), QString("°C"), QString("F"), QString("°F"), QString("K")}) {
            for (double value : {59.3284, 60.0, -5.678}) {
                QVariant formatted;
                const QVariantMap reading{{"value", value}, {"unit", unit}, {"kind", "number"}, {"available", true}};
                QVERIFY(QMetaObject::invokeMethod(page, "showValue", Q_RETURN_ARG(QVariant, formatted),
                                                  Q_ARG(QVariant, reading)));
                QCOMPARE(formatted.toMap()["value"].toString(), QLocale().toString(value, 'f', 1));
                QCOMPARE(formatted.toMap()["unit"].toString(), unit);
            }
        }
        auto *content = list->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        auto rowAt = [&](int index) -> QQuickItem * {
            for (auto *item : content->childItems())
                if (item->property("index").toInt() == index && item->property("modelData").isValid() &&
                    !item->property("pooled").toBool())
                    return item;
            return nullptr;
        };
        auto capture = [&](const QString &name) {
            const QString output = qEnvironmentVariable("WHAT_REDESIGN_PREVIEW");
            if (!output.isEmpty()) {
                window->grabWindow();
                QTest::qWait(100);
                QVERIFY(window->grabWindow().save(output + "/dense-" + name + ".png"));
            }
        };
        for (int width : {1250, 820}) {
            window->resize(width, 850);
            QVERIFY(!window->grabWindow().isNull()); // Complete layout for the resized window.
            QVERIFY(QMetaObject::invokeMethod(list, "forceLayout"));
            // Windows can clamp this window to the runner's smaller desktop.
            // Check full viewport coverage using the fixture's 30/32 px rows.
            int expectedVisible = 0;
            qreal bottom = 0;
            for (const auto &row : rows) {
                bottom += row.toMap()["header"].toBool() ? 32 : 30;
                if (bottom > list->height())
                    break;
                ++expectedVisible;
            }
            QVERIFY(expectedVisible > 0);
            const auto visibleRowCount = [&] {
                int count = 0;
                const auto top = list->property("contentY").toReal();
                for (auto *item : content->childItems()) {
                    if (item->property("modelData").isValid() && !item->property("pooled").toBool()
                        && item->y() >= top && item->y() + item->height() <= top + list->height())
                        ++count;
                }
                return count;
            };
            QTRY_COMPARE(visibleRowCount(), expectedVisible);
            int delegates = 0;
            for (auto *item : content->childItems()) {
                if (!item->property("modelData").isValid())
                    continue;
                ++delegates;
            }
            QVERIFY(delegates < 55); // The other 3,000 readings must not create controls.
            for (int index : {0, 1, 13, 14}) {
                QVERIFY(rowAt(index));
                auto *badge = visualItem(rowAt(index), "sensorWritability");
                QVERIFY(badge);
                QCOMPARE(badge->property("text").toString(),
                         index >= 13 ? QString("Writable") : QString("Read Only"));
                QVERIFY(badge->x() >= 0);
                QVERIFY(badge->x() + badge->width() <= badge->parentItem()->width());
            }
            capture(QString::number(width));
        }
        window->resize(1250, 850);
        auto *volumeRow = rowAt(14);
        QVERIFY(volumeRow);
        QCOMPARE(volumeRow->height(), 30.0);
        QVERIFY(!visualItem(volumeRow, "beacnControl_volume_2"));
        auto *toggle = visualItem(volumeRow, "expand_dense_14");
        QVERIFY(toggle);
        QSignalSpy cacheChanges(list, SIGNAL(cacheBufferChanged()));
        QVERIFY(cacheChanges.isValid());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          toggle->mapToScene(QPointF(80, 15)).toPoint());
        QTRY_VERIFY(volumeRow->height() > 80);
        QTRY_VERIFY(visualItem(volumeRow, "beacnControl_volume_2"));
        QTRY_COMPARE(volumeRow->property("expansion").toReal(), 1.0);
        qInfo() << "Sensor expansion cache changes:" << cacheChanges.count();
        // Reserve the editor's full size once; changing the cache on every
        // reveal frame makes ListView repeatedly adjust its delegate pool.
        QVERIFY(cacheChanges.count() <= 4);
        cacheChanges.clear();
        capture("audio-expanded");
        // Live samples must update the expanded row without replacing its editor.
        auto *editor = visualItem(volumeRow, "beacnControl_volume_2");
        auto updated = rows[14].toMap();
        auto beacn = updated["beacn"].toMap();
        beacn["value"] = 42;
        updated["beacn"] = beacn;
        rows[14] = updated;
        local.browserModel.update(rows);
        QTRY_COMPARE(editor->property("control").toMap()["value"].toInt(), 42);
        QCOMPARE(visualItem(volumeRow, "beacnControl_volume_2"), editor);
        page->setProperty("expandedId", "dense_15");
        QTRY_COMPARE(volumeRow->height(), 30.0);
        QTRY_VERIFY(!visualItem(volumeRow, "beacnControl_volume_2"));
        auto *colourRow = rowAt(15);
        QVERIFY(colourRow);
        QTRY_VERIFY(colourRow->height() > 120);
        QTRY_COMPARE(colourRow->property("expansion").toReal(), 1.0);
        qInfo() << "Sensor editor switch cache changes:" << cacheChanges.count();
        QVERIFY(cacheChanges.count() <= 6);
        capture("colour-expanded");
        page->setProperty("expandedId", "dense_16");
        QTRY_VERIFY(visualItem(window->contentItem(), "inlineWallpaperControls"));
        // A second click can cancel construction before an asynchronous editor is ready.
        page->setProperty("expandedId", "");
        QTRY_VERIFY(!visualItem(window->contentItem(), "inlineWallpaperControls"));
        page->setProperty("expandedId", "dense_15");
        page->setProperty("expandedId", "dense_14");
        page->setProperty("expandedId", "");
        QTRY_COMPARE(volumeRow->height(), 30.0);
        QTRY_COMPARE(colourRow->height(), 30.0);
        QVERIFY(!visualItem(window->contentItem(), "inlineColour_dense_15"));
        QVERIFY(!visualItem(window->contentItem(), "beacnControl_volume_2"));
        // Switch editors while their asynchronous creation and reveal overlap.
        // Live samples and garbage collection must not leave an editor referencing
        // controls that the previous row has already destroyed.
        for (int attempt = 0; attempt < 120; ++attempt) {
            page->setProperty("expandedId", QString("dense_%1").arg(14 + attempt % 3));
            auto liveRow = rows[14].toMap();
            auto liveControl = liveRow["beacn"].toMap();
            liveControl["value"] = attempt % 100;
            liveRow["beacn"] = liveControl;
            rows[14] = liveRow;
            local.browserModel.update(rows);
            QTest::qWait(attempt % 4 == 0 ? 1 : 40);
            if (attempt % 7 == 0)
                engine.collectGarbage();
        }
        page->setProperty("expandedId", "");
        QTRY_COMPARE(volumeRow->height(), 30.0);
        QTRY_COMPARE(colourRow->height(), 30.0);
        // The instant path used to feed Loader.active back through row height.
        window->setProperty("animate", false);
        for (const QString &id : {QString("dense_14"), QString("dense_15"), QString("dense_1"), QString()}) {
            page->setProperty("expandedId", id);
            QTest::qWait(20);
            if (id == "dense_1")
                capture("reading-permissions");
        }
        auto *header = visualItem(rowAt(0), "expand_group_0");
        QVERIFY(header);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          header->mapToScene(QPointF(80, 15)).toPoint());
        QVERIFY(local.browserFolding.isCollapsed("group_0"));
        window->setProperty("animate", true);
        // Closing and recycling cannot leak the previous editor into another row.
        page->setProperty("expandedId", "");
        QTRY_COMPARE(colourRow->height(), 30.0);
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 1500), Q_ARG(int, 0)));
        QTest::qWait(100);
        for (auto *item : content->childItems()) {
            if (!item->property("modelData").isValid() || item->property("pooled").toBool())
                continue;
            QCOMPARE(item->height(), item->property("modelData").toMap()["header"].toBool() ? 32.0 : 30.0);
            auto *loader = visualItem(item, "sensorDetailLoader");
            QVERIFY(loader && !loader->property("active").toBool());
        }
        // Scrolling starts asynchronous creation of the next cached rows.
        if (auto *incubation = engine.incubationController())
            QTRY_COMPARE_WITH_TIMEOUT(incubation->incubatingObjectCount(), 0, 5000);
        window->hide();
    }

    void inlineSensorColour() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.sampleTimer.stop();
        local.publishTimer.stop();
        local.icueTimer.stop();
        local.setModuleEnabled("icue", true);
        QTRY_VERIFY(!local.icueJob.isRunning());
        IcueSdk sdk(QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(ICUE_TEST_DLL));
        local.acceptIcue(sdk.poll(true));
        local.setBrowserFilters("Device RGB lighting", "icue", false, false);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 1);
        QTRY_VERIFY(openBrowserControl(window, local, "rgb"));
        auto *page = window->findChild<QObject *>("sensorsPage");
        QVERIFY(page);
        const QString id = page->property("expandedId").toString();
        QPointer<QQuickItem> editor;
        QTRY_VERIFY((editor = visualItem(window->contentItem(), "inlineColour_" + id)));
        QTRY_VERIFY(editor && editor->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(page, "applyFilters"));
        QCOMPARE(page->property("expandedId").toString(), id);
        QSignalSpy replies(&local, &Controller::localIcueReply);
        QVERIFY(QMetaObject::invokeMethod(editor, "choose", Q_ARG(QVariant, QVariant("#8044bbff"))));
        QCOMPARE(editor->property("draft").value<QColor>(), QColor("#44bbff"));
        QVERIFY(replies.isEmpty()); // Previewing a colour does not write to hardware.
        QVERIFY(QMetaObject::invokeMethod(editor, "apply"));
        QTRY_VERIFY(!replies.isEmpty());
        QVERIFY(replies.last()[1].toBool());
        QTRY_COMPARE(local.colourPalette()->recent(), QStringList{"#44bbff"});
        QVERIFY(editor);
        QCOMPARE(editor->property("result").toString(), QString("Colour applied"));

        QTRY_VERIFY(!local.icueJob.isRunning());
        auto gate = std::make_shared<QSemaphore>();
        const auto release = qScopeGuard([gate] { gate->release(); });
        local.icueJob.post([gate] { gate->acquire(); });
        QVERIFY(QMetaObject::invokeMethod(editor, "choose", Q_ARG(QVariant, QVariant("#663399"))));
        QVERIFY(QMetaObject::invokeMethod(editor, "apply"));
        page->setProperty("expandedId", "");
        QTRY_VERIFY(!visualItem(window->contentItem(), "inlineColour_" + id));
        gate->release();
        QTRY_VERIFY(local.colourPalette()->recent().contains("#663399"));
        window->hide();
    }

    void sensorModuleFilterClicks_data() {
        QTest::addColumn<bool>("collapsed");
        QTest::newRow("expanded") << false;
        QTest::newRow("collapsed-at-startup") << true;
    }

    void sensorModuleFilterClicks() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QFETCH(bool, collapsed);
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(collapsed);
        UpdateManager updater(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.setBrowserVisible(false);
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        for (const auto *module : {"wallpaper", "coretemp", "icue", "nvidia"})
            local.settings.setValue(QString("modules/%1/enabled").arg(module), true);
        local.saveBeacn(true, false, false);
        local.acceptBeacn({{"ready", true}, {"connected", true},
                          {"controls", QVariantList{QVariantMap{{"id", "volume_2"}, {"name", "Game volume"},
                                                               {"kind", "number"}, {"value", 50},
                                                               {"available", true}, {"min", 0}, {"max", 100}}}}});
        QJsonArray readings;
        for (int index = 0; index < 3000; ++index)
            readings.append(QJsonObject{{"id", QString("filter_fixture_%1").arg(index)},
                                        {"source", "hwinfo"}, {"group", "Filter Fixture"},
                                        {"name", QString("Reading %1").arg(index)},
                                        {"kind", "number"}, {"value", index}, {"available", true}});
        local.mergeReadings("hwinfo", readings, readings.size());
        local.setBrowserFilters("", "all", false, false);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                     {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 1);
        auto *stack = window->findChild<QObject *>("pageStack");
        QVERIFY(stack);
        QTRY_VERIFY(!stack->property("moving").toBool());
        auto *button = window->findChild<QQuickItem *>("beacnFilter");
        QVERIFY(button && button->isVisible());
        auto beacnRows = [&] {
            int count = 0;
            for (const auto &value : local.browserModel.snapshot()) {
                const auto row = value.toMap();
                count += row["source"] == "beacn" ||
                         row["category"].toString().startsWith("beacn / ");
            }
            return count;
        };
        QTRY_VERIFY(beacnRows() > 0);
        const int allBeacn = beacnRows();
        for (int width : {1160, 1000, 1440}) {
            window->resize(width, 820);
            QTest::qWait(50);
            for (bool show : {false, true}) {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
                QCOMPARE(button->property("checked").toBool(), show);
                QTRY_COMPARE(beacnRows(), show ? allBeacn : 0);
                QVERIFY(local.beacnState()["enabled"].toBool());
            }
        }
        // A previous scroll must not keep the new filter waiting for list updates.
        local.setBrowserInteracting(true);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
        QVERIFY(!button->property("checked").toBool());
        QVERIFY(!local.browserInteracting);
        QTRY_COMPARE(beacnRows(), 0);
        window->hide();
    }

    void sensorGroupActionsDuringLiveUpdates() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        UpdateManager updater(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.setModuleEnabled("windows", false);
        local.setBrowserVisible(false);
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        QJsonArray frames[2];
        for (int n = 0; n < 3000; ++n) {
            QJsonObject row{{"id", QString("live_%1").arg(n)},
                            {"source", "hwinfo"},
                            {"group", QString("Device%1").arg(n / 150, 2, 10, QChar('0'))},
                            {"name", QString("Reading %1").arg(n)},
                            {"kind", "number"},
                            {"value", n},
                            {"available", true}};
            frames[0].append(row);
            row["value"] = n + 0.5;
            frames[1].append(row);
        }
        local.mergeReadings("hwinfo", frames[0]);
        for (int n = 0; n < 3000; ++n) {
            const auto id = QString("live_%1").arg(n);
            local.selected.insert(id);
            local.settings.setValue("publishing/" + id,
                                    QVariantMap{{"scope", 3}, {"high", false}, {"decimals", -1}});
        }
        local.refreshPublishingPolicies();
        local.settings.setValue("selected", local.selected.values());
        local.latest.valid = true;
        local.latest.readings = frames[0];
        local.latest.pollTime = QDateTime::currentSecsSinceEpoch();
        local.initialAge = 0;
        local.pollClock.start();
        local.setBrowserFilters("", "hwinfo", false, false);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 1);
        auto *stack = window->findChild<QObject *>("pageStack");
        QVERIFY(stack);
        QTRY_VERIFY(!stack->property("moving").toBool());
        QTRY_COMPARE(local.browserModel.rowCount(), 3020);
        int sample = 0;
        QTimer samples;
        connect(&samples, &QTimer::timeout, this, [&] {
            if (local.hwinfoJob.isRunning())
                return;
            const auto prepare = local.prepareReadingsUpdate("hwinfo");
            const auto frame = frames[++sample % 2];
            local.hwinfoJob.submit([prepare, frame] { return prepare(frame); },
                                   [&](Controller::ReadingsUpdate update) {
                                       local.applyReadingsUpdate("hwinfo", update);
                                       local.pollClock.restart();
                                       emit local.sensorsChanged();
                                       local.hwinfoJob.post([update = std::move(update)] {});
                                   });
        });
        samples.start(100);
        QQuickItem *toggle = nullptr;
        QTRY_VERIFY((toggle = visualItem(window->contentItem(), "expand_live_0")));
        auto *reading = toggle->parentItem();
        auto toggleReading = [&] {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              toggle->mapToScene(QPointF(80, 15)).toPoint());
        };
        toggleReading();
        QTRY_COMPARE(reading->property("expansion").toReal(), 1.0);
        QVERIFY(reading->property("readingAvailable").toBool());
        const auto previousValue = reading->property("readingValue");
        QTRY_VERIFY(reading->property("readingValue") != previousValue);
        toggleReading();
        QTRY_COMPARE(reading->property("expansion").toReal(), 0.0);
        auto *categoryToggle = visualItem(window->contentItem(), "expand_hwinfo / Device00");
        QVERIFY(categoryToggle);
        for (int i = 0; i < 3; ++i) {
            const auto point = categoryToggle->mapToScene(QPointF(80, 15)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point, 0);
            QCOMPARE(local.browserModel.rowCount(), 2870);
            QVERIFY(categoryToggle->parentItem()->property("categoryCollapsed").toBool());
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point, 0);
            QCOMPARE(local.browserModel.rowCount(), 3020);
            QVERIFY(!categoryToggle->parentItem()->property("categoryCollapsed").toBool());
        }
        auto *page = window->findChild<QObject *>("sensorsPage");
        auto *categoryList = window->findChild<QQuickItem *>("sensorList");
        QVERIFY(page && categoryList);
        auto *categoryContent = categoryList->property("contentItem").value<QQuickItem *>();
        QVERIFY(categoryContent);
        QVERIFY(QMetaObject::invokeMethod(page, "foldGroups", Q_ARG(QVariant, true)));
        qint64 slowestCategory = 0;
        int mostDelegates = 0;
        // Each click interrupts the previous category's transition. Large groups
        // must not instantiate their off-screen readings during those transitions.
        for (bool close : {false, true}) {
            for (int group = 19; group >= 0; --group) {
                QElapsedTimer elapsed;
                elapsed.start();
                const auto name = QString("hwinfo / Device%1").arg(group, 2, 10, QChar('0'));
                QVERIFY(QMetaObject::invokeMethod(page, "foldCategory", Q_ARG(QVariant, name),
                                                 Q_ARG(QVariant, close)));
                slowestCategory = qMax(slowestCategory, elapsed.elapsed());
                QTest::qWait(15);
                int count = 0;
                for (const auto *item : categoryContent->childItems())
                    count += item->property("modelData").isValid();
                mostDelegates = qMax(mostDelegates, count);
            }
            QCOMPARE(local.browserModel.rowCount(), close ? 20 : 3020);
        }
        qInfo() << "Rapid category expansion: slowest click" << slowestCategory
                << "ms, peak delegates" << mostDelegates;
        QVERIFY2(slowestCategory < 1000, "A category click blocked the GUI for over a second");
        QVERIFY2(mostDelegates < 200, "Category transitions created off-screen readings");
        auto click = [&](const char *name) {
            auto *button = window->findChild<QQuickItem *>(name);
            QVERIFY(button && button->isVisible());
            QTest::mouseClick(
                window, Qt::LeftButton, Qt::NoModifier,
                button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
        };
        click("collapseSensorGroups");
        QCOMPARE(local.browserModel.rowCount(), 20);
        for (const auto &value : local.browserModel.snapshot()) {
            QVERIFY(value.toMap()["header"].toBool());
            QVERIFY(value.toMap()["collapsed"].toBool());
        }
        click("expandSensorGroups");
        QCOMPARE(local.browserModel.rowCount(), 3020);
        local.setBrowserInteracting(true);
        click("collapseSensorGroups");
        QCOMPARE(local.browserModel.rowCount(), 20);
        click("expandSensorGroups");
        QCOMPARE(local.browserModel.rowCount(), 3020);
        local.setBrowserFilters("Device00", "hwinfo", false, false);
        QTRY_COMPARE(local.browserModel.rowCount(), 151);
        click("collapseSensorGroups");
        QCOMPARE(local.browserModel.rowCount(), 1);
        QCOMPARE(local.browserFolding.closed(local.browserCategories).size(), 20);
        local.setBrowserFilters("", "hwinfo", false, false);
        QTRY_COMPARE(local.browserModel.rowCount(), 20);
        click("expandSensorGroups");
        QCOMPARE(local.browserModel.rowCount(), 3020);
        samples.stop();
        QTRY_VERIFY(!local.hwinfoJob.isRunning());
        auto *list = window->findChild<QQuickItem *>("sensorList");
        QVERIFY(list);
        auto *content = qvariant_cast<QQuickItem *>(list->property("contentItem"));
        QVERIFY(content);
        int delegates = 0;
        for (const auto *item : content->childItems())
            delegates += item->property("modelData").isValid();
        // Transitions must keep virtualization, including their temporary exiting rows.
        QVERIFY(delegates < 512);
        window->hide();
    }

    void sensorCategoryTransitions() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        UpdateManager updater(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.setModuleEnabled("windows", false);
        QJsonArray readings;
        for (int n = 0; n < 6; ++n) {
            auto item = row(QString("animated_%1").arg(n), n);
            item["source"] = "hwinfo";
            item["group"] = n < 3 ? "A" : "B";
            readings.append(item);
        }
        local.mergeReadings("hwinfo", readings);
        local.setBrowserFilters("", "hwinfo", false, false);
        local.setUiTiming("panels", 600);
        local.setUiTiming("lists", 600);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 1);
        auto *stack = window->findChild<QObject *>("pageStack");
        QVERIFY(stack);
        QTRY_VERIFY(!stack->property("moving").toBool());
        QTRY_COMPARE(local.browserModel.rowCount(), 8);
        auto *page = window->findChild<QObject *>("sensorsPage");
        auto *added = window->findChild<QObject *>("categoryRowsAdded");
        auto *removed = window->findChild<QObject *>("categoryRowsRemoved");
        auto *displaced = window->findChild<QObject *>("categoryRowsDisplaced");
        QVERIFY(page && added && removed && displaced);
        auto *category = visualItem(window->contentItem(), "expand_hwinfo / A");
        auto *following = visualItem(window->contentItem(), "expand_hwinfo / B");
        QVERIFY(category && following);
        auto *followingRow = following->parentItem();
        QPointer<QQuickItem> exitingRow =
            visualItem(window->contentItem(), "expand_animated_0")->parentItem();
        const qreal expandedY = followingRow->y();
        const qreal closedY = expandedY - 90;
        auto click = [&] {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              category->mapToScene(QPointF(80, 15)).toPoint(), 0);
        };
        click();
        QCOMPARE(local.browserModel.rowCount(), 5);
        QVERIFY(removed->property("running").toBool());
        QVERIFY(displaced->property("running").toBool());
        QTRY_VERIFY(followingRow->y() < expandedY && followingRow->y() > closedY);
        QVERIFY(!exitingRow || !exitingRow->isEnabled());
        QTRY_VERIFY(!exitingRow || exitingRow->opacity() == 0);
        QTRY_COMPARE(followingRow->y(), closedY);
        click();
        QCOMPARE(local.browserModel.rowCount(), 8);
        QVERIFY(added->property("running").toBool());
        QTRY_VERIFY(followingRow->y() > closedY && followingRow->y() < expandedY);
        // Reverse an unfinished expansion, then reopen it. No row may remain faded or disabled.
        click();
        QCOMPARE(local.browserModel.rowCount(), 5);
        click();
        QCOMPARE(local.browserModel.rowCount(), 8);
        QTRY_COMPARE(followingRow->y(), expandedY);
        auto *list = window->findChild<QQuickItem *>("sensorList");
        QVERIFY(list);
        auto *content = qvariant_cast<QQuickItem *>(list->property("contentItem"));
        QVERIFY(content);
        for (int n = 0; n < 6; ++n) {
            auto reading = [&]() -> QQuickItem * {
                for (auto *item : content->childItems())
                    if (!item->property("retiring").toBool() && !item->property("pooled").toBool() &&
                        item->property("modelData").toMap()["id"] == QString("animated_%1").arg(n))
                        return item;
                return nullptr;
            };
            QTRY_VERIFY(reading());
            QVERIFY(reading()->isEnabled());
            QTRY_COMPARE(reading()->opacity(), 1.0);
            QTRY_COMPARE(reading()->property("revealOffset").toReal(), 0.0);
        }
        local.setAnimationsEnabled(false);
        QTRY_VERIFY(!window->property("animationsEnabled").toBool());
        click();
        QCOMPARE(local.browserModel.rowCount(), 5);
        QCOMPARE(followingRow->y(), closedY);
        local.setUiTiming("panels", 0);
        local.setUiTiming("lists", 0);
        local.setAnimationsEnabled(true);
        QTRY_VERIFY(window->property("animationsEnabled").toBool());
        click();
        QCOMPARE(local.browserModel.rowCount(), 8);
        QCOMPARE(followingRow->y(), expandedY);
        window->hide();
    }

    void sensorFiltersWrapWithAllModules() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.stopService();
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        window->setProperty("previewPage", 1);
        auto *filters = window->findChild<QQuickItem *>("sensorFilterFlow");
        QVERIFY(filters);
        // Exercise the maximum filter row regardless of locally installed providers.
        for (auto *child : filters->childItems())
            QVERIFY(QQmlProperty::write(child, "visible", true));
        for (int width : {1160, 1000}) {
            window->resize(width, 824);
            QTest::qWait(100);
            QVERIFY(filters->height() >= 28);
            for (auto *child : filters->childItems()) {
                QVERIFY(child->x() >= 0);
                QVERIFY(child->x() + child->width() <= filters->width() + 1);
                QVERIFY(child->y() + child->height() <= filters->height() + 1);
            }
            auto *content = window->findChild<QQuickItem *>("mainContentLayout");
            QVERIFY(content);
            QVERIFY(content->mapToScene(QPointF(content->width(), 0)).x() <= window->width());
        }
        auto *check = window->findChild<QObject *>("windowsFilter");
        QVERIFY(check);
        check->setProperty("checked", true);
        QVERIFY(check->property("selected").toBool());
        check->setProperty("checked", false);
        QVERIFY(!check->property("selected").toBool());
        window->hide();
    }

    void sharedToolTips_data() {
        QTest::addColumn<QString>("windowName");
        QTest::newRow("inside-WHAT") << QString();
        QTest::newRow("detached-Web-window") << QString("homeAssistantWebOverlay");
        QTest::newRow("desktop-Web-window") << QString("homeAssistantWebDesktop");
    }

    void cachedSensorCheckboxCancellation() {
        QQmlIncubationController creation;
        IncubationCheckpoint checkpoint;
        QQmlEngine engine;
        engine.setIncubationController(&creation);
        engine.rootContext()->setContextProperty("checkpoint", &checkpoint);
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQuick
            import "qrc:/qml/controls"
            OptionCheckBox {
                width: 44; height: 26
                indicatorCentered: true
                indicatorVisible: false
                property bool stopHere: checkpoint.reached()
            }
        )QML", QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QQmlIncubator pending(QQmlIncubator::Asynchronous);
        component.create(pending);
        creation.incubateWhile(&checkpoint.keepGoing);
        QVERIFY(!checkpoint.keepGoing.load());
        QVERIFY(pending.isLoading());
        // Filtering or removing a module may discard a cached row at this point.
        // Engine destruction must not report unfinished deferred indicators.
        pending.clear();
        QCOMPARE(creation.incubatingObjectCount(), 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void sharedToolTips() {
        QFETCH(QString, windowName);
        // This checks the controls' hover bindings and native popup layering.
        // Keep desktop cursor events from changing the target while a delayed
        // tooltip is opening; send ordinary Qt hover events to each control.
        class HoverInput final : public QObject {
        public:
            void send(QQuickItem *control, QEvent::Type type) {
                const QPointF position(control->width() / 2, control->height() / 2);
                QHoverEvent event(type, position, control->mapToGlobal(position), position);
                sending = true;
                QCoreApplication::sendEvent(control, &event);
                sending = false;
            }

            bool eventFilter(QObject *, QEvent *event) override {
                return !sending && (event->type() == QEvent::HoverEnter
                    || event->type() == QEvent::HoverMove || event->type() == QEvent::HoverLeave);
            }

        private:
            bool sending = false;
        } hoverInput;
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"(
            import QtQuick
            import QtQuick.Controls.Basic
            import "qrc:/qml/controls"
            ApplicationWindow {
                id: window
                width: 320; height: 320
                color: "black"
                property var sharedTip: first.ToolTip.toolTip
                property var otherTip: second.ToolTip.toolTip
                // Reproduce the browser's native child without loading a site.
                WindowContainer {
                    width: parent.width
                    height: 220
                    window: Window {
                        objectName: "nativeWebFrame"
                        color: "navy"
                    }
                }
                function prepareHandoffs() {
                    first.ToolTip.visible = false;
                    second.ToolTip.visible = false;
                    first.hoverEnabled = false;
                    second.hoverEnabled = false;
                }
                function showFirst() {
                    first.ToolTip.delay = 0;
                    first.ToolTip.timeout = -1;
                    first.ToolTip.show(first.ToolTip.text);
                }
                function showSecond() {
                    second.ToolTip.show(second.ToolTip.text);
                    // Hover delivery can reach the new control before the old
                    // control's exit. Its hide must not close the new hint.
                    first.ToolTip.hide();
                }
                ActionButton {
                    id: first
                    objectName: "first"
                    x: 12; y: 260; width: 140
                    text: "Copy Diagnostics"
                    ToolTip.text: "Copy a redacted report with connection status, module versions and recent activity. Pairing secrets are excluded from the report."
                }
                Button {
                    id: second
                    objectName: "second"
                    x: 210; y: 260; width: 98
                    text: "Help"
                    hoverEnabled: true
                    ToolTip.text: "Open help."
                    ToolTip.visible: hovered
                }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->setObjectName(windowName);
        window->setPosition(0, 0);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *frame = window->findChild<QWindow *>("nativeWebFrame");
        QVERIFY(frame);
        QVERIFY(QTest::qWaitForWindowExposed(frame));
        for (const auto *name : {"first", "second"}) {
            auto *control = window->findChild<QQuickItem *>(name);
            QVERIFY(control);
            control->installEventFilter(&hoverInput);
            hoverInput.send(control, QEvent::HoverLeave);
        }
        auto *tip = window->property("sharedTip").value<QObject *>();
        QVERIFY(tip);
        QCOMPARE(tip->objectName(), QString("whatToolTip"));
        QCOMPARE(tip, window->property("otherTip").value<QObject *>());
        QVERIFY(!tip->property("enter").value<QObject *>());
        QVERIFY(!tip->property("exit").value<QObject *>());
        QVERIFY(!tip->property("popupMotion").isValid());
        for (const auto *name : {"first", "second"}) {
            auto *control = window->findChild<QQuickItem *>(name);
            QVERIFY(control);
            hoverInput.send(control, QEvent::HoverEnter);
            QTRY_VERIFY(control->property("hovered").toBool());
            QTRY_VERIFY(tip->property("visible").toBool());
            QTRY_COMPARE(tip->property("parent").value<QQuickItem *>(), control);
            auto *content = tip->property("contentItem").value<QQuickItem *>();
            QVERIFY(content);
            // An item popup would be obscured by the native web frame. The
            // window must belong to this host, and hovering must keep Web shown.
            QTRY_VERIFY(content->window() && content->window() != window);
            QVERIFY(content->window()->isTopLevel());
            QCOMPARE(content->window()->transientParent(), window);
            QVERIFY(frame->isVisible());
            const QRectF bounds = content->mapRectToScene(content->boundingRect());
            QVERIFY(bounds.left() >= 0 && bounds.right() <= window->width());
            QVERIFY(bounds.top() >= 0 && bounds.bottom() <= window->height());
            if (control->objectName() == "first") {
                QVERIFY(content->property("lineCount").toInt() > 1);
                if (!qEnvironmentVariableIsEmpty("WHAT_REDESIGN_PREVIEW"))
                    window->grabWindow().save(qEnvironmentVariable("WHAT_REDESIGN_PREVIEW") + "/tooltip.png");
            }
            hoverInput.send(control, QEvent::HoverLeave);
            QVERIFY(!control->property("hovered").toBool());
            QTRY_VERIFY(!tip->property("visible").toBool());
        }

        QVERIFY(QMetaObject::invokeMethod(window, "prepareHandoffs"));
        QVERIFY(QMetaObject::invokeMethod(window, "showFirst"));
        QVERIFY(tip->property("opened").toBool());

        // A shared hint changes text and owner immediately. A late hide from
        // the previous control must not close it for the new control.
        QVERIFY(QMetaObject::invokeMethod(window, "showSecond"));
        QCOMPARE(tip->property("text").toString(), QString("Open help."));
        QCOMPARE(tip->property("parent").value<QQuickItem *>(), window->findChild<QQuickItem *>("second"));
        QVERIFY(tip->property("opened").toBool());

        // Closing and reopening must not leave a transition or render layer.
        QVERIFY(QMetaObject::invokeMethod(tip, "close"));
        QVERIFY(!tip->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(window, "showFirst"));
        QVERIFY(tip->property("opened").toBool());
        auto *content = tip->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        QVERIFY(!QQmlProperty::read(content->parentItem(), "layer.enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(tip, "close"));
        QVERIFY(!tip->property("visible").toBool());
        QCOMPARE(warnings.count(), 0);
    }

    void dropdownSlideAndRefraction() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"(
            import QtQuick
            import QtQuick.Controls.Basic
            import "qrc:/qml/controls"
            ApplicationWindow {
                width: 480; height: 430; color: Theme.canvas
                Component.onCompleted: {
                    Theme.animationsEnabled = true;
                    Theme.transitionStyle = "refraction";
                    Theme.transitionMode = 7;
                    Theme.timings = Object.assign({}, Theme.timings, {panels: 1000});
                }
                ChoiceBox {
                    objectName: "choice"
                    x: 30; y: 30; width: 360
                    model: ["First installation", "Second installation", "Third installation", "Fourth installation"]
                }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *choice = window->findChild<QObject *>("choice");
        QVERIFY(choice);
        auto *popup = choice->property("popup").value<QObject *>();
        QVERIFY(popup);
        auto *effect = popup->property("refraction").value<QObject *>();
        QVERIFY(effect);
        auto *list = effect->property("target").value<QQuickItem *>();
        QVERIFY(list);
        QCOMPARE(list->property("count").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("reveal").toDouble() > 0.15
            && popup->property("reveal").toDouble() < 0.85, 1200);
        QVERIFY(popup->property("height").toDouble() < popup->property("implicitHeight").toDouble());
        QVERIFY(effect->property("active").toBool());
        if (!qEnvironmentVariableIsEmpty("WHAT_DROPDOWN_PREVIEW"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_DROPDOWN_PREVIEW") + "-opening.png"));
        QTRY_VERIFY(popup->property("opened").toBool());
        QVERIFY(!effect->property("active").toBool());

        // The first row must already be drawn while the slide is only partly
        // open. A centre-out lens plus the height clip used to hide it entirely.
        popup->setProperty("reveal", 0.25);
        const auto moving = window->grabWindow();
        const auto origin = list->mapToScene(QPointF(8, 8));
        const qreal ratio = window->devicePixelRatio();
        const auto firstLetters = moving.copy(QRect(qRound(origin.x() * ratio), qRound(origin.y() * ratio),
                                                    qRound(80 * ratio), qRound(16 * ratio)));
        int visibleText = 0;
        for (int y = 0; y < firstLetters.height(); ++y)
            for (int x = 0; x < firstLetters.width(); ++x) {
                const auto colour = firstLetters.pixelColor(x, y);
                visibleText += std::max({colour.red(), colour.green(), colour.blue()}) > 120;
            }
        if (!qEnvironmentVariableIsEmpty("WHAT_DROPDOWN_PREVIEW"))
            QVERIFY(moving.save(qEnvironmentVariable("WHAT_DROPDOWN_PREVIEW") + "-quarter.png"));
        QVERIFY2(visibleText > 30, "The sliding dropdown hid the first row behind its lens mask");
        popup->setProperty("reveal", 1.0);
        QVERIFY(!QQmlProperty::read(list, "layer.enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(popup, "close"));
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("reveal").toDouble() > 0.15
            && popup->property("reveal").toDouble() < 0.85, 1200);
        QVERIFY(effect->property("active").toBool());
        QTRY_VERIFY(!popup->property("visible").toBool());
        QVERIFY(!effect->property("active").toBool());
        QCOMPARE(list->property("count").toInt(), 4);
        QSignalSpy countChanges(list, SIGNAL(countChanged()));
        QVERIFY(countChanges.isValid());
        // Reopening must reuse the populated list instead of dropping and
        // rebuilding its delegates on the first animation frame.
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY(popup->property("opened").toBool());
        QCOMPARE(countChanges.count(), 0);
        QVERIFY(QMetaObject::invokeMethod(popup, "close"));
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(countChanges.count(), 0);
        choice->setProperty("model", QStringList{"Updated installation", "Another installation"});
        QTRY_COMPARE(list->property("count").toInt(), 2);
        QCOMPARE(choice->property("displayText").toString(), QString("Updated installation"));
        // Only visited, visible controls retain rows. Returning to a hidden
        // page must not eagerly construct all of its previously used dropdowns.
        choice->setProperty("visible", false);
        QTRY_COMPARE(list->property("count").toInt(), 0);
        choice->setProperty("visible", true);
        QCOMPARE(list->property("count").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY(popup->property("opened").toBool());
        QCOMPARE(list->property("count").toInt(), 2);
        QCOMPARE(warnings.size(), 0);
    }

    void sharedControlInteractions() {
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.loadData(R"(
            import QtQuick
            import QtQuick.Controls.Basic
            import QtQuick.Layouts
            import "qrc:/qml/controls"
            ApplicationWindow {
                width: 400; height: 360
                color: Theme.canvas
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spaceL
                    ChoiceBox {
                        objectName: "choice"
                        model: Array.from({length: 200}, (_, i) => "Reading " + i)
                        Layout.fillWidth: true
                    }
                    InputField { objectName: "field"; Layout.fillWidth: true }
                    ValueSlider { objectName: "slider"; from: 0; to: 100; stepSize: 1; value: 50 }
                    OptionCheckBox { objectName: "check"; text: "Share reading" }
                    ToggleSwitch { objectName: "toggle" }
                    Item { Layout.fillHeight: true }
                }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        auto *choice = window->findChild<QQuickItem *>("choice");
        QVERIFY(choice);
        choice->forceActiveFocus(Qt::TabFocusReason);
        QTRY_COMPARE(window->activeFocusItem(), choice);
        QTest::keyClick(window, Qt::Key_Space);
        auto *popup = choice->property("popup").value<QObject *>();
        QVERIFY(popup);
        QTRY_VERIFY(popup->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_End);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(choice->property("currentIndex").toInt(), 199);
        QCOMPARE(choice->property("displayText").toString(), QString("Reading 199"));
        QTRY_VERIFY(!popup->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(popup->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Home);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("visible").toBool());
        QCOMPARE(choice->property("currentIndex").toInt(), 199);

        QTest::keyClick(window, Qt::Key_Tab);
        auto *field = window->findChild<QQuickItem *>("field");
        QCOMPARE(window->activeFocusItem(), field);
        for (const char key : QByteArray("temperature"))
            QTest::keyClick(window, key);
        QCOMPARE(field->property("text").toString(), QString("temperature"));
        QTest::keyClick(window, Qt::Key_Tab);
        auto *slider = window->findChild<QQuickItem *>("slider");
        QCOMPARE(window->activeFocusItem(), slider);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(slider->property("value").toInt(), 51);

        for (const auto *name : {"check", "toggle"}) {
            QTest::keyClick(window, Qt::Key_Tab);
            auto *control = window->findChild<QQuickItem *>(name);
            QCOMPARE(window->activeFocusItem(), control);
            QTest::keyClick(window, Qt::Key_Space);
            QVERIFY(control->property("checked").toBool());
        }
        QCOMPARE(warnings.size(), 0);
    }

    void controlsShowFocusOnlyForKeyboard() {
        QQmlApplicationEngine engine;
        engine.loadData(R"(
            import QtQuick
            import QtQuick.Layouts
            import "qrc:/qml/controls"
            Window {
                width: 600; height: 500
                ColumnLayout {
                    anchors.fill: parent
                    NavigationButton { objectName: "nav"; text: "Overview" }
                    ActionButton { objectName: "action"; text: "Apply" }
                    QuietButton { objectName: "quiet"; text: "Help" }
                    InputField { objectName: "field"; text: "Search" }
                    ChevronButton { objectName: "chevron" }
                    SettingsSection { title: "Example" }
                    ModuleSection { title: "Notifications"; available: false; unavailableReason: "Installation required" }
                }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        for (const auto *name : {"nav", "action", "quiet", "field", "chevron", "ExampleSectionToggle",
                                 "submoduleExpandButton", "moduleRestrictionHelp"}) {
            auto *control = window->findChild<QQuickItem *>(name);
            QVERIFY(control);
            auto *background = qvariant_cast<QQuickItem *>(control->property("background"));
            QVERIFY(background);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              control->mapToScene(QPointF(control->width() / 2, control->height() / 2)).toPoint());
            QVERIFY(control->hasActiveFocus());
            QVERIFY(!control->property("visualFocus").toBool());
            QVERIFY(QQmlProperty::read(background, "border.color").value<QColor>() != QColor("#66caff"));
            QTest::keyClick(window, Qt::Key_Tab);
            QVERIFY(window->activeFocusItem() != control);
            QTest::keyClick(window, Qt::Key_Backtab);
            QCOMPARE(window->activeFocusItem(), control);
            QVERIFY2(control->property("visualFocus").toBool(), name);
            QCOMPARE(QQmlProperty::read(background, "border.color").value<QColor>(), QColor("#66caff"));
        }
    }

    void sensorBrowserLoadingState() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.setBrowserVisible(false);
        local.browserRefreshTimer.stop();
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        local.browserModel.replace({});
        local.browserPending = true;
        local.browserResultsReady = false;
        // Hold the real preparation job so the loading state is deterministic.
        auto gate = std::make_shared<QSemaphore>();
        const auto release = qScopeGuard([gate] { gate->release(); });
        local.browserJob.post([gate] { gate->acquire(); });
        local.setBrowserVisible(true);
        QVERIFY(local.browserLoading());
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)}});
        engine.loadData(R"(
            import QtQuick
            import "qrc:/qml/pages"
            Window {
                id: root
                required property var controller
                width: 1250; height: 820
                SensorsPage { anchors.fill: parent; controller: root.controller }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *loading = window->findChild<QQuickItem *>("sensorLoading");
        auto *spinner = window->findChild<QQuickItem *>("sensorLoadingSpinner");
        auto *empty = window->findChild<QQuickItem *>("sensorEmptyMessage");
        QVERIFY(loading && spinner && empty);
        QTRY_VERIFY(loading->isVisible());
        QVERIFY(spinner->property("running").toBool());
        QVERIFY(!empty->isVisible());
        if (!qEnvironmentVariableIsEmpty("WHAT_SENSOR_LOADING_PREVIEW"))
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_SENSOR_LOADING_PREVIEW")));
        gate->release();
        // A frame can be rejected when the initial filters settle. Let the normal retry run.
        local.browserRefreshTimer.start();
        QTRY_VERIFY(!local.browserLoading());
        QTRY_VERIFY(local.browserModel.rowCount() > 0);
        QVERIFY(!loading->isVisible());

        local.browserJob.post([gate] { gate->acquire(); });
        local.refreshBrowserAsync();
        QVERIFY(local.browserJob.isRunning());
        QVERIFY(!local.browserLoading());
        QVERIFY(!loading->isVisible()); // Keep existing readings visible during refresh.
        gate->release();
        QTRY_VERIFY(!local.browserLoading());
        auto *search = window->findChild<QQuickItem *>("sensorSearch");
        QVERIFY(search);
        auto *clearSearch = window->findChild<QQuickItem *>("clearSensorSearch");
        QVERIFY(clearSearch);
        QVERIFY(!clearSearch->isVisible());
        search->setProperty("text", "termperature");
        QTRY_COMPARE(local.browserFilters()["query"].toString(), QString("termperature"));
        QTRY_COMPARE(local.browserModel.rowCount(), 0);
        QTRY_VERIFY(!local.browserLoading());
        QTRY_VERIFY(empty->isVisible());
        QTRY_VERIFY(!loading->isVisible());
        QTRY_VERIFY(!spinner->property("running").toBool());

        // Polling continues after an empty search has completed. A slow refresh
        // must not replace the no-matches message with an endless loading state.
        local.browserRefreshTimer.stop();
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        local.browserJob.post([gate] { gate->acquire(); });
        emit local.sensorsChanged();
        local.refreshBrowserAsync();
        QVERIFY(local.browserJob.isRunning());
        QVERIFY(!local.browserLoading());
        QVERIFY(empty->isVisible());
        QVERIFY(!loading->isVisible());
        QVERIFY(!spinner->property("running").toBool());

        auto searchGate = std::make_shared<QSemaphore>();
        const auto releaseSearch = qScopeGuard([searchGate] { searchGate->release(); });
        local.browserSearchJob.post([searchGate] { searchGate->acquire(); });
        // Editing and then clearing the query while that refresh is in flight
        // must keep the latest search pending until its own result arrives.
        search->setProperty("text", "temperature");
        QTRY_COMPARE(local.browserFilters()["query"].toString(), QString("temperature"));
        QVERIFY(local.browserLoading());
        QVERIFY(loading->isVisible());
        QVERIFY(clearSearch->isVisible());
        QVERIFY(clearSearch->x() + clearSearch->width() <= search->width());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          clearSearch->mapToScene(QPointF(14, 14)).toPoint());
        QCOMPARE(search->property("text").toString(), QString());
        QVERIFY(!clearSearch->isVisible());
        QVERIFY(search->hasActiveFocus());
        QTRY_COMPARE(local.browserFilters()["query"].toString(), QString());
        QVERIFY(local.browserLoading());
        gate->release();
        searchGate->release();
        local.browserRefreshTimer.start();
        QTRY_VERIFY(!local.browserLoading());
        QTRY_VERIFY(local.browserModel.rowCount() > 0);
        QVERIFY(!loading->isVisible());
        QVERIFY(!empty->isVisible());
    }

    void sensorSearchWithLargeCatalogue() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        UpdateManager updater(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.icueTimer.stop();
        local.setModuleEnabled("windows", false);
        local.setBrowserVisible(false);
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        local.settings.setValue("modules/icue/enabled", true);
        QJsonArray devices;
        for (int n = 0; n < 6; ++n)
            devices.append(
                QJsonObject{{"id", QString("fixture-%1").arg(n)},
                            {"model", QString("Lighting device %1").arg(n)},
                            {"properties", QJsonArray{QJsonObject{{"id", 2}, {"writable", true}},
                                                      QJsonObject{{"id", 5}, {"writable", true}}}}});
        local.icueData = {{"connected", true}, {"devices", devices}};
        local.icueClock.start();
        QJsonArray readings;
        for (int n = 0; n < 3000; ++n) {
            const QString id = QString("search_%1").arg(n);
            readings.append(QJsonObject{{"id", id},
                                        {"source", "hwinfo"},
                                        {"group", QString("Device %1").arg(n / 150)},
                                        {"name", QString("Reading %1").arg(n)},
                                        {"kind", "number"},
                                        {"value", n},
                                        {"available", true},
                                        {"unit", n % 3 == 0 ? QString::fromUtf8("°C") : QString("MHz")}});
            local.selected.insert(id);
            local.settings.setValue("publishing/" + id,
                                    QVariantMap{{"scope", 3}, {"high", false}, {"decimals", -1}});
        }
        local.refreshPublishingPolicies();
        local.settings.setValue("selected", local.selected.values());
        local.mergeReadings("hwinfo", readings);
        local.latest.valid = true;
        local.latest.readings = readings;
        local.latest.pollTime = QDateTime::currentSecsSinceEpoch();
        local.initialAge = 0;
        local.pollClock.start();
        local.setBrowserFilters("", "hwinfo", false, false);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 1);
        auto *stack = window->findChild<QObject *>("pageStack");
        QVERIFY(stack);
        QTRY_VERIFY(!stack->property("moving").toBool());
        QTRY_COMPARE(local.browserModel.rowCount(), 3020);
        // Start timing after the populated page has rendered its first frame.
        QSignalSpy rendered(window, &QQuickWindow::frameSwapped);
        window->update();
        QTRY_VERIFY(!rendered.isEmpty());
        auto *search = window->findChild<QQuickItem *>("sensorSearch");
        QVERIFY(search);
        auto *list = window->findChild<QQuickItem *>("sensorList");
        QVERIFY(list);
        auto *content = qvariant_cast<QQuickItem *>(list->property("contentItem"));
        QVERIFY(content);
        // Scroll far enough to recycle rows before replacing the search results.
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 1500), Q_ARG(int, 0)));
        QTest::qWait(100);
        QSet<QRgb> categoryColours;
        // Search must finish even while catalogue preparation cannot make progress.
        local.browserRefreshTimer.stop();
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        auto refreshGate = std::make_shared<QSemaphore>();
        const auto releaseRefresh = qScopeGuard([refreshGate] { refreshGate->release(); });
        local.browserJob.post([refreshGate] { refreshGate->acquire(); });
        local.refreshBrowserAsync();
        QVERIFY(local.browserJob.isRunning());
        QTimer samples;
        connect(&samples, &QTimer::timeout, &local, &Controller::sensorsChanged);
        samples.start(25);
        QElapsedTimer guiClock;
        QTimer heartbeat;
        qint64 longestGap = 0;
        connect(&heartbeat, &QTimer::timeout, this,
                [&] { longestGap = qMax(longestGap, guiClock.restart()); });
        heartbeat.start(1);
        for (bool blocked : {true, false}) {
            if (!blocked) {
                refreshGate->release();
                local.browserRefreshTimer.start();
            }
            for (const auto &query : {QString("temperature"), QString("tempp"), QString()}) {
                const int expected = query.isEmpty() ? 3020 : query == "temperature" ? 1020 : 0;
                if (query == "tempp") {
                    local.setAnimationsEnabled(true);
                    local.setUiTiming("panels", 600);
                    local.setUiTiming("lists", 600);
                    auto *page = window->findChild<QObject *>("sensorsPage");
                    QVERIFY(page);
                    QVERIFY(QMetaObject::invokeMethod(page, "foldGroups", Q_ARG(QVariant, QVariant(true))));
                    QTest::qWait(20);
                    QVERIFY(QMetaObject::invokeMethod(page, "foldGroups", Q_ARG(QVariant, QVariant(false))));
                    QTest::qWait(20);
                }
                longestGap = 0;
                guiClock.start();
                QElapsedTimer completion;
                completion.start();
                search->setProperty("text", query);
                QVERIFY(QTest::qWaitFor(
                    [&] {
                        return local.browserFilters()["query"] == query && !local.browserLoading() &&
                               local.browserModel.rowCount() == expected;
                    },
                    1000));
                const auto modelTime = completion.elapsed();
                rendered.clear();
                window->update();
                QVERIFY(QTest::qWaitFor([&] { return !rendered.isEmpty(); }, 1000));
                // Cached delegates stay alive, but interrupted transitions must
                // never leave them visible after they enter the reuse pool.
                for (auto *item : content->childItems())
                    if (item->property("pooled").toBool())
                        QVERIFY(!item->isVisible());
                if (expected > 0) {
                    for (auto *item : content->childItems())
                        if (item->property("modelData").toMap()["header"].toBool())
                            categoryColours.insert(item->property("color").value<QColor>().rgba());
                    QVERIFY(!categoryColours.isEmpty());
                } else {
                    QTest::qWait(700); // Removed delegates must stay gone after interrupted transitions settle.
                    const auto image = window->grabWindow();
                    if (!qEnvironmentVariableIsEmpty("WHAT_EMPTY_SEARCH_PREVIEW"))
                        QVERIFY(image.save(qEnvironmentVariable("WHAT_EMPTY_SEARCH_PREVIEW")));
                    const auto top = list->mapToScene(QPointF(10, 0)) * window->devicePixelRatio();
                    const int bottom = qMin(image.height(), qRound(top.y() + list->height() * window->devicePixelRatio()));
                    int leftoverRows = 0;
                    for (int y = qMax(0, qRound(top.y())); y < bottom; ++y)
                        leftoverRows += categoryColours.contains(image.pixel(qRound(top.x()), y));
                    QCOMPARE(leftoverRows, 0);
                }
                longestGap = qMax(longestGap, guiClock.elapsed());
                qInfo() << (blocked ? "Blocked refresh search" : "Live refresh search") << query
                        << "model ready in" << modelTime << "ms; displayed in" << completion.elapsed()
                        << "ms; longest GUI gap" << longestGap << "ms";
            }
        }
        samples.stop();
        heartbeat.stop();
        refreshGate->release();
        // Cancel any queued delegate creation before destroying the fixture's engine.
        QVERIFY(list->setProperty("model", QVariant()));
        window->hide();
    }

    void indexedBrowserSearchPreservesFiltersAndHierarchy() {
        const QVariantList rows{QVariantMap{{"id", "cpu"},
                                            {"source", "hwinfo"},
                                            {"group", "CPU"},
                                            {"name", "CPU package"},
                                            {"unit", QString::fromUtf8("°C")},
                                            {"enabled", true},
                                            {"favourite", true},
                                            {"shareScope", 3}},
                                QVariantMap{{"id", "gpu"},
                                            {"source", "hwinfo"},
                                            {"group", "GPU"},
                                            {"name", "GPU utilisation"},
                                            {"unit", "%"},
                                            {"enabled", false}},
                                QVariantMap{{"id", "rgb"},
                                            {"source", "icue"},
                                            {"group", "Device"},
                                            {"categoryKey", "icue_device"},
                                            {"categoryName", "Device"},
                                            {"name", "Device temperature"}},
                                QVariantMap{{"id", "led"},
                                            {"source", "icue"},
                                            {"group", "Device / LEDs"},
                                            {"categoryKey", "icue_device/leds"},
                                            {"parentCategory", "icue_device"},
                                            {"categoryName", "Individual LEDs"},
                                            {"name", "LED 2"},
                                            {"control", "rgb"},
                                            {"available", false},
                                            {"controlOnly", true},
                                            {"favourite", true}}};
        const auto indexed = Controller::indexBrowserRows(rows);
        // Capability is independent of availability and includes hidden children.
        // Filtering out the only control must remove the category's writable label.
        for (const auto *query : {"", "LED 2", "Device temperature"}) {
            const auto result = Controller::formatBrowser(
                rows, {{"query", query}, {"source", "icue"}}, {"icue_device"});
            QCOMPARE(result.size(), 1);
            QVERIFY(result.first().toMap()["header"].toBool());
            QCOMPARE(result.first().toMap()["writable"].toBool(),
                     QString(query) != "Device temperature");
        }
        for (const auto query : {"", "temperature", "LED 2", "Device temperature", "missing", "load", "CPU   package"})
            for (const auto source : {"all", "hwinfo", "icue", "none", "hwinfo,icue"})
                for (int sharing = 0; sharing < 3; ++sharing)
                    for (bool favourite : {false, true}) {
                        const QVariantMap filters{{"query", query},
                                                  {"source", source},
                                                  {"selected", sharing == 1},
                                                  {"unshared", sharing == 2},
                                                  {"favourites", favourite}};
                        const auto groups = Controller::filterBrowserIndex(indexed, filters);
                        for (const auto &closed : {QSet<QString>{}, QSet<QString>{"icue_device"}}) {
                            const auto visible = BrowserModel::visibleCategoryRows(groups, closed);
                            QCOMPARE(BrowserModel::categorySnapshot(groups, visible, closed),
                                     Controller::formatBrowser(rows, filters, closed));
                        }
                    }
    }

    void browserSearchCacheRespectsPolicyChanges() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.stopService();
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.browserRefreshTimer.stop();
        local.setBrowserVisible(false);
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        auto reading = row("search_policy", 42);
        reading["source"] = "hwinfo";
        local.mergeReadings("hwinfo", {reading});
        local.setSensorPublishing("search_policy", 1, false, -1);
        local.setBrowserFilters("", "hwinfo", false, false);
        local.setBrowserVisible(true);
        QTRY_VERIFY(!local.browserLoading());
        QVERIFY(local.browserInputCache);

        // Change policy without changing selection or catalogue metadata. A new
        // search must not reuse the old sharing scope from the cached input.
        local.setBrowserVisible(false);
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        local.setSensorPublishing("search_policy", 2, true, -1);
        local.setBrowserFilters("Fixture", "hwinfo", false, false);
        local.setBrowserVisible(true);
        QTRY_VERIFY(!local.browserLoading());
        QCOMPARE(local.browserModel.rowCount(), 2);
        const auto shown = local.browserModel.snapshot().last().toMap();
        QCOMPARE(shown.value("shareScope").toInt(), 2);
        QVERIFY(shown.value("highFrequency").toBool());

        local.setBrowserVisible(false);
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        local.setModuleEnabled("hwinfo", false);
        local.setBrowserFilters("", "hwinfo", false, false);
        local.setBrowserVisible(true);
        QTRY_VERIFY(!local.browserLoading());
        QCOMPARE(local.browserModel.rowCount(), 0);
    }

    void sensorRowsReuseCurrentValuesAndPermissions() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.setBrowserVisible(false);
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        QVariantList rows;
        for (int n = 0; n < 300; ++n)
            rows.append(QVariantMap{{"id", QString("reuse_%1").arg(n)},
                                    {"name", QString("Sensor %1").arg(n)},
                                    {"header", false},
                                    {"kind", "number"},
                                    {"unit", "MHz"},
                                    {"value", n},
                                    {"available", n % 2 == 0},
                                    {"shareScope", n % 4},
                                    {"highFrequency", false},
                                    {"favourite", false}});
        local.browserModel.replace(rows);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)}});
        engine.loadData(R"(
            import QtQuick
            import "qrc:/qml/pages"
            Window {
                id: root
                required property var controller
                width: 1250; height: 820
                SensorsPage { anchors.fill: parent; controller: root.controller }
            }
        )");
        QVERIFY2(!engine.rootObjects().isEmpty(), "Sensor page did not load");
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *list = window->findChild<QQuickItem *>("sensorList");
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), 300);
        auto *content = list->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        QSet<QQuickItem *> original;
        for (auto *item : content->childItems())
            if (item->property("modelData").isValid())
                original.insert(item);
        QVERIFY(!original.isEmpty());
        std::function<QQuickItem *(QQuickItem *, const QString &)> find =
            [&](QQuickItem *item, const QString &name) -> QQuickItem * {
            if (item->objectName() == name)
                return item;
            for (auto *child : item->childItems())
                if (auto *found = find(child, name))
                    return found;
            return nullptr;
        };
        // Several moves allow off-screen delegates to enter and leave the reuse pool.
        for (int row : {50, 100, 150}) {
            QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, row), Q_ARG(int, 0)));
            QTest::qWait(100);
        }
        int recycled = 0;
        for (auto *item : content->childItems()) {
            if (!item->property("modelData").isValid() || item->property("pooled").toBool())
                continue;
            const int n = item->property("index").toInt();
            if (n < 140)
                continue;
            recycled += original.contains(item);
            QCOMPARE(item->property("readingValue").toInt(), n);
            QCOMPARE(item->property("readingAvailable").toBool(), n % 2 == 0);
            auto *localSharing = find(item, "sharing_1");
            auto *cloudSharing = find(item, "sharing_2");
            QVERIFY(localSharing && cloudSharing);
            QCOMPARE(localSharing->property("checked").toBool(), bool(n & 1));
            QCOMPARE(cloudSharing->property("checked").toBool(), bool(n & 2));
            QVERIFY(!find(item, "icueDeviceColour"));
        }
        QVERIFY(recycled > 0);
        auto *bar = window->findChild<QQuickItem *>("sensorScrollBar");
        QVERIFY(bar);
        QVERIFY(bar->mapToScene(QPointF()).x() >= list->mapToScene(QPointF(list->width(), 0)).x());
        list->setProperty("contentY", 0);
        QCoreApplication::processEvents();
        const auto start = list->mapToScene(QPointF(140, list->height() / 2)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        for (int step = 1; step <= 6; ++step)
            QTest::mouseMove(window, start - QPoint(0, step * 25), 16);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start - QPoint(0, 150));
        QTRY_VERIFY(list->property("contentY").toReal() > 80);
        QVERIFY(window->findChild<QObject *>("sensorsPage")->property("expandedId").toString().isEmpty());
        window->hide();
    }

    void diagnosticScrolling() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.stopService();
        local.setAnimationsEnabled(false);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 3);
        QTest::qWait(100);
        auto *live = window->findChild<QObject *>("liveDiagnosticsView");
        auto *log = window->findChild<QObject *>("activityLogView");
        QVERIFY(live);
        QVERIFY(log);
        auto *activitySection = window->findChild<QObject *>("activityLogSection");
        auto *liveSection = window->findChild<QObject *>("liveDiagnosticsSection");
        QVERIFY(activitySection && liveSection);
        QVERIFY(activitySection->property("expanded").toBool());
        QVERIFY(!live->property("updatesEnabled").toBool());
        const QString lines = QString("A diagnostic line\n").repeated(200);
        // Replace the controller bindings, not just their current values. Otherwise
        // a queued controller notification can overwrite the fixture mid-assertion.
        QVERIFY(QQmlProperty::write(live, "sourceText", lines));
        QVERIFY(QQmlProperty::write(log, "sourceText", lines));
        auto bottom = [](QObject *view) {
            return qMax(0.0,
                        view->property("contentHeight").toDouble() - view->property("height").toDouble());
        };
        // Opening one reading section gives it the available space. Closed text
        // views retain their snapshot without laying out incoming text.
        QVERIFY(live->property("displayedText").toString().isEmpty());
        liveSection->setProperty("expanded", true);
        QVERIFY(!activitySection->property("expanded").toBool());
        QTRY_VERIFY(bottom(live) > 300);
        live->setProperty("contentY", 300);
        live->setProperty("sourceText", lines + "New diagnostic\n");
        QTRY_COMPARE(live->property("contentY").toDouble(), 300.0);
        activitySection->setProperty("expanded", true);
        QVERIFY(!liveSection->property("expanded").toBool());
        const auto closedSnapshot = live->property("displayedText").toString();
        live->setProperty("sourceText", lines + "Closed arrival\n");
        QCOMPARE(live->property("displayedText").toString(), closedSnapshot);
        QTRY_VERIFY(bottom(log) > 300);
        QTRY_VERIFY(qAbs(log->property("contentY").toDouble() - bottom(log)) < 2);
        log->setProperty("contentY", 300);
        log->setProperty("sourceText", lines + "New log entry\n");
        // Controller notifications must not replace this test's synthetic text.
        emit local.changed();
        emit local.logChanged();
        QCOMPARE(live->property("sourceText").toString(), lines + "Closed arrival\n");
        QCOMPARE(log->property("sourceText").toString(), lines + "New log entry\n");
        QTest::qWait(100);
        QCOMPARE(live->property("contentY").toDouble(), 300.0);
        QCOMPARE(log->property("contentY").toDouble(), 300.0);
        log->setProperty("contentY", bottom(log));
        QVERIFY(log->property("following").toBool());
        log->setProperty("sourceText", lines + QString("More log entries\n").repeated(30));
        QTRY_VERIFY(qAbs(log->property("contentY").toDouble() - bottom(log)) < 2);
        log->setProperty("paused", true);
        const auto frozen = log->property("displayedText").toString();
        log->setProperty("sourceText", lines + "Paused arrival\n");
        QCOMPARE(log->property("displayedText").toString(), frozen);
        QVERIFY2(writeClipboardWithRetry([&] { local.copyLogSnapshot(frozen); }),
                 "Windows clipboard remained unavailable for log copying");
        QCOMPARE(QApplication::clipboard()->text(), frozen);
        log->setProperty("paused", false);
        QTRY_COMPARE(log->property("displayedText").toString(), lines + "Paused arrival\n");
        auto *text = window->findChild<QObject *>("activityLogViewText");
        QVERIFY(text);
        QVERIFY(QMetaObject::invokeMethod(text, "select", Q_ARG(int, 0), Q_ARG(int, 10)));
        const auto selected = text->property("selectedText").toString();
        QVERIFY(!selected.isEmpty());
        log->setProperty("sourceText", lines + "Selection arrival\n");
        QCOMPARE(text->property("selectedText").toString(), selected);
        QCOMPARE(log->property("displayedText").toString(), lines + "Paused arrival\n");
        QVERIFY(QMetaObject::invokeMethod(text, "deselect"));
        QTRY_COMPARE(log->property("displayedText").toString(), lines + "Selection arrival\n");
        local.beacnCommand({{"control", "volume_1"}, {"value", 12.345}});
        QVERIFY(local.logs().contains("ERROR BEACN [WHAT] volume_1: failed"));
        local.handleBeacnCommand("test-session", {{"args", QJsonObject{{"control", "mute_1"}}}});
        QVERIFY(local.logs().contains("ERROR BEACN [Home Assistant] mute_1: failed"));
        local.prepareDiagnosticReport();
        QTRY_VERIFY_WITH_TIMEOUT(!local.diagnosticReportBusy(), 5000);
        QVERIFY(local.diagnosticReport().contains("ERROR BEACN [WHAT] volume_1: failed"));
        QVERIFY(!local.diagnosticReport().contains("12.345"));
        const auto ordinaryHeight = log->property("height").toDouble();
        window->resize(window->width(), window->property("availableWindowHeight").toInt());
        QTest::qWait(100);
        QVERIFY(log->property("height").toDouble() >= ordinaryHeight);
        window->resize(window->minimumWidth(), window->minimumHeight());
        QTest::qWait(100);
        QVERIFY(log->property("height").toDouble() >= 150);
        const auto directory = qEnvironmentVariable("WHAT_SPECS_PREVIEW");
        if (!directory.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(window->grabWindow().save(directory + "/diagnostics.png"));
            auto *logging = window->findChild<QObject *>("diagnosticLoggingSection");
            QVERIFY(logging);
            logging->setProperty("expanded", true);
            QTest::qWait(100);
            QVERIFY(window->grabWindow().save(directory + "/diagnostic-settings.png"));
        }
        window->hide();
    }

    void remoteMissingEndpointDiagnostic() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::renew(dir.path(), true);
        auto original = QSslConfiguration::defaultConfiguration(), trust = original;
        trust.addCaCertificate(identity.certificate);
        QSslConfiguration::setDefaultConfiguration(trust);
        auto restore = qScopeGuard([&] { QSslConfiguration::setDefaultConfiguration(original); });
        auto ssl = original;
        ssl.setLocalCertificate(identity.certificate);
        ssl.setPrivateKey(identity.key);
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);

        struct HttpEndpoint : QTcpServer {
            QSslConfiguration ssl;

            void incomingConnection(qintptr descriptor) override {
                auto *socket = new QSslSocket(this);
                socket->setSocketDescriptor(descriptor);
                socket->setSslConfiguration(ssl);
                connect(socket, &QSslSocket::readyRead, socket, [socket] {
                    if (!socket->readAll().isEmpty()) {
                        socket->write(
                            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                    }
                });
                connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
                socket->startServerEncryption();
            }
        } endpoint;

        endpoint.ssl = ssl;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost, 0));
        NetworkService service;
        service.configure("remote-fixture", QString(64, 'a'), ssl);
        QVERIFY(service.listen(QHostAddress::LocalHost, 0));
        QSignalSpy diagnostics(&service, &NetworkService::diagnostic);
        service.setRemote(QString("wss://localhost:%1/api/what/connect").arg(endpoint.serverPort()));
        auto message = [&] {
            for (const auto &entry : diagnostics)
                if (entry[0].toString().contains("HTTP 404"))
                    return entry[0].toString();
            return QString();
        };
        QTRY_VERIFY_WITH_TIMEOUT(!message().isEmpty(), 6000);
        QVERIFY(message().contains("restart Home Assistant"));
        QVERIFY(!message().contains("localhost"));
        service.close();
    }

    void outboundRemoteTlsAndExpiry() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::renew(dir.path(), true);
        auto original = QSslConfiguration::defaultConfiguration();
        auto trust = original;
        trust.addCaCertificate(identity.certificate);
        QSslConfiguration::setDefaultConfiguration(trust);
        auto restore = qScopeGuard([&] { QSslConfiguration::setDefaultConfiguration(original); });
        auto ssl = original;
        ssl.setLocalCertificate(identity.certificate);
        ssl.setPrivateKey(identity.key);
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        QWebSocketServer endpoint("fixture", QWebSocketServer::SecureMode);
        endpoint.setSslConfiguration(ssl);
        QVERIFY(endpoint.listen(QHostAddress::LocalHost, 0));
        const QString secret(64, 'a');
        NetworkService service;
        service.configure("remote-fixture", secret, ssl);
        QVERIFY(service.listen(QHostAddress::LocalHost, 0));
        service.setSnapshot({{"type", "snapshot"},
                             {"protocol", 2},
                             {"device_id", "remote-fixture"},
                             {"sensors", QJsonArray{}},
                             {"sample_age", 0},
                             {"publish_seconds", 1}});
        QWebSocket *peer = nullptr;
        QJsonArray received;
        connect(&endpoint, &QWebSocketServer::newConnection, this, [&] {
            peer = endpoint.nextPendingConnection();
            connect(peer, &QWebSocket::textMessageReceived, this, [&](QString text) {
                auto obj = QJsonDocument::fromJson(text.toUtf8()).object();
                received.append(obj);
                if (obj["type"] == "remote_auth") {
                    QCOMPARE(obj["token"].toString(), secret);
                    peer->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "auth"},
                                                                                      {"protocol", 2},
                                                                                      {"token", secret},
                                                                                      {"commands_v1", true},
                                                                                      {"delta_v1", true}})
                                                                .toJson()));
                }
            });
        });
        QSignalSpy commands(&service, &NetworkService::commandRequested);
        QSignalSpy transports(&service, &NetworkService::transportState);
        QSignalSpy progress(&service, &NetworkService::connectionProgress);
        service.setRemote(QString("wss://localhost:%1/api/what/connect").arg(endpoint.serverPort()));
        auto has = [&](QString type) {
            for (auto v : received)
                if (v.toObject()["type"] == type)
                    return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(has("hello"), 6000);
        QTRY_VERIFY_WITH_TIMEOUT(has("snapshot"), 6000);
        QTRY_VERIFY(!transports.isEmpty());
        bool attempted = false, authenticated = false;
        for (const auto &event : progress) {
            attempted |= event[0] == "remote" && event[1] == "connecting";
            authenticated |= event[0] == "remote" && event[1] == "authenticating";
        }
        QVERIFY(attempted);
        QVERIFY(authenticated);
        QCOMPARE(transports.last()[0].toString(), QString("cloud"));
        QCOMPARE(transports.last()[1].toString(), peer->sslConfiguration().sessionProtocol() == QSsl::TlsV1_3
                                                      ? QString("TLS 1.3 encrypted")
                                                      : QString("TLS 1.2 encrypted"));
        auto send = [&](QString id, qint64 expires) {
            peer->sendTextMessage(
                QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "command"},
                                                            {"id", id},
                                                            {"action", "keyboard"},
                                                            {"args", QJsonObject{{"keys", QJsonArray{"A"}}}},
                                                            {"expires_at", double(expires)}})
                                      .toJson()));
        };
        send("expired", QDateTime::currentMSecsSinceEpoch() - 1);
        QTRY_VERIFY(has("command_result"));
        QCOMPARE(commands.size(), 0);
        send("valid", QDateTime::currentMSecsSinceEpoch() + 10000);
        QTRY_COMPARE(commands.size(), 1);
        send("valid", QDateTime::currentMSecsSinceEpoch() + 10000);
        QTest::qWait(100);
        QCOMPARE(commands.size(), 1);
        auto lease = service.commandLease(commands[0][0].toString());
        QVERIFY(lease && lease->load());
        service.setRemote({});
        QTRY_VERIFY(!lease->load());
        service.close();
        if (peer)
            peer->deleteLater();
    }

    void remoteInputValidation() {
        QVERIFY(
            WindowsCommands::validate("keyboard", {{"keys", QJsonArray{"CTRL", "SHIFT", "S"}}}).isEmpty());
        QVERIFY(WindowsCommands::validate("keyboard", {{"text", QString::fromUtf8("Hello \xF0\x9F\x98\x80")}})
                    .isEmpty());
        QVERIFY(!WindowsCommands::validate("keyboard", {{"keys", QJsonArray{"NO_SUCH_KEY"}}}).isEmpty());
        QVERIFY(!WindowsCommands::validate("keyboard", {{"keys", QJsonArray{"CTRL", "CTRL"}}}).isEmpty());
        QVERIFY(
            !WindowsCommands::validate("keyboard", {{"text", "test"}, {"keys", QJsonArray{"A"}}}).isEmpty());
        QVERIFY(!WindowsCommands::validate("keyboard", {{"text", QString(1025, 'x')}}).isEmpty());
        QVERIFY(
            WindowsCommands::validate("mouse", {{"x", -100}, {"y", 500}, {"button", "left"}, {"clicks", 2}})
                .isEmpty());
        QVERIFY(!WindowsCommands::validate("mouse", {{"x", 1.5}, {"y", 0}}).isEmpty());
        QVERIFY(!WindowsCommands::validate("mouse", {{"x", 0}, {"y", 0}, {"button", "hold"}}).isEmpty());
        QVERIFY(!WindowsCommands::validate("mouse", {{"x", 0}, {"y", 0}, {"clicks", 2}}).isEmpty());
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        QVERIFY(!local.controlState()["input_enabled"].toBool());
        local.setControlPermission("input", true);
        QVERIFY(local.controlState()["input_enabled"].toBool());
        QVERIFY(!local.controlState()["permissions"].toObject().contains("input"));
        local.setModuleEnabled("windows", false);
        QVERIFY(!local.controlState()["input_enabled"].toBool());
    }

    void browserDefersRefreshWhileScrolling() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.icueTimer.stop();
        local.browserModel.replace({});
        local.setBrowserInteracting(true);
        emit local.sensorsChanged();
        QCOMPARE(local.browserModel.rowCount(), 0);
        QVERIFY(local.browserPending);
        local.setBrowserInteracting(false);
        QVERIFY(!local.browserPending);
        QTRY_COMPARE(local.browserModel.rowCount(), local.browser().size());
    }

    void specificationsInputDevices() {
        QCOMPARE(inputDeviceName("HID Keyboard Device", "HID Keyboard Device", "CORSAIR K95"), QString("CORSAIR K95"));
        QCOMPARE(inputDeviceName("ELECOM Trackball Mouse", "HID-compliant mouse", "USB Receiver"), QString("ELECOM Trackball Mouse"));
        QVERIFY(inputDeviceName("HID-compliant mouse", "HID-compliant mouse", "USB Receiver").isEmpty());
        QVERIFY(inputDeviceName("", "", "").isEmpty());
        QVERIFY(inputDeviceName("HID Keyboard Device", "HID Keyboard Device", "Corsair virtual input device").isEmpty());
        const auto report = inputSpecifications();
        // CI machines may expose only generic or virtual input interfaces.
        QVERIFY(report.isEmpty() || report.startsWith("Input devices\n"));
        QVERIFY(!report.contains(": USB Receiver"));
        QVERIFY(!report.contains("One device can expose"));
        // Device instance paths contain serials and must stay out of copied reports.
        QVERIFY(!report.contains("HID\\"));
        QVERIFY(!report.contains("USB\\"));
        qInfo().noquote() << report;
    }

    void specificationsInventoryScript() {
        QFile fixture(":/tests/specs_inventory_fixture.ps1");
        QFile inventory(":/src/modules/windows/specs/SpecsInventory.ps1");
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QVERIFY(inventory.open(QIODevice::ReadOnly));
        const QString mocks = QString::fromUtf8(fixture.readAll());
        const QString script = QString::fromUtf8(inventory.readAll());
        const QString executable = qEnvironmentVariable("SystemRoot", "C:/Windows") +
                                   "/System32/WindowsPowerShell/v1.0/powershell.exe";
        for (const int scenario : {0, 1, 2}) {
            const bool denied = scenario == 1;
            QProcess process;
            const auto mode = QString("$script:unavailable = $%1\n$script:desktop = $%2\n")
                                  .arg(denied ? "true" : "false", scenario == 2 ? "true" : "false");
            process.start(executable,
                          {"-NoLogo", "-NoProfile", "-NonInteractive", "-Command", mocks + mode + script});
            QVERIFY(process.waitForFinished(15000));
            const auto error = process.readAllStandardError();
            QVERIFY2(error.isEmpty(), error.constData());
            QCOMPARE(process.exitCode(), 0);
            const auto report = QString::fromUtf8(process.readAllStandardOutput());
            QVERIFY(!report.contains("PRIVATE-IDENTIFIER-DO-NOT-PRINT"));
            if (denied) {
                QVERIFY(report.contains("System: unavailable"));
                QVERIFY(report.contains("Battery details unavailable"));
                QVERIFY(!report.contains("No physical network adapters"));
                continue;
            }
            QVERIFY(report.contains("2 of 4 slots populated, 2 available"));
            QVERIFY(report.contains("DIMM A2"));
            QVERIFY(report.contains("DDR5 | 6800 MT/s configured"));
            QVERIFY(report.contains("SSD | NVMe | firmware FW1"));
            QVERIFY(report.contains("feature version 24H2 | build 26100.1234"));
            QVERIFY(report.contains("2.5 Gbit/s"));
            QVERIFY(report.contains("Test Wi-Fi | Wi-Fi | Disconnected"));
            QVERIFY(!report.contains("99 Gbit/s"));
            if (scenario == 2) {
                QVERIFY(!report.contains("Batteries"));
                QVERIFY(!report.contains("Battery details unavailable"));
            } else {
                QVERIFY(report.contains("full charge"));
                QVERIFY(report.contains("mWh"));
            }
            QVERIFY(report.contains("Some Windows queries were unavailable: Win32_BaseBoard"));
        }
    }

    void specificationsStatus() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.setAnimationsEnabled(false);
        local.specsLoaded = true;
        local.specsText = "Cached specifications";
        local.specsRefreshTime = "03 Oct 2026, 12:00";
        local.specsBusy = true;
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 4);
        auto *refresh = window->findChild<QObject *>("refreshSpecsButton");
        auto *progress = window->findChild<QObject *>("specsProgress");
        auto *status = window->findChild<QObject *>("specsRefreshStatus");
        auto *notice = window->findChild<QObject *>("specsCollectionNotice");
        QVERIFY(refresh && progress && status && notice);
        QTRY_VERIFY(progress->property("running").toBool());
        QVERIFY(!refresh->property("enabled").toBool());
        QCOMPARE(local.specs(), QString("Cached specifications"));
        local.specsBusy = false;
        local.specsCollectionNotice = "Some details are unavailable.";
        local.specsCollectionIssues = specificationIssues(
            "Monitors\nFixture Display | Enabled\n  HDR support/state: unavailable (Windows error 50)\n"
            "\nMemory modules\nDIMM A2 | speed unavailable\n");
        emit local.specsChanged();
        QTRY_VERIFY(!progress->property("running").toBool());
        QVERIFY(refresh->property("enabled").toBool());
        QVERIFY(status->property("text").toString().contains(local.specsRefreshTime));
        QVERIFY(notice->property("visible").toBool());
        notice->setProperty("expanded", true);
        auto *issueDetails = window->findChild<QObject *>("specsIssueDetails");
        QVERIFY(issueDetails);
        QVERIFY(issueDetails->property("text").toString().contains("Fixture Display: HDR support/state"));
        QVERIFY(issueDetails->property("text").toString().contains("Windows error 50"));
        QVERIFY(issueDetails->property("text").toString().contains("DIMM A2"));
        if (!qEnvironmentVariableIsEmpty("WHAT_SPECS_PREVIEW")) {
            QTest::qWait(100);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_SPECS_PREVIEW") + "/specs-expanded.png"));
        }
        QCOMPARE(warnings.count(), 0);
        window->hide();
    }

    void specificationsKeepReportOnFailure() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.specsLoaded = true;
        local.specsText = "Previous complete report";
        local.specsRefreshTime = "Earlier collection";
        const auto originalRoot = qgetenv("SystemRoot");
        {
            // Only the collector's executable lookup uses this temporary path.
            // Restore it before processing events or running any other jobs.
            auto restore = qScopeGuard([&] { qputenv("SystemRoot", originalRoot); });
            qputenv("SystemRoot", dir.path().toUtf8());
            local.refreshSpecs();
        }
        QTRY_VERIFY_WITH_TIMEOUT(!local.specsRefreshing(), 3000);
        QCOMPARE(local.specs(), QString("Previous complete report"));
        QCOMPARE(local.specsLastRefreshed(), QString("Earlier collection"));
        QVERIFY(local.specsNotice().contains("previous report"));
    }

    void specificationsHardwareSmoke() {
        if (!qEnvironmentVariableIsSet("WHAT_SPECS_HARDWARE_TEST"))
            QSKIP("Set WHAT_SPECS_HARDWARE_TEST to inspect local hardware collection.");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.setAnimationsEnabled(false);
        local.refreshSpecs();
        QVERIFY(local.specsRefreshing());
        QTRY_VERIFY_WITH_TIMEOUT(!local.specsRefreshing(), 35000);
        QVERIFY(!local.specsLastRefreshed().isEmpty());
        QVERIFY(local.specs().contains("Graphics"));
        QVERIFY(local.specs().contains("Monitors"));
        QVERIFY(local.specs().contains("Boot mode:"));
        QVERIFY(local.specs().contains("TPM:"));
        const auto osStart = local.specs().indexOf("Operating system\n");
        const auto activation = local.specs().indexOf("Windows activation:");
        QVERIFY(osStart >= 0 && activation > osStart);
        const auto sectionEnd = local.specs().indexOf("\n\n", osStart);
        QVERIFY(sectionEnd < 0 || activation < sectionEnd);
        QVERIFY(!local.specs().contains("Specifications have not"));
        if (!qEnvironmentVariableIsEmpty("WHAT_SPECS_PREVIEW")) {
            QFile report(qEnvironmentVariable("WHAT_SPECS_PREVIEW") + "/specs-hardware.txt");
            QVERIFY(report.open(QIODevice::WriteOnly));
            report.write(local.specs().toUtf8());
            report.close();
            UpdateManager updater(dir.path(), true);
            QQmlApplicationEngine engine;
            engine.setInitialProperties(
                {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
            engine.load(QUrl("qrc:/qml/Main.qml"));
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
            QVERIFY(window);
            window->show();
            QVERIFY(QTest::qWaitForWindowExposed(window));
            window->setProperty("previewPage", 4);
            QTest::qWait(100);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_SPECS_PREVIEW") + "/specs.png"));
            window->hide();
        }
    }

    void monitorSpecificationsFormatting() {
        QJsonObject a{{"id", "abcdef123456"},
                      {"name", QString::fromUtf8("Test display · 123456")},
                      {"enabled", true},
                      {"primary", true},
                      {"width", 2560},
                      {"height", 1440},
                      {"refresh", 59.951234},
                      {"rotation", 1},
                      {"connection", "DisplayPort"},
                      {"scalePercent", 150},
                      {"bitsPerChannel", 10},
                      {"hdrSupported", true},
                      {"hdrEnabled", false}};
        auto b = a;
        b["enabled"] = false;
        b["rotation"] = 2;
        b["width"] = 1080;
        b["height"] = 1920;
        const auto report = displaySpecifications({{"monitors", QJsonArray{a, b}}});
        QVERIFY(report.contains(QString::fromUtf8("2560 × 1440")));
        QVERIFY(report.contains("59.95 Hz"));
        QVERIFY(report.contains("Scaling: 150%"));
        QVERIFY(report.contains("Colour depth: 10 bits per channel"));
        QVERIFY(report.contains("HDR: supported, off"));
        QVERIFY(report.contains("display inactive"));
        auto unknown = a;
        unknown.remove("hdrSupported");
        const auto partial = displaySpecifications({{"monitors", QJsonArray{unknown}}});
        QVERIFY(partial.contains("HDR support/state: unavailable"));
        QVERIFY(!partial.contains("HDR: not supported"));
        QVERIFY(report.contains("Enabled | Primary"));
        QVERIFY(report.contains("Disabled | Saved mode:"));
        QVERIFY(!report.contains("Disabled | Primary"));
        QVERIFY(!report.contains("123456"));
        QVERIFY(report.contains(QString::fromUtf8("Rotation 90°")));
        QVERIFY(displaySpecifications({}).contains("No connected monitors"));
    }

    void overviewInstallationEditingAndPairing() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.setAnimationsEnabled(false);
        const auto firstId = local.installations().first().toMap()["id"].toString();
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                     {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *choice = window->findChild<QObject *>("pairingInstallation");
        auto *add = window->findChild<QObject *>("pairingAddInstallation");
        auto *edit = window->findChild<QObject *>("pairingEditInstallation");
        auto *editor = window->findChild<QObject *>("pairingInstallationEditor");
        auto *instructions = window->findChild<QObject *>("pairingInstructions");
        auto *copy = window->findChild<QObject *>("pairingCopyDetails");
        auto *replace = window->findChild<QObject *>("pairingReplaceToken");
        auto *tokenDialog = window->findChild<QObject *>("selectedInstallationTokenDialog");
        QVERIFY(choice && add && edit && editor && instructions && copy && replace && tokenDialog);
        auto *name = editor->findChild<QObject *>("installationName");
        auto *type = editor->findChild<QObject *>("installationType");
        auto *host = editor->findChild<QObject *>("installationMqttHost");
        auto *enabled = editor->findChild<QObject *>("installationEnabled");
        QVERIFY(name && type && host && enabled);
        QTRY_COMPARE(choice->property("currentValue").toString(), firstId);
        QVERIFY(instructions->property("text").toString().contains("Choose WHAT API"));
        auto *toggle = window->findChild<QObject *>("overviewPairingToggle");
        QVERIFY(toggle);
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));

        // Cancel and failed validation must not create an installation or change selection.
        QVERIFY(QMetaObject::invokeMethod(add, "clicked"));
        QTRY_VERIFY(editor->property("opened").toBool());
        QVERIFY(window->property("webModalOpen").toBool());
        QVERIFY(QMetaObject::invokeMethod(editor, "save"));
        QVERIFY(editor->property("visible").toBool());
        QCOMPARE(local.installations().size(), 1);
        QVERIFY(QMetaObject::invokeMethod(editor, "close"));
        QTRY_VERIFY(!window->property("webModalOpen").toBool());
        QCOMPARE(choice->property("currentValue").toString(), firstId);

        QVERIFY(QMetaObject::invokeMethod(add, "clicked"));
        name->setProperty("text", "Second House");
        type->setProperty("currentIndex", 1);
        host->setProperty("text", "localhost");
        enabled->setProperty("checked", false);
        QVERIFY(QMetaObject::invokeMethod(editor, "save"));
        QTRY_VERIFY(!editor->property("visible").toBool());
        QCOMPARE(local.installations().size(), 2);
        const auto secondId = local.installations().last().toMap()["id"].toString();
        QTRY_COMPARE(choice->property("currentValue").toString(), secondId);
        QVERIFY(instructions->property("text").toString().contains("Choose MQTT"));
        const auto credentials = local.settings.value("connections/credentials/" + secondId).toMap();
        const auto token = QString::fromLatin1(unprotectSecret(credentials["token"].toByteArray()));
        QVERIFY(writeClipboardWithRetry([&] { QMetaObject::invokeMethod(copy, "clicked"); }));
        QVERIFY(QApplication::clipboard()->text().contains("Installation: Second House"));
        QVERIFY(QApplication::clipboard()->text().contains("Token: " + token));
        QVERIFY(QApplication::clipboard()->text().contains("Device ID: " + local.deviceId));

        QVERIFY(QMetaObject::invokeMethod(edit, "clicked"));
        QCOMPARE(editor->property("installationId").toString(), secondId);
        name->setProperty("text", "Renamed House");
        type->setProperty("currentIndex", 0);
        QVERIFY(QMetaObject::invokeMethod(editor, "save"));
        QTRY_COMPARE(choice->property("currentText").toString(), QString("Renamed House"));
        QCOMPARE(choice->property("currentValue").toString(), secondId);
        QCOMPARE(local.installations().size(), 2);
        QCOMPARE(local.settings.value("connections/credentials/" + secondId).toMap(), credentials);
        QVERIFY(instructions->property("text").toString().contains("Choose WHAT API"));
        QVERIFY(local.setInstallationEnabled(firstId, false));
        QTRY_COMPARE(choice->property("currentValue").toString(), secondId);

        QVERIFY(QMetaObject::invokeMethod(replace, "clicked"));
        QTRY_VERIFY(tokenDialog->property("opened").toBool());
        QCOMPARE(tokenDialog->property("installationId").toString(), secondId);
        QVERIFY(!tokenDialog->property("allInstallations").toBool());
        QVERIFY(QMetaObject::invokeMethod(tokenDialog, "reject"));
        QCOMPARE(local.settings.value("connections/credentials/" + secondId).toMap(), credentials);
        QVERIFY(QMetaObject::invokeMethod(replace, "clicked"));
        QVERIFY(QMetaObject::invokeMethod(tokenDialog, "accept"));
        QTRY_VERIFY(!tokenDialog->property("visible").toBool());
        QCOMPARE(choice->property("currentValue").toString(), secondId);
        QVERIFY(local.settings.value("connections/credentials/" + secondId).toMap() != credentials);
        if (!qEnvironmentVariableIsEmpty("WHAT_PAIRING_PREVIEW")) {
            QTest::qWait(100);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_PAIRING_PREVIEW")));
        }

        // Deleting a selected entry picks a valid remaining entry; an empty list stays usable.
        QVERIFY(local.deleteInstallation(secondId));
        QTRY_COMPARE(choice->property("currentValue").toString(), firstId);
        QVERIFY(local.deleteInstallation(firstId));
        QTRY_COMPARE(choice->property("currentIndex").toInt(), -1);
        QVERIFY(!edit->property("enabled").toBool());
        QVERIFY(!copy->property("enabled").toBool());
        QVERIFY(!replace->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(add, "clicked"));
        name->setProperty("text", "New First House");
        enabled->setProperty("checked", false);
        QVERIFY(QMetaObject::invokeMethod(editor, "save"));
        QCOMPARE(local.installations().size(), 1);
        QTRY_COMPARE(choice->property("currentValue").toString(),
                     local.installations().first().toMap()["id"].toString());
    }

    void overviewTransportAndPairing() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *initialPanel = window->findChild<QQuickItem *>("overviewPairingPanel");
        auto *initialReveal = window->findChild<QQuickItem *>("pairingReveal");
        QVERIFY(initialPanel && initialReveal);
        QVERIFY(!initialPanel->property("expanded").toBool());
        QCOMPARE(initialReveal->height(), 0.0);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *modulesHeading = window->findChild<QQuickItem *>("overviewModulesHeading");
        auto *serviceControls = window->findChild<QQuickItem *>("overviewServiceControls");
        QVERIFY(modulesHeading && serviceControls);
        const auto sceneTop = [](QQuickItem *item) { return item->mapToScene(QPointF()).y(); };
        QTRY_VERIFY(sceneTop(initialPanel) < sceneTop(modulesHeading));
        std::function<QQuickItem *(QQuickItem *, QString)> find = [&](QQuickItem *item,
                                                                      QString name) -> QQuickItem * {
            if (item->objectName() == name)
                return item;
            for (auto *child : item->childItems())
                if (auto *r = find(child, name))
                    return r;
            return nullptr;
        };
        auto *selection = find(window->contentItem(), "pairingInstallation");
        QVERIFY(selection);
        QCOMPARE(selection->property("count").toInt(), 1);
        auto *summary = find(window->contentItem(), "connectionSummaryStatus");
        QVERIFY(summary);
        QCOMPARE(summary->property("text").toString(), QString("Home Assistant Connections Stopped"));
        QVERIFY(local.saveInstallation({{"name", "Another House"}, {"type", "api"}, {"url", ""}, {"enabled", true}, {"controls", false}}));
        QTRY_COMPARE(selection->property("count").toInt(), 2);
        QVERIFY(QMetaObject::invokeMethod(window, "openConnectionSettings"));
        QCOMPARE(window->property("previewPage").toInt(), 2);
        auto *security = find(window->contentItem(), "Pairing and SecuritySectionToggle");
        QVERIFY(security);
        QVERIFY(QMetaObject::invokeMethod(security, "clicked"));
        auto *appearance = find(window->contentItem(), "AppearanceSectionToggle");
        QVERIFY(appearance);
        QVERIFY(QMetaObject::invokeMethod(appearance, "clicked"));
        auto *connectionToggle = find(window->contentItem(), "Home Assistant InstallationsSectionToggle");
        QVERIFY(connectionToggle);
        QVERIFY(QMetaObject::invokeMethod(connectionToggle, "clicked"));
        if (!qEnvironmentVariableIsEmpty("WHAT_SETTINGS_PREVIEW")) {
            QTest::qWait(300);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_SETTINGS_PREVIEW")));
        }
        window->setProperty("previewPage", 0);

        auto *panel = find(window->contentItem(), "overviewPairingPanel");
        auto *toggle = find(window->contentItem(), "overviewPairingToggle");
        auto *scroll = find(window->contentItem(), "overviewScroll");
        QVERIFY(panel);
        QVERIFY(toggle);
        QVERIFY(scroll);
        QVERIFY(!panel->property("expanded").toBool());
        auto *reveal = find(window->contentItem(), "pairingReveal");
        QVERIFY(reveal);
        QCOMPARE(reveal->height(), 0.0);
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        QTRY_VERIFY(reveal->height() > 0);
        // Connecting or disconnecting must not override an explicit expansion.
        local.clientCount = 1;
        emit local.changed();
        QTRY_COMPARE(window->property("status").toMap()["clients"].toInt(), 1);
        QTRY_VERIFY(sceneTop(panel) >= sceneTop(serviceControls) + serviceControls->height());
        QVERIFY(panel->property("expanded").toBool());
        // Losing one installation must not move pairing up while another is connected.
        local.clientCount = 2;
        emit local.changed();
        QTRY_COMPARE(window->property("status").toMap()["clients"].toInt(), 2);
        local.clientCount = 1;
        emit local.changed();
        QTRY_COMPARE(window->property("status").toMap()["clients"].toInt(), 1);
        QTRY_VERIFY(sceneTop(panel) >= sceneTop(serviceControls) + serviceControls->height());
        local.clientCount = 0;
        emit local.changed();
        QTRY_COMPARE(window->property("status").toMap()["clients"].toInt(), 0);
        QTRY_VERIFY(sceneTop(panel) < sceneTop(modulesHeading));
        QCOMPARE(panel, initialPanel);
        QCOMPARE(find(window->contentItem(), "pairingInstallation"), selection);
        QVERIFY(panel->property("expanded").toBool());
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        QVERIFY(!panel->property("expanded").toBool());
        QTRY_COMPARE(reveal->height(), 0.0);
        local.setAnimationsEnabled(false);
        QTRY_VERIFY(!window->property("animationsEnabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        QVERIFY(reveal->height() > 0);
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        QCOMPARE(reveal->height(), 0.0);
        local.setAnimationsEnabled(true);
        local.clientCount = 1;
        emit local.changed();
        QTRY_VERIFY(sceneTop(panel) >= sceneTop(serviceControls) + serviceControls->height());
        auto *flick = qvariant_cast<QObject *>(scroll->property("contentItem"));
        QVERIFY(flick);
        flick->setProperty("contentY", 0);
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        QTRY_VERIFY(flick->property("contentY").toDouble() > 0);
        QTRY_VERIFY(panel->mapToItem(qobject_cast<QQuickItem *>(flick), QPointF(0, panel->height())).y()
                    <= flick->property("height").toDouble() + 2);
        QVERIFY(!local.browserVisible);
        window->setProperty("previewPage", 1);
        QTRY_VERIFY(local.browserVisible);
        window->hide();
        QTRY_VERIFY(!local.browserVisible);
    }

    void hiddenSensorBrowserDefersRefresh() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.setBrowserVisible(false);
        local.setBrowserFilters("hidden_fixture", "all", false, false);
        auto reading = row("hidden_fixture", 42);
        reading["name"] = "hidden_fixture";
        local.mergeReadings("windows", {reading}, 1);
        const int before = local.browserModel.rowCount();
        for (int i = 0; i < 100; ++i)
            emit local.sensorsChanged();
        QCOMPARE(local.browserModel.rowCount(), before);
        QVERIFY(local.browserPending);
        local.setBrowserInteracting(false);
        QVERIFY(local.browserPending);
        local.setBrowserVisible(true);
        QVERIFY(!local.browserPending);
        QTRY_COMPARE(local.browserModel.rowCount(), local.browser().size());
        QVERIFY(local.browserModel.rowCount() > 0);
    }

    void startupSettings() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *root = engine.rootObjects().first();
        root->setProperty("previewPage", 2);
        auto *minimised = root->findChild<QObject *>("startMinimisedSetting");
        auto *notice = root->findChild<QObject *>("startupNotificationSetting");
        auto *startup = root->findChild<QObject *>("windowsStartupSetting");
        QVERIFY(minimised);
        QVERIFY(notice);
        QVERIFY(startup);
        QCOMPARE(local.info()["startupAvailable"].toBool(), StartupRegistration::supported());
        if (!StartupRegistration::supported()) {
            QVERIFY(!startup->property("enabled").toBool());
            QVERIFY(!startup->property("checked").toBool());
        }
        local.setStartMinimised(true);
        local.setStartupNotice(true);
        QTRY_VERIFY(notice->property("enabled").toBool());
        local.setStartMinimised(false);
        QTRY_VERIFY(!notice->property("enabled").toBool());
        QVERIFY(local.info()["startupNotice"]
                    .toBool()); // Disabling background startup retains the notification choice.
        local.setStartMinimised(true);
        QTRY_VERIFY(notice->property("enabled").toBool());
        notice->setProperty("checked", false);
        QVERIFY(QMetaObject::invokeMethod(notice, "clicked"));
        QVERIFY(!local.info()["startupNotice"].toBool());
        QSettings saved(dir.path() + "/test-settings.yaml", SettingsStore::format());
        QVERIFY(saved.value("start_minimised").toBool());
        QVERIFY(!saved.value("startup_notice").toBool());
    }

    void startupNoticePresentation() {
        bool opened = false;
        std::unique_ptr<QWidget> notice(createStartupNotice([&] { opened = true; }));
        QVERIFY(notice->testAttribute(Qt::WA_ShowWithoutActivating));
        QVERIFY(notice->testAttribute(Qt::WA_TranslucentBackground));
        if (!qEnvironmentVariableIsEmpty("WHAT_NOTICE_PREVIEW"))
            notice->grab().save(qEnvironmentVariable("WHAT_NOTICE_PREVIEW"));
        auto *button = notice->findChild<QPushButton *>("openApp");
        QVERIFY(button);
        button->click();
        QVERIFY(opened);
    }

    void notificationValidationAndPermissions() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.icueTimer.stop();
        QJsonObject args{{"title", "Home Assistant"}, {"message", "Washing finished"}};
        QVERIFY(local.sendDesktopNotification(args).contains("Enable"));
        local.setControlPermission("notifications", true);
        QVERIFY(local.controlState()["notifications_enabled"].toBool());
        QVERIFY(!local.controlState()["permissions"].toObject().contains("notifications"));
        QVERIFY(local.sendDesktopNotification(args).isEmpty());
        QVERIFY(local.sendDesktopNotification(args).contains("one per second"));
        local.notificationClock.invalidate();
        auto bad = args;
        bad["title"] = QString(64, 'x');
        QVERIFY(!local.sendDesktopNotification(bad).isEmpty());
        bad = args;
        bad["message"] = QString(256, 'x');
        QVERIFY(!local.sendDesktopNotification(bad).isEmpty());
        bad = args;
        bad["level"] = "critical";
        QVERIFY(!local.sendDesktopNotification(bad).isEmpty());
        bad = args;
        bad["url"] = "https://example.com";
        QVERIFY(!local.sendDesktopNotification(bad).isEmpty());
        bad = args;
        bad["title"] = "one\ntwo";
        QVERIFY(!local.sendDesktopNotification(bad).isEmpty());
        QString captured;
        local.notificationSender = [&](const QString &title, const QString &message, const QString &level) {
            captured = title + "|" + message + "|" + level;
            return QString();
        };
        local.testMode = false;
        args["message"] = "<b>Plain text</b>\nSecond line";
        args["level"] = "warning";
        QVERIFY(local.sendDesktopNotification(args).isEmpty());
        QCOMPARE(captured, QString("Home Assistant|<b>Plain text</b>\nSecond line|warning"));
        local.testMode = true;
        local.setModuleEnabled("windows", false);
        local.notificationClock.invalidate();
        QVERIFY(local.sendDesktopNotification(args).contains("Enable"));
    }

    void iconTransparencyAndAbout() {
        const QIcon icon(":/assets/app-icon.ico");
        QVERIFY(!icon.isNull());
        for (int size : {16, 24, 32, 48, 64, 128, 256}) {
            auto image = icon.pixmap(size, size).toImage();
            QVERIFY(!image.isNull());
            QVERIFY(image.hasAlphaChannel());
            QCOMPARE(qAlpha(image.pixel(0, 0)), 0);
        }
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.sampleTimer.stop();
        local.publishTimer.stop();
        local.icueTimer.stop();
        local.stopService();
        updater.available = "0.5.99";
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->setPosition(-32000, -32000);
        window->show();
        window->setProperty("previewPage", 6);
        std::function<QQuickItem *(QQuickItem *, QString)> find = [&](QQuickItem *item,
                                                                      QString name) -> QQuickItem * {
            if (item->objectName() == name)
                return item;
            for (auto *child : item->childItems())
                if (auto *r = find(child, name))
                    return r;
            return nullptr;
        };
        QQuickItem *settings = nullptr, *diagnostics = nullptr, *about = nullptr;
        QTRY_VERIFY((window->grabWindow(), settings = find(window->contentItem(), "settingsNavigation")) !=
                    nullptr);
        diagnostics = find(window->contentItem(), "diagnosticsNavigation");
        about = find(window->contentItem(), "aboutNavigation");
        QVERIFY(diagnostics);
        QVERIFY(about);
        QVERIFY(about->y() < diagnostics->y());
        QVERIFY(diagnostics->y() < settings->y());
        QVERIFY(find(window->contentItem(), "updateBadge")->isVisible());
        updater.available = "0.0.0";
        emit updater.changed();
        QTRY_VERIFY(!find(window->contentItem(), "updateBadge")->isVisible());
        updater.available = "0.5.99";
        emit updater.changed();
        QTRY_VERIFY(find(window->contentItem(), "updateBadge")->isVisible());
        auto *aboutVersion = window->findChild<QObject *>("aboutVersion");
        QVERIFY(aboutVersion);
        QCOMPARE(aboutVersion->property("text").toString(), "WHAT v" + local.info()["version"].toString());
        auto *logo = find(window->contentItem(), "animatedSidebarLogo");
        QVERIFY(logo);
        local.setUiTiming("logo", 2400);
        local.setUiTiming("logoPause", 350);
        QTRY_COMPARE(logo->property("animationDuration").toInt(), 2400);
        QCOMPARE(logo->property("pauseDuration").toInt(), 350);
        local.resetUiTimings();
        QTRY_COMPARE(logo->property("animationDuration").toInt(), 1500);
        const auto originalSize = QSizeF(logo->width(), logo->height());
        auto *brand = find(window->contentItem(), "sidebarBrandExpansion");
        QVERIFY(brand);
        QTest::mouseMove(window,
                         brand->mapToScene(QPointF(brand->width() / 2, brand->height() / 2)).toPoint());
        QTRY_VERIFY(logo->property("textHovered").toBool());
        QTest::qWait(180);
        QCOMPARE(QSizeF(logo->width(), logo->height()), originalSize);
        QVERIFY(logo->property("animating").toBool());
        local.setAnimationsEnabled(false);
        QVERIFY(!local.settings.value("ui/animations", true).toBool());
        QTRY_VERIFY(!logo->property("animating").toBool());
        QTest::qWait(250);
        QCOMPARE(logo->property("progress").toDouble(), 0.0);
        QTest::mouseMove(window, QPoint(int(window->width() - 10), 10));
        QTRY_VERIFY(!logo->property("textHovered").toBool());
        local.setAnimationsEnabled(true);
        QTest::qWait(150);
        QCOMPARE(logo->property("progress").toDouble(), 0.0);
        window->setProperty("previewPage", 0);
        window->grabWindow();
        auto *title = find(window->contentItem(), "sidebarBrandTitle");
        QVERIFY(title);
        QCOMPARE(title->property("horizontalAlignment").toInt(), int(Qt::AlignHCenter));
        auto *expansion = find(window->contentItem(), "sidebarBrandExpansion");
        QVERIFY(expansion);
        QVERIFY(expansion->property("text").toString().contains("Windows Home\nAssistant Toolkit"));
        local.setLicense("free");
        auto *licenseWarning = find(window->contentItem(), "hwinfoLicenseWarning");
        QVERIFY(licenseWarning);
        QTRY_VERIFY(licenseWarning->isVisible());
        // The warning follows the edition selected in Modules; it has no dismiss button.
        local.setLicense("pro");
        QTRY_VERIFY(!licenseWarning->isVisible());
        QCOMPARE(local.settings.value("license").toString(), QString("pro"));

        for (const auto &key : {"overview-windows", "overview-hwinfo", "overview-icue", "overview-comms"})
            QVERIFY(find(window->contentItem(), key));
        if (!qEnvironmentVariableIsEmpty("WHAT_OVERVIEW_PREVIEW")) {
            QTest::qWait(150);
            window->grabWindow().save(qEnvironmentVariable("WHAT_OVERVIEW_PREVIEW"));
        }
        window->setProperty("previewPage", 6);

        if (!qEnvironmentVariableIsEmpty("WHAT_ABOUT_PREVIEW")) {
            QTest::qWait(250);
            window->grabWindow().save(qEnvironmentVariable("WHAT_ABOUT_PREVIEW"));
        }
        local.prepareDiagnosticReport();
        QTRY_VERIFY_WITH_TIMEOUT(!local.diagnosticReportBusy(), 5000);
        auto *reportDialog = window->findChild<QObject *>("diagnosticReportDialog");
        QVERIFY(reportDialog);
        QVERIFY(QMetaObject::invokeMethod(reportDialog, "open"));
        QTest::qWait(150);
        if (!qEnvironmentVariableIsEmpty("WHAT_REPORT_PREVIEW"))
            window->grabWindow().save(qEnvironmentVariable("WHAT_REPORT_PREVIEW"));
        QVERIFY(QMetaObject::invokeMethod(reportDialog, "close"));
        window->hide();
    }

    void displayLayoutPlanning() {
        const QString a(24, 'a'), b(24, 'b'), d(24, 'd');
        auto monitor = [](QString id, int x, bool enabled = true) {
            return QJsonObject{{"id", id},
                               {"name", id},
                               {"group", id},
                               {"enabled", enabled},
                               {"connected", true},
                               {"primary", x == 0 && enabled},
                               {"x", x},
                               {"y", 0},
                               {"width", 1920},
                               {"height", 1080},
                               {"rotation", 1},
                               {"refresh", 60.0},
                               {"mode", enabled ? "extended" : "disconnected"}};
        };
        QJsonObject state{{"available", true},
                          {"monitors", QJsonArray{monitor(a, 0), monitor(b, 1920), monitor(d, 0, false)}}};
        auto start = Displays::layout(state);
        QVERIFY(Displays::validateLayout(start).isEmpty());
        auto primary = Displays::edit(state, {{"operation", "primary"}, {"monitor", b}});
        QVERIFY(!primary.contains("error"));
        auto rows = primary["layout"].toArray();
        QCOMPARE(rows[0].toObject()["x"].toInt(), -1920);
        QCOMPARE(rows[1].toObject()["x"].toInt(), 0);
        auto mirror = Displays::edit(state, {{"operation", "mirror"}, {"monitor", b}, {"source", a}});
        QVERIFY(!mirror.contains("error"));
        rows = mirror["layout"].toArray();
        QCOMPARE(rows[0].toObject()["group"], rows[1].toObject()["group"]);
        QVERIFY(Displays::edit(state, {{"operation", "mirror"}, {"monitor", a}, {"source", a}})
                    .contains("error"));
        auto disconnected = Displays::edit(state, {{"operation", "disconnect"}, {"monitor", a}});
        QVERIFY(!disconnected.contains("error"));
        QCOMPARE(disconnected["layout"].toArray().first().toObject()["x"].toInt(), 0);
        QJsonObject single{{"monitors", QJsonArray{monitor(a, 0)}}};
        QVERIFY(Displays::edit(single, {{"operation", "disconnect"}, {"monitor", a}}).contains("error"));
        auto extended = Displays::edit(state, {{"operation", "extend"}, {"monitor", d}});
        QVERIFY(!extended.contains("error"));
        QCOMPARE(extended["layout"].toArray().last().toObject()["x"].toInt(), 3840);
        for (int rotation = 1; rotation <= 4; ++rotation) {
            auto rotated = monitor(d, 0, false);
            rotated["rotation"] = rotation;
            rotated["width"] = (rotation == 2 || rotation == 4) ? 1080 : 1920;
            rotated["height"] = (rotation == 2 || rotation == 4) ? 1920 : 1080;
            auto rotatedState = state;
            rotatedState["monitors"] = QJsonArray{monitor(a, 0), rotated};
            auto plan = Displays::edit(rotatedState, {{"operation", "extend"}, {"monitor", d}});
            QVERIFY(!plan.contains("error"));
            const auto enabled = plan["layout"].toArray().last().toObject();
            QCOMPARE(enabled["rotation"], rotated["rotation"]);
            QCOMPARE(enabled["width"], rotated["width"]);
            QCOMPARE(enabled["height"], rotated["height"]);
        }
        QVERIFY(!Displays::validateLayout({}).isEmpty());
        auto duplicate = start;
        duplicate.append(start.first());
        QVERIFY(!Displays::validateLayout(duplicate).isEmpty());
        auto invalid = start;
        auto row = invalid[0].toObject();
        row["x"] = 0.5;
        invalid[0] = row;
        QVERIFY(!Displays::validateLayout(invalid).isEmpty());
        auto cloneState = state;
        auto clones = mirror["layout"].toArray();
        for (int i = 0; i < clones.size(); ++i) {
            auto r = clones[i].toObject();
            r["enabled"] = true;
            clones[i] = r;
        }
        cloneState["monitors"] = clones;
        auto split = Displays::edit(cloneState, {{"operation", "extend"}, {"monitor", a}});
        QVERIFY2(!split.contains("error"), qPrintable(split["error"].toString()));
        auto splitRows = split["layout"].toArray();
        QVERIFY(splitRows[0].toObject()["group"] != splitRows[1].toObject()["group"]);
        auto renumbered = mirror["layout"].toArray();
        for (int i = 0; i < renumbered.size(); ++i) {
            auto r = renumbered[i].toObject();
            r["group"] = b;
            renumbered[i] = r;
        }
        QVERIFY(Displays::matches(mirror["layout"].toArray(), renumbered));
        QVERIFY(!Displays::matches(start, renumbered));
    }

    void disabledDisplaysWithholdSavedLayoutsOnWire() {
        for (bool deltas : {false, true}) {
            QTemporaryDir dir;
            Controller app(dir.path(), true);
            app.stopService();
            app.sampleTimer.stop();
            app.publishTimer.stop();
            app.setDisplaysEnabled(true);
            QTRY_VERIFY(!app.displayJob.isRunning());
            const QString layoutId = "12345678-1234-1234-1234-123456789abc", label = "Private audit layout";
            app.displayLayouts = {QJsonObject{{"id", layoutId}, {"name", label}, {"layout", QJsonArray{}}}};
            app.displayMessage = label;
            QVERIFY(app.startService());
            app.publish();
            QWebSocket socket;
            auto ssl = QSslConfiguration::defaultConfiguration();
            ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
            socket.setSslConfiguration(ssl);
            QSignalSpy messages(&socket, &QWebSocket::textMessageReceived);
            socket.open(QUrl(QString("wss://127.0.0.1:%1").arg(app.activePort)));
            QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
            socket.sendTextMessage(QJsonDocument(QJsonObject{{"type", "auth"},
                                                             {"protocol", 2},
                                                             {"token", app.token},
                                                             {"delta_v1", deltas}})
                                       .toJson(QJsonDocument::Compact));
            QTRY_VERIFY(messages.size() >= 2);
            auto lastFrame = [&] {
                return QJsonDocument::fromJson(messages.last()[0].toString().toUtf8()).object();
            };
            QTRY_VERIFY(messages.last()[0].toString().contains(label));
            for (bool disableWindows : {false, true}) {
                messages.clear();
                if (disableWindows)
                    app.setModuleEnabled("windows", false);
                else
                    app.setDisplaysEnabled(false);
                QTRY_VERIFY(!messages.empty());
                QTRY_VERIFY(!lastFrame()["displays"].toObject()["enabled"].toBool());
                const auto wire = messages.last()[0].toString();
                QVERIFY(!wire.contains(label));
                QVERIFY(!wire.contains(layoutId));
                const auto display = lastFrame()["displays"].toObject();
                QVERIFY(display["layouts"].toArray().isEmpty());
                QVERIFY(display["monitors"].toArray().isEmpty());
                QCOMPARE(display["status"].toString(), QString("Disabled"));
                QCOMPARE(lastFrame()["type"].toString(), deltas ? QString("delta") : QString("snapshot"));
                QCOMPARE(app.displayLayouts.size(), 1); // Local configuration survives revocation.
                QVERIFY(!QJsonDocument(app.snapshot()).toJson().contains(label.toUtf8()));
                if (!disableWindows) {
                    app.setDisplaysEnabled(true);
                    QTRY_VERIFY(!app.displayJob.isRunning());
                    app.publish();
                    QTRY_VERIFY(lastFrame()["displays"].toObject()["enabled"].toBool());
                    QTRY_VERIFY(messages.last()[0].toString().contains(label));
                }
            }
            app.displayRollback = {QJsonObject{{"id", "recovery-fixture"}}};
            QVERIFY(app.displays()["pending"].toBool()); // Local recovery remains visible.
            QVERIFY(!app.snapshot()["displays"].toObject()["pending"].toBool());
            socket.close();
            app.stopService();
        }
    }

    void displayEditorAtomicPlanning() {
        const QString a(24, 'a'), b(24, 'b');
        auto monitor = [](QString id, int x) {
            return QJsonObject{{"id", id},      {"group", id},    {"enabled", true},
                               {"width", 1920}, {"height", 1080}, {"x", x},
                               {"y", 0},        {"rotation", 1},  {"refresh", 60}};
        };
        QJsonObject state{{"monitors", QJsonArray{monitor(a, 0), monitor(b, 1920)}}};
        auto expected = Displays::layout(state), wanted = expected;
        auto second = wanted[1].toObject();
        second["rotation"] = 2;
        second["width"] = 1080;
        second["height"] = 1920;
        second["refresh"] = 120;
        wanted[1] = second;
        auto args = QJsonObject{{"operation", "apply"}, {"layout", wanted}, {"expected", expected}};
        auto result = Displays::edit(state, args);
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        QCOMPARE(result["layout"].toArray(), wanted);
        auto changed = state["monitors"].toArray();
        auto first = changed[0].toObject();
        first["refresh"] = 144;
        changed[0] = first;
        QVERIFY(Displays::edit(QJsonObject{{"monitors", changed}}, args).contains("error"));
        second["x"] = 100;
        wanted[1] = second;
        args["layout"] = wanted;
        QVERIFY(Displays::edit(state, args).contains("error"));
        second["id"] = QString(24, 'c');
        wanted[1] = second;
        args["layout"] = wanted;
        QVERIFY(Displays::edit(state, args).contains("error"));
        auto malformed = expected;
        first = malformed[0].toObject();
        first["refresh"] = "60";
        malformed[0] = first;
        QVERIFY(!Displays::validateLayout(malformed).isEmpty());
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.setDisplaysEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(!local.displayJob.isRunning(), 5000);
        local.localDisplayCommand(
            {{"operation", "apply"}, {"layout", QVariantList{}}, {"expected", QVariantList{}}});
        QVERIFY(local.displaysState()["status"].toString().contains("Invalid"));
    }

    void displayModuleAndRecovery() {
        QTemporaryDir dir;
        Controller app(dir.path(), true);
        app.stopService();
        app.sampleTimer.stop();
        QVERIFY(!app.displaysEnabled());
        app.setDisplaysEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(!app.displayJob.isRunning(), 5000);
        QVERIFY(app.displaysEnabled());
        QVERIFY(!app.displaysState()["control_enabled"].toBool());
        app.setDisplayControl(true);
        QVERIFY(app.displaysState()["control_enabled"].toBool());
        const QString id(24, 'a');
        QJsonObject m{{"id", id},
                      {"name", "Test monitor"},
                      {"group", id},
                      {"connected", true},
                      {"enabled", true},
                      {"primary", true},
                      {"width", 1920},
                      {"height", 1080},
                      {"x", 0},
                      {"y", 0},
                      {"rotation", 1},
                      {"refresh", 60.0},
                      {"mode", "extended"}};
        app.displayInfo = {{"available", true}, {"monitors", QJsonArray{m}}};
        app.displayClock.restart();
        app.mergeReadings("windows", Displays::readings(app.displayInfo), 6);
        app.setBrowserFilters("Test monitor", "all", false, false);
        QVERIFY(!app.browser().isEmpty());
        app.mergeReadings("windows", {}, 1);
        QVERIFY(app.catalog["win_display_" + id + "_width"]["available"].toBool());
        app.displayRollback = Displays::layout(app.displayInfo);
        app.localDisplayCommand({{"operation", "keep"}});
        QVERIFY(app.displayRollback.isEmpty());
        QVERIFY(!app.displays()["pending"].toBool());
        app.setDisplaysEnabled(false);
        QVERIFY(app.browser().isEmpty());
        QVERIFY(!app.displaysState()["control_enabled"].toBool());
        QVERIFY(app.settings.value("controls/displays").toBool());
    }

    void lightingLinkCommandsAndReadback() {
        AutomationFixture fixture;
        IcueAutomation api(fixture.endpoint);
        int released = 0;
        auto send = [&](QJsonObject args) {
            return api.lightingCommand(args, [&] { ++released; }, [] { return false; });
        };
        QVERIFY(
            send({{"effect", "StaticColor"}, {"parameters", QJsonObject{{"Color1", "#204060"}}}}).isEmpty());
        QCOMPARE(fixture.commands.load(), 1);
        QCOMPARE(released, 1);
        auto lighting = api.poll(true)["lighting"].toObject();
        QCOMPARE(lighting["parameters"].toObject()["Color1"].toString(), QString("#204060"));
        QVERIFY(
            !send({{"expected_effect", "StaticColor"}, {"parameters", QJsonObject{{"Speed", 1}}}}).isEmpty());
        QCOMPARE(fixture.commands.load(), 1);
        QVERIFY(send({{"effect", "RainbowWave"}, {"parameters", QJsonObject{{"Speed", 1}, {"Direction", 1}}}})
                    .isEmpty());
        QVERIFY(
            send({{"expected_effect", "RainbowWave"}, {"parameters", QJsonObject{{"Speed", 2}}}}).isEmpty());
        lighting = api.poll(true)["lighting"].toObject();
        QCOMPARE(lighting["parameters"].toObject()["Speed"].toInt(), 2);
        QCOMPARE(lighting["parameters"].toObject()["Direction"].toInt(), 1);
        const int before = fixture.commands.load();
        QVERIFY(
            !send({{"expected_effect", "StaticColor"}, {"parameters", QJsonObject{{"Color1", "#102030"}}}})
                 .isEmpty());
        QVERIFY(!send({{"effect", "RainbowWave"}, {"parameters", QJsonObject{{"Speed", 3}}}}).isEmpty());
        QVERIFY(!send({{"effect", "RainbowWave"}, {"method", "anything"}}).isEmpty());
        bool cancelled = false;
        QVERIFY(!api.lightingCommand(
                        {{"effect", "StaticColor"}}, [&] { cancelled = true; }, [&] { return cancelled; })
                     .isEmpty());
        QCOMPARE(fixture.commands.load(), before);
        fixture.ignoreLightingWrite = true;
        QVERIFY(send({{"effect", "StaticColor"}}).contains("did not match"));
        QCOMPARE(fixture.commands.load(),
                 before + 1); // No automatic retry after an acknowledged mismatch.
        fixture.ignoreLightingWrite = false;
        fixture.deny = true;
        QVERIFY(send({{"effect", "StaticColor"}}).contains("NotAllowed"));
        fixture.deny = false;
        QVERIFY(send({{"stop", true}}).isEmpty());
        lighting = api.poll(true)["lighting"].toObject();
        QVERIFY(lighting["available"].toBool());
        QVERIFY(lighting["effect"].isNull());
    }

    void automationReadOnlyLighting() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        AutomationFixture fixture;
        IcueAutomation api(fixture.endpoint);
        auto state = api.poll(true);
        QVERIFY(state["connected"].toBool());
        QCOMPARE(state["readings"].toArray()[0].toObject()["value"].toString(), QString("None"));
        fixture.lightingMode = 1;
        state = api.poll(true);
        const auto rows = state["readings"].toArray();
        QCOMPARE(rows.size(), 7);
        QCOMPARE(rows[0].toObject()["value"].toString(), QString("Colour wave"));
        QCOMPARE(rows[1].toObject()["value"].toString(), QString("2"));
        QCOMPARE(rows[2].toObject()["value"].toString(), QString("1"));
        QCOMPARE(rows[4].toObject()["value"].toString(), QString("#00aaff"));
        QVERIFY(!rows[5].toObject()["available"].toBool());
        QCOMPARE(fixture.commands.load(), 0);
        fixture.lightingMode = 2;
        state = api.poll(true);
        QVERIFY(state["connected"].toBool()); // Optional read failure does not break profile switching.
        QVERIFY(!state["readings"].toArray()[0].toObject()["available"].toBool());
        QVERIFY(state["telemetry_status"].toString().contains("NotAllowed"));
        auto definitions = IcueAutomation::lightingReadings(
            {{"effect",
              QJsonObject{{"type", "RainbowWave"}, {"parameters", QJsonArray{"Speed", "Direction"}}}}});
        QVERIFY(!definitions[1].toObject()["available"].toBool());
        auto missing = IcueAutomation::lightingReadings({});
        QVERIFY(!missing[0].toObject()["available"].toBool());
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.sampleTimer.stop();
        local.publishTimer.stop();
        local.icueTimer.stop();
        local.setModuleEnabled("icue", true);
        local.setIcueOption("automation", true);
        QTRY_VERIFY(!local.icueJob.isRunning());
        fixture.lightingMode = 1;
        state = api.poll(true);
        local.acceptIcueAutomation(state);
        const QString id = "icue_automation_lighting_effect";
        auto findLocal = [&] {
            for (auto value : local.allReadings())
                if (value.toObject()["id"] == id)
                    return value.toObject();
            return QJsonObject{};
        };
        QVERIFY(findLocal()["available"].toBool());
        QVERIFY(local.selected.contains(id));
        local.acceptIcue({});
        QVERIFY(findLocal()["available"].toBool()); // RGB refresh is a separate scope.
        local.setIcueOption("direct", false);
        QTRY_VERIFY(!local.icueJob.isRunning());
        local.acceptIcueAutomation(state);
        QVERIFY(findLocal()["available"].toBool());
        QVERIFY(!local.icueAutomationState()["control_enabled"].toBool());
        local.setIcueOption("automation", false);
        QVERIFY(!findLocal()["available"].toBool());
        QVERIFY(!local.readingBrowserEnabled(findLocal()));
        QVERIFY(api.poll(false)["readings"].toArray().isEmpty());
    }

    void automationPipeAndCommandValidation() {
        AutomationFixture fixture;
        QVERIFY(fixture.listening);
        IcueAutomation api(fixture.endpoint);
        auto state = api.poll(true);
        QVERIFY2(state["connected"].toBool(), qPrintable(state["status"].toString()));
        QCOMPARE(state["controls"].toArray().size(), 4);
        int cleared = 0;
        const QJsonObject args{{"control", "profile"}, {"option", "b"}};
        QVERIFY(api.command(args, [&] { ++cleared; }, [] { return false; }).isEmpty());
        QCOMPARE(cleared, 1);
        QCOMPARE(fixture.commands.load(), 1);
        QCOMPARE(api.poll(true)["controls"].toArray()[0].toObject()["current"].toString(), QString("b"));
        fixture.removeB = true;
        QVERIFY(!api.command(args, [&] { ++cleared; }, [] { return false; }).isEmpty());
        QCOMPARE(fixture.commands.load(), 1);
        QCOMPARE(cleared, 1);
        fixture.removeB = false;
        fixture.deny = true;
        QVERIFY(api.command(
                       {{"control", "profile"}, {"option", "a"}}, [] {}, [] { return false; })
                    .contains("NotAllowed"));
        QCOMPARE(fixture.commands.load(), 1);
        QVERIFY(!api.command(args, [] {}, [] { return true; }).isEmpty());
        QVERIFY(!IcueAutomation::validate({{"control", "profile"}, {"option", "b"}, {"method", "anything"}},
                                          state)
                     .isEmpty());
        fixture.deny = false;
        for (const auto &value : state["controls"].toArray()) {
            const auto control = value.toObject();
            if (control["id"] == "profile")
                continue;
            QVERIFY(api.command(
                           {{"control", control["id"]}, {"option", "preset"}}, [&] { ++cleared; },
                           [] { return false; })
                        .isEmpty());
        }
        QCOMPARE(fixture.commands.load(), 4);
        QCOMPARE(cleared, 1);
        fixture.oversized = true;
        QVERIFY(!api.poll(true)["connected"].toBool());
        QVERIFY(!api.poll(false)["enabled"].toBool());
    }

    void icueSubmodulesAndAutomationBrowser() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.sampleTimer.stop();
        local.publishTimer.stop();
        local.icueTimer.stop();
        local.setModuleEnabled("icue", true);
        QTRY_VERIFY(!local.icueJob.isRunning());
        QVERIFY(local.icueDirectEnabled());
        QVERIFY(!local.icueAutomationEnabled());
        local.setIcueOption("automation", true);
        QTRY_VERIFY(!local.icueJob.isRunning());
        IcueSdk sdk(QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(ICUE_TEST_DLL));
        local.acceptIcue(sdk.poll(true));
        const QJsonObject state{
            {"enabled", true},
            {"connected", true},
            {"status", "Connected"},
            {"controls", QJsonArray{QJsonObject{
                             {"id", "profile"},
                             {"name", "iCUE profile"},
                             {"device", "iCUE"},
                             {"confirmed", true},
                             {"current", "a"},
                             {"options", QJsonArray{QJsonObject{{"id", "a"}, {"name", "Default"}}}}}}}};
        local.acceptIcueAutomation(state);
        local.setBrowserFilters("", "icue", false, false);
        QVERIFY(!local.icueAutomationState()["control_enabled"].toBool());
        auto has = [&](QString control) {
            for (auto value : local.browser())
                if (value.toMap()["control"] == control)
                    return true;
            return false;
        };
        QVERIFY(has("automation"));
        QVERIFY(has("rgb"));
        local.setIcueOption("direct", false);
        QTRY_VERIFY(!local.icueJob.isRunning());
        local.acceptIcueAutomation(state);
        QVERIFY(!has("rgb"));
        QVERIFY(has("automation"));
        QVERIFY(!local.snapshot()["icue"].toObject()["enabled"].toBool());
        local.setIcueOption("automationControl", true);
        QTRY_VERIFY(!local.icueJob.isRunning());
        local.acceptIcueAutomation(state);
        QVERIFY(local.icueAutomationState()["control_enabled"].toBool());
        local.setIcueOption("automation", false);
        QVERIFY(!has("automation"));
        QVERIFY(!local.icueAutomationState()["control_enabled"].toBool());
        QCOMPARE(local.settings.value("modules/icue/direct").toBool(), false);
        QCOMPARE(local.settings.value("modules/icue/automationControl").toBool(), true);
    }

    void qmlDisplayControls() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.sampleTimer.stop();
        local.publishTimer.stop();
        local.icueTimer.stop();
        local.stopService();
        local.setDisplaysEnabled(true);
        QTRY_VERIFY(!local.displayJob.isRunning());
        const QString a(24, 'a'), b(24, 'b');
        auto monitor = [](QString id, QString name, int x) {
            return QJsonObject{{"id", id},           {"name", name},    {"group", id},
                               {"connected", true},  {"enabled", true}, {"primary", x == 0},
                               {"mode", "extended"}, {"x", x},          {"y", 0},
                               {"width", 2560},      {"height", 1440},  {"refresh", 144.0},
                               {"rotation", 1}};
        };
        local.displayInfo = {{"available", true},
                             {"status", "2 connected displays"},
                             {"monitors", QJsonArray{monitor(a, "Dell U2724D · aaaaaa", 0),
                                                     monitor(b, "LG UltraGear · bbbbbb", 2560)}}};
        local.displayClock.restart();
        local.mergeReadings("windows", Displays::readings(local.displayInfo), 6);
        local.displayLayouts = {QJsonObject{{"id", "12345678-1234-1234-1234-123456789abc"},
                                            {"name", "Desk"},
                                            {"layout", Displays::layout(local.displayInfo)}}};
        local.setBrowserFilters("Displays", "windows", false, false);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->setPosition(-32000, -32000);
        window->show();
        window->setProperty("previewPage", 1);
        std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == "displayMode_" + a)
                return item;
            for (auto *child : item->childItems())
                if (auto *r = find(child))
                    return r;
            return nullptr;
        };
        QTRY_VERIFY(openBrowserControl(window, local, "display", "monitor", a));
        QQuickItem *choice = nullptr;
        QTRY_VERIFY((window->grabWindow(), choice = find(window->contentItem())) != nullptr);
        QCOMPARE(choice->property("count").toInt(), 3);
        if (!qEnvironmentVariableIsEmpty("WHAT_DISPLAYS_PREVIEW")) {
            QTest::qWait(200);
            window->grabWindow().save(qEnvironmentVariable("WHAT_DISPLAYS_PREVIEW"));
        }
        local.setDisplaysEnabled(false);
        QVERIFY(local.browser().isEmpty());
        window->hide();
    }

    void qmlLightingLinkControls() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        AutomationFixture fixture;
        fixture.lightingMode = 1;
        IcueAutomation api(fixture.endpoint);
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.sampleTimer.stop();
        local.publishTimer.stop();
        local.icueTimer.stop();
        local.setModuleEnabled("icue", true);
        local.setIcueOption("direct", false);
        local.setIcueOption("automation", true);
        QTRY_VERIFY(!local.icueJob.isRunning() && !local.icueAutomationJob.isRunning() && !local.icueReset);
        auto state = api.poll(true);
        local.acceptIcueAutomation(state);
        local.setBrowserFilters("Lighting Link effect", "icue", false, false);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        // Native runners must expose the window so QML polish/layout can run.
        window->setPosition(0, 0);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 1);
        std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == "lightingChoice_effect")
                return item;
            for (auto *child : item->childItems())
                if (auto *result = find(child))
                    return result;
            return nullptr;
        };
        QTRY_VERIFY(openBrowserControl(window, local, "lightingChoice", "lightingParameter", "effect"));
        QQuickItem *choice = nullptr;
        QTRY_VERIFY((window->grabWindow(), choice = find(window->contentItem())) != nullptr);
        QCOMPARE(choice->property("count").toInt(), 4);
        QSignalSpy replies(&local, &Controller::localIcueReply);
        choice->setProperty("currentIndex", 0);
        QVERIFY(QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, 0)));
        QTRY_VERIFY(!replies.isEmpty());
        QVERIFY(replies.last()[1].toBool());
        local.setBrowserFilters("", "icue", false, false);
        QTRY_VERIFY(openBrowserControl(window, local, "rgb", "id", "icue_lighting_control_Color1"));
        QPointer<QQuickItem> editor;
        QTRY_VERIFY((editor = visualItem(window->contentItem(), "inlineColour_icue_lighting_control_Color1")));
        QTRY_VERIFY(editor && editor->isEnabled());
        const int before = replies.size();
        QVERIFY(QMetaObject::invokeMethod(editor, "choose", Q_ARG(QVariant, QVariant("#204060"))));
        QVERIFY(QMetaObject::invokeMethod(editor, "apply"));
        QTRY_VERIFY(replies.size() > before);
        QVERIFY(replies.last()[1].toBool());
        fixture.lightingMode = 0;
        local.acceptIcueAutomation(api.poll(true));
        QTRY_VERIFY(!editor || !editor->isEnabled());
        local.setIcueOption("automation", false);
        QVERIFY(local.icueLightingRow("icue_lighting_control_Color1").isEmpty());
        // Disabling the module rebuilds the browser, including its cached rows.
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        if (auto *incubation = engine.incubationController())
            QTRY_COMPARE_WITH_TIMEOUT(incubation->incubatingObjectCount(), 0, 5000);
        window->hide();
    }

    void qmlAutomationSelectorAndModules() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.sampleTimer.stop();
        local.publishTimer.stop();
        local.icueTimer.stop();
        local.setModuleEnabled("icue", true);
        local.setIcueOption("direct", false);
        local.setIcueOption("automation", true);
        QTRY_VERIFY(!local.icueJob.isRunning() && !local.icueAutomationJob.isRunning() && !local.icueReset);
        QJsonObject control{{"id", "profile"},
                            {"name", "iCUE profile"},
                            {"device", "iCUE"},
                            {"confirmed", true},
                            {"current", "a"},
                            {"options", QJsonArray{QJsonObject{{"id", "a"}, {"name", "Default"}},
                                                   QJsonObject{{"id", "b"}, {"name", "Gaming"}}}}};
        auto update = [&] {
            local.acceptIcueAutomation({{"enabled", true},
                                        {"connected", true},
                                        {"status", "Connected"},
                                        {"controls", QJsonArray{control}}});
        };
        update();
        local.setBrowserFilters("", "icue", false, false);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        // Native runners must expose the window so QML polish/layout can run.
        window->setPosition(0, 0);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 1);
        std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
            if (item->objectName() == "automationPreset_profile")
                return item;
            for (auto *child : item->childItems())
                if (auto *result = find(child))
                    return result;
            return nullptr;
        };
        QTRY_VERIFY(openBrowserControl(window, local, "automation", "automationId", "profile"));
        QQuickItem *selector = nullptr;
        QTRY_VERIFY((window->grabWindow(), selector = find(window->contentItem())) != nullptr);
        QCOMPARE(selector->property("count").toInt(), 2);
        QCOMPARE(selector->property("currentIndex").toInt(), 0);
        QSignalSpy replies(&local, &Controller::localIcueFinished);
        selector->setProperty("currentIndex", 1);
        QVERIFY(QMetaObject::invokeMethod(selector, "activated", Q_ARG(int, 1)));
        QTRY_COMPARE(replies.size(), 1);
        QVERIFY(replies.first()[0].toBool());
        control["current"] = "b";
        update();
        QTRY_COMPARE(selector->property("currentIndex").toInt(), 1);
        control["current"] = "a";
        update();
        QTRY_COMPARE(selector->property("currentIndex").toInt(), 0);
        if (!qEnvironmentVariableIsEmpty("WHAT_AUTOMATION_BROWSER"))
            window->grabWindow().save(qEnvironmentVariable("WHAT_AUTOMATION_BROWSER"));
        window->setProperty("previewPage", 5);
        auto *query = window->findChild<QQuickItem *>("moduleSearch");
        QVERIFY(query);
        query->setProperty("text", "icue");
        auto *icueCard = window->findChild<QQuickItem *>("icueModuleCard");
        QVERIFY(icueCard);
        icueCard->setProperty("collapsed", false);
        auto *direct = window->findChild<QQuickItem *>("icueDirectSubmodule");
        auto *automation = window->findChild<QQuickItem *>("icueAutomationSubmodule");
        QVERIFY(direct);
        QVERIFY(automation);
        QTRY_VERIFY(direct->isVisible());
        QTRY_VERIFY(automation->isVisible());
        automation->setProperty("expanded", true);
        auto *card = window->findChild<QQuickItem *>("icueModuleCard");
        QVERIFY(card);
        // Hidden Windows test windows need explicit frames to advance nested animations/layouts.
        for (int frame = 0; frame < 50; ++frame) {
            window->grabWindow();
            QTest::qWait(20);
        }
        QVERIFY(card->height() >= card->implicitHeight() - 1);
        QVERIFY(automation->height() >= automation->implicitHeight() - 1);
        for (auto *section : {direct, automation}) {
            section->setProperty("expanded", true);
            QTRY_VERIFY(section->property("contentExpanded").toBool());
            auto *sibling = section == direct ? automation : direct;
            QVERIFY(!sibling->property("contentExpanded").toBool());
            QVERIFY(card->property("expanded").toBool());
            auto *button = section->findChild<QQuickItem *>("submoduleExpandButton");
            QVERIFY(button);
            auto *slot = section->findChild<QQuickItem *>("submoduleChevronSlot");
            QVERIFY(slot);
            auto *chevron = section->findChild<QQuickItem *>("submoduleChevron");
            QVERIFY(chevron);
            const auto headerWidth = button->width();
            QCOMPARE(slot->width(), 20.0);
            QCOMPARE(slot->height(), 20.0);
            QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
            for (int frame = 0; frame < 15; ++frame) {
                window->grabWindow();
                QTest::qWait(20);
            }
            QVERIFY(!section->property("contentExpanded").toBool());
            QCOMPARE(section->height(), 56.0);
            QCOMPARE(button->width(), headerWidth);
            QCOMPARE(chevron->width(), 20.0);
            QCOMPARE(chevron->height(), 20.0);
            QCOMPARE(chevron->rotation(), -90.0);
            QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
            for (int frame = 0; frame < 15; ++frame) {
                window->grabWindow();
                QTest::qWait(20);
            }
            QVERIFY(section->property("contentExpanded").toBool());
            QCOMPARE(button->width(), headerWidth);
            QCOMPARE(slot->width(), 20.0);
            QCOMPARE(chevron->rotation(), 0.0);
        }
        if (!qEnvironmentVariableIsEmpty("WHAT_AUTOMATION_MODULES"))
            window->grabWindow().save(qEnvironmentVariable("WHAT_AUTOMATION_MODULES"));
        QVERIFY(QMetaObject::invokeMethod(automation, "toggled", Q_ARG(bool, false)));
        QVERIFY(!local.icueAutomationEnabled());
        QVERIFY(local.browser().isEmpty());
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        if (auto *incubation = engine.incubationController())
            QTRY_COMPARE_WITH_TIMEOUT(incubation->incubatingObjectCount(), 0, 5000);
        window->hide();
    }

    void icueSdkDiscoveryAndControl() {
        IcueSdk sdk(QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(ICUE_TEST_DLL));
        auto data = sdk.poll(true);
        QVERIFY2(data["connected"].toBool(), qPrintable(data["status"].toString()));
        QCOMPARE(data["devices"].toArray().size(), 1);
        QCOMPARE(data["devices"].toArray()[0].toObject()["leds"].toArray()[0].toObject()["name"].toString(),
                 QString("A key"));
        QCOMPARE(data["devices"].toArray()[0].toObject()["leds"].toArray().size(), 2);
        QVERIFY(sdk.command(
                       "icue_colors",
                       {{"device", "test-device"}, {"leds", QJsonArray{22}}, {"rgb", QJsonArray{10, 20, 30}}})
                    .isEmpty());
        data = sdk.poll(true);
        auto leds = data["devices"].toArray()[0].toObject()["leds"].toArray();
        QCOMPARE(leds[0].toObject()["rgb"].toArray(), QJsonArray({1, 2, 3}));
        QCOMPARE(leds[1].toObject()["rgb"].toArray(), QJsonArray({10, 20, 30}));
        QVERIFY(
            !sdk.command("icue_colors",
                         {{"device", "test-device"}, {"leds", QJsonArray{999}}, {"rgb", QJsonArray{0, 0, 0}}})
                 .isEmpty());
        QVERIFY(!IcueSdk::validate("icue_colors", {{"device", "test-device"}, {"rgb", QJsonArray{256, 0, 0}}})
                     .isEmpty());
        QVERIFY(
            !IcueSdk::validate("icue_property", {{"device", "test-device"}, {"property", 9}, {"value", 1}})
                 .isEmpty());
        QVERIFY(sdk.command("icue_property", {{"device", "test-device"}, {"property", 2}, {"value", false}})
                    .isEmpty());
        QCOMPARE(sdk.poll(true)["devices"]
                     .toArray()[0]
                     .toObject()["properties"]
                     .toArray()[0]
                     .toObject()["value"]
                     .toBool(),
                 false);
        QVERIFY(sdk.command("icue_release", {{"device", "test-device"}}).isEmpty());
        QVERIFY(!sdk.poll(false)["connected"].toBool());
        QVERIFY(!sdk.command("icue_release", {{"device", "test-device"}}).isEmpty());
    }

    void icueModuleHidesReadingsIndependently() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        IcueSdk sdk(QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(ICUE_TEST_DLL));
        c->setModuleEnabled("icue", true);
        QTRY_VERIFY(!c->icueJob.isRunning());
        c->acceptIcue(sdk.poll(true));
        c->setBrowserFilters("", "icue", false, false);
        QVERIFY(!c->browser().isEmpty());
        c->setModuleEnabled("windows", false);
        QVERIFY(!c->browser().isEmpty());
        c->setModuleEnabled("icue", false);
        QVERIFY(c->browser().isEmpty());
        QVERIFY(!c->snapshot()["icue"].toObject()["control_enabled"].toBool());
        c->setModuleEnabled("windows", true);
        c->setBrowserFilters("", "all", false, false);
    }

    void localIcueControls() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.sampleTimer.stop();
        local.publishTimer.stop();
        local.icueTimer.stop();
        local.setModuleEnabled("icue", true);
        QTRY_VERIFY(!local.icueJob.isRunning());
        IcueSdk sdk(QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(ICUE_TEST_DLL));
        local.acceptIcue(sdk.poll(true));
        local.setBrowserFilters("", "icue", false, false);
        int colors = 0, toggles = 0;
        for (auto value : local.icueBrowserRows(local.sensors())) {
            auto row = value.toMap();
            if (row["control"] == "rgb") {
                ++colors;
                QVERIFY(row["controlOnly"].toBool());
                QVERIFY(!row["enabled"].toBool());
            }
            if (row["control"] == "toggle")
                ++toggles;
        }
        QCOMPARE(colors, 3);
        QVERIFY(toggles > 0);
        for (auto value : local.allReadings())
            QVERIFY(!value.toObject().contains("controlOnly"));
        QVERIFY(!local.settings.contains("icue/led_groups"));
        QSignalSpy finished(&local, &Controller::localIcueFinished);
        local.localIcueCommand("icue_colors", {{"device", "test-device"}, {"rgb", QVariantList{256, 0, 0}}});
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.takeFirst()[0].toBool());
        local.localIcueCommand("icue_colors", {{"device", "test-device"}, {"rgb", QVariantList{10, 20, 30}}});
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.takeFirst()[0].toBool());
        QVERIFY(!local.settings.value("modules/icue/control", false).toBool());
        local.setModuleEnabled("icue", false);
        local.localIcueCommand("icue_release", {{"device", "test-device"}});
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.takeFirst()[0].toBool());
        QVERIFY(local.browser().isEmpty());
    }

    void savedColoursPersist() {
        QTemporaryDir dir;
        const auto file = dir.filePath("settings.yaml");
        {
            QSettings settings(file, SettingsStore::format());
            ColourPalette palette(settings);
            palette.remember(QColor("#33AAFF"));
            palette.remember(QColor("#ff33aaff"));
            QCOMPARE(palette.recent(), QStringList{"#33aaff"});
            palette.remember(QColor("#80445566"));
            palette.remember(QColor());
            QCOMPARE(palette.recent(), (QStringList{"#80445566", "#33aaff"}));
            palette.setFavourite(QColor("#80445566"), true);
            palette.setFavourite(QColor("#33aaff"), true);
            palette.setFavourite(QColor("#ff33aaff"), true);
            QCOMPARE(palette.favourites().size(), 2);
            palette.setFavourite(QColor("#33aaff"), false);
            for (int i = 0; i < 20; ++i)
                palette.remember(QColor(i, 0, 0));
            QCOMPARE(palette.recent().size(), 16);
            QCOMPARE(palette.recent().first(), QString("#130000"));
            QCOMPARE(palette.recent().last(), QString("#040000"));
            const auto exported = ConfigurationFile::snapshot(settings);
            QVERIFY(exported.contains("colours/recent"));
            QCOMPARE(exported["colours/favourites"].toArray(), QJsonArray{"#80445566"});
            QVERIFY(ConfigurationFile::decode(ConfigurationFile::encode(exported)).error.isEmpty());
            settings.sync();
            QCOMPARE(settings.status(), QSettings::NoError);
        }
        QSettings reopened(file, SettingsStore::format());
        ColourPalette restored(reopened);
        QCOMPARE(restored.recent().size(), 16);
        QCOMPARE(restored.favourites(), QStringList{"#80445566"});
        restored.setFavourite(QColor("#80445566"), false);
        QVERIFY(restored.favourites().isEmpty());
        for (int i = 0; i < 33; ++i)
            restored.setFavourite(QColor(0, i, 0), true);
        QCOMPARE(restored.favourites().size(), 32);
        QCOMPARE(restored.favourites().last(), QString("#001f00"));
    }

    void savedColourDialog() {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("settings.yaml"), SettingsStore::format());
        ColourPalette palette(settings);
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] { dialog->reject(); });
            dialog->setCurrentColor(QColor("#80445566"));
            auto *favourite = dialog->findChild<QPushButton *>("favouriteColour");
            QVERIFY(favourite);
            favourite->click();
            QCOMPARE(palette.favourites(), QStringList{"#80445566"});
            if (!qEnvironmentVariableIsEmpty("WHAT_COLOUR_PREVIEW")) {
                QTest::qWait(80);
                dialog->grab().save(qEnvironmentVariable("WHAT_COLOUR_PREVIEW"));
            }
            dialog->reject();
        });
        QVERIFY(!palette.choose(Qt::white, "Icon Colour", true).isValid());
        QVERIFY(palette.recent().isEmpty()); // Cancelling does not apply or record the draft.
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            bool chosen = false;
            for (auto *button : dialog->findChildren<QToolButton *>()) {
                if (button->accessibleName() == "Choose #80445566") {
                    button->click();
                    chosen = true;
                    break;
                }
            }
            QVERIFY(chosen);
            dialog->accept();
        });
        QCOMPARE(palette.choose(Qt::white, "Icon Colour", true), QColor("#80445566"));
        QCOMPARE(palette.recent(), QStringList{"#80445566"});
        SensorTrayManager tray(settings.fileName(), true);
        tray.setColor("main", true, QColor("#80445566"));
        QCOMPARE(tray.entries().first().toMap()["background"].toString(), QString("#80445566"));
        tray.taskbarIcons()->setColor("main", false, QColor("#80445566"));
        QCOMPARE(tray.taskbarIcons()->entries().first().toMap()["foreground"].toString(), QString("#80445566"));
        tray.taskbarIcons()->setColor("main", false, QColor());
        QCOMPARE(tray.taskbarIcons()->entries().first().toMap()["foreground"].toString(), QString("#80445566"));
    }

    void shellStreamsBeforeCompletion() {
        QString streamed;
        auto result = ShellCommand::run(
            {{"shell", "powershell"},
             {"command", "[Console]::WriteLine('early'); Start-Sleep 5; [Console]::WriteLine('late')"},
             {"timeout", 10}},
            [&] { return streamed.contains("early"); }, [&](bool, const QString &text) { streamed += text; });
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        QVERIFY(streamed.contains("early"));
        QVERIFY(!streamed.contains("late"));
        QVERIFY(result["cancelled"].toBool());
    }

    void windowsSecurityHealthAndPermissions() {
        static int health = 0;
        static HRESULT result = S_OK;
        auto query = +[](DWORD, int *value) -> HRESULT {
            *value = health;
            return result;
        };
        const QStringList states{"Good", "Not monitored", "Poor", "Snoozed"};
        for (health = 0; health < 4; ++health) {
            const auto rows = WindowsSecurity::read(query);
            QCOMPARE(rows.size(), 3);
            QCOMPARE(rows.first().toObject()["value"].toString(), states[health]);
            QVERIFY(rows.first().toObject()["available"].toBool());
        }
        health = 2;
        result = S_FALSE; // Service stopped is unknown, not an actual poor-health report.
        auto unavailable = WindowsSecurity::read(query).first().toObject();
        QVERIFY(unavailable["value"].isNull());
        QVERIFY(!unavailable["available"].toBool());
        QVERIFY(!WindowsSecurity::read(nullptr).first().toObject()["available"].toBool());
        result = S_OK;
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        const auto reading = WindowsSecurity::read(query).first().toObject();
        QVERIFY(!local.sourceOptions()["security"].toBool());
        QVERIFY(!local.readingBrowserEnabled(reading));
        local.saveSources({{"security", true}});
        QVERIFY(local.readingBrowserEnabled(reading));
        local.mergeReadings("windows", {reading}, 2);
        local.mergeReadings("windows", {}, 1);
        QVERIFY(local.catalog[reading["id"].toString()]["available"].toBool());
        local.mergeReadings("windows", {}, 2);
        QVERIFY(!local.catalog[reading["id"].toString()]["available"].toBool());
        local.saveSources({{"security", false}});
        QVERIFY(!local.readingBrowserEnabled(reading));
    }

    void windowsSubmodulePolicyAndStreamBound() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        QVERIFY(!local.controlPermissions()["shell"].toBool());
        local.setControlPermission("media", true);
        QVERIFY(local.controlPermissions()["media"].toBool());
        const auto epoch = local.commandEpoch->load();
        local.saveSources({{"media", false}});
        QVERIFY(!local.controlPermissions()["media"].toBool());
        QVERIFY(local.commandEpoch->load() > epoch);
        QVERIFY(!local.readingBrowserEnabled({{"id", "win_media_title"}, {"source", "windows"}}));
        local.appendCommandStream(QString(100000, 'x'));
        QCOMPARE(local.commandStream().size(), 65536);
        local.clearCommandStream();
        QVERIFY(local.commandStream().isEmpty());
    }

    void icueDeviceSubtreesAndLabels() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.icueTimer.stop();
        local.setModuleEnabled("icue", true);
        QTRY_VERIFY(!local.icueJob.isRunning());
        IcueSdk sdk(QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(ICUE_TEST_DLL));
        auto data = sdk.poll(true);
        auto devices = data["devices"].toArray();
        auto second = devices[0].toObject();
        second["id"] = "another-device";
        devices.append(second);
        data["devices"] = devices;
        local.acceptIcue(data);
        local.setBrowserFilters("", "icue", false, false);
        local.collapseCategories(false);
        QString root;
        int headers = 0, leds = 0;
        for (auto value : local.browser()) {
            auto row = value.toMap();
            if (row["header"].toBool()) {
                ++headers;
                if (!row["depth"].toInt())
                    root = row["category"].toString();
            }
            if (row.contains("icueLed")) {
                ++leds;
                QVERIFY(row["name"].toString().contains("Fixture keyboard"));
                QVERIFY(row["tooltip"].toString().contains("SDK LED ID:"));
                QCOMPARE(row["depth"].toInt(), 1);
            }
        }
        QCOMPARE(headers, 4);
        QCOMPARE(leds, 4);
        QVERIFY(!root.isEmpty());
        local.setCategoryCollapsed(root, true);
        for (auto value : local.browser()) {
            auto row = value.toMap();
            QVERIFY(row["category"].toString() != root + "/leds");
        }
        local.setBrowserFilters("LED 2", "icue", false, false);
        headers = 0;
        leds = 0;
        for (auto value : local.browser()) {
            auto row = value.toMap();
            if (row["header"].toBool())
                ++headers;
            if (row.contains("icueLed")) {
                ++leds;
                QCOMPARE(row["icueLed"].toULongLong(), qulonglong(22));
            }
        }
        // Search keeps the user's collapsed device closed.
        QCOMPARE(headers, 3);
        QCOMPARE(leds, 1);
        local.setCategoryCollapsed(root, false);
        headers = leds = 0;
        for (const auto &value : local.browser()) {
            const auto shown = value.toMap();
            headers += shown.value("header").toBool();
            leds += shown.contains("icueLed");
        }
        QCOMPARE(headers, 4);
        QCOMPARE(leds, 2);
    }

    void icueHubChannelDevices() {
        if (Architecture::arm64)
            QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
        QLibrary library(QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(ICUE_TEST_DLL));
        QVERIFY(library.load());
        auto mode = reinterpret_cast<void (*)(int)>(library.resolve("IcueFixtureHub"));
        QVERIFY(mode);
        mode(1);
        auto reset = qScopeGuard([&] { mode(0); });
        IcueSdk sdk(QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(ICUE_TEST_DLL));
        auto data = sdk.poll(true);
        auto leds = data["devices"].toArray()[0].toObject()["leds"].toArray();
        QCOMPARE(leds.size(), 5);
        QCOMPARE(leds[0].toObject()["segment_name"].toString(), QString("Channel 1 · QX fan 1"));
        QCOMPARE(leds[2].toObject()["segment_name"].toString(), QString("Channel 1 · Pump 2"));
        QCOMPARE(leds[4].toObject()["segment_name"].toString(), QString("Channel 2 · LED strip 1"));
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.icueTimer.stop();
        local.setModuleEnabled("icue", true);
        QTRY_VERIFY(!local.icueJob.isRunning());
        local.acceptIcue(data);
        local.setBrowserFilters("", "icue", false, false);
        local.collapseCategories(false);
        int subsets = 0, individual = 0;
        QSet<quint32> ids;
        for (auto value : local.browser()) {
            auto row = value.toMap();
            if (row.contains("icueLeds")) {
                ++subsets;
                for (auto led : row["icueLeds"].toList())
                    ids.insert(led.toUInt());
                QCOMPARE(row["depth"].toInt(), 1);
            }
            if (row.contains("icueLed")) {
                ++individual;
                QCOMPARE(row["depth"].toInt(), 2);
            }
        }
        QCOMPARE(subsets, 3);
        QCOMPARE(individual, 5);
        QCOMPARE(ids.size(), 5);
        local.setBrowserFilters("Pump", "icue", false, false);
        int ancestors = 0;
        for (auto value : local.browser())
            if (value.toMap()["header"].toBool())
                ++ancestors;
        QCOMPARE(ancestors, 3);
        mode(2);
        data = sdk.poll(true);
        leds = data["devices"].toArray()[0].toObject()["leds"].toArray();
        QVERIFY(!leds[0].toObject().contains("segment"));
        QVERIFY(leds[4].toObject().contains("segment"));
    }

    void shellValidation() {
        const QJsonObject valid{{"shell", "cmd"}, {"command", "echo hello"}, {"timeout", 30}};
        QVERIFY(ShellCommand::validate(valid).isEmpty());
        for (const auto &bad :
             {QJsonObject{{"shell", "other"}}, QJsonObject{{"timeout", 0}}, QJsonObject{{"timeout", 301}},
              QJsonObject{{"timeout", 1.5}}, QJsonObject{{"command", ""}},
              QJsonObject{{"distribution", "Ubuntu"}}, QJsonObject{{"extra", true}}}) {
            auto args = valid;
            for (auto it = bad.begin(); it != bad.end(); ++it)
                args[it.key()] = it.value();
            QVERIFY(!ShellCommand::validate(args).isEmpty());
        }
        auto wsl = valid;
        wsl["shell"] = "wsl";
        wsl["distribution"] = "Ubuntu";
        QVERIFY(ShellCommand::validate(wsl).isEmpty());
    }

    void shellLeaveRunning() {
        for (const bool leave : {false, true}) {
            // A hidden, uniquely identified child is terminated by this test on every path.
            const auto result = ShellCommand::run({{"shell", "powershell"}, {"timeout", 10},
                {"leave_running", leave},
                {"command", "$p = Start-Process powershell.exe -WindowStyle Hidden -PassThru "
                            "-ArgumentList '-NoProfile -Command Start-Sleep 30'; $p.Id"}}, [] { return false; });
            QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
            QCOMPARE(result["exit_code"].toInt(), 0);
            const auto pid = result["stdout"].toString().trimmed().toUInt();
            QVERIFY(pid > 0);
            HANDLE child = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
            const auto cleanup = qScopeGuard([child] {
                if (child) {
                    TerminateProcess(child, 0);
                    WaitForSingleObject(child, 2000);
                    CloseHandle(child);
                }
            });
            if (leave) {
                QVERIFY(child);
                QCOMPARE(WaitForSingleObject(child, 0), DWORD(WAIT_TIMEOUT));
            } else if (child) {
                QCOMPARE(WaitForSingleObject(child, 2000), DWORD(WAIT_OBJECT_0));
            }
        }
        const auto cancelled = ShellCommand::run({{"shell", "powershell"},
            {"command", "Start-Sleep 10"}, {"timeout", 1}, {"leave_running", true}}, [] { return false; });
        QVERIFY(cancelled["timed_out"].toBool());
        QVERIFY(!ShellCommand::validate({{"shell", "wsl"}, {"command", "true"},
            {"timeout", 10}, {"leave_running", true}}).isEmpty());
        QVERIFY(!ShellCommand::validate({{"shell", "cmd"}, {"command", "echo test"},
            {"timeout", 10}, {"leave_running", "yes"}}).isEmpty());
    }

    void relativeCommandDeadlines() {
        NetworkWorker worker;
        worker.initialize();
        worker.stopped = false;
        worker.token = "test-token";
        auto *peer = new WirePeer(true, &worker);
        QList<QJsonObject> sent;
        peer->send = [&](QString text) {
            sent.append(QJsonDocument::fromJson(text.toUtf8()).object());
            return qint64(text.size());
        };
        worker.attachPeer(peer);
        const auto receive = [&](QJsonObject message) {
            emit peer->textMessageReceived(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
        };
        receive({{"type", "auth"}, {"protocol", 2}, {"token", worker.token},
                 {"commands_v1", true}, {"command_deadline_v1", true}});
        QVERIFY(sent.first()["command_deadline_v1"].toBool());
        worker.setSnapshot(c->snapshot());
        worker.publish();
        QVERIFY(sent.last().contains("session_ms"));
        QVERIFY(sent.last()["session_ms"].toDouble() >= 0);
        QVERIFY(sent.last()["session_ms"].toDouble() <= worker.commandClients[peer].clock.elapsed());
        QVERIFY(qAbs(sent.last()["sent_at"].toDouble() - QDateTime::currentMSecsSinceEpoch() / 1000.0) < 1);
        QSignalSpy commands(&worker, &NetworkWorker::commandRequested);
        const auto command = [&](QString id, double deadline) {
            receive({{"type", "command"}, {"id", id}, {"action", "volume"},
                     {"args", QJsonObject{{"value", .5}}}, {"deadline_ms", deadline},
                     {"expires_at", 1}}); // UTC is deliberately expired and must not be used.
        };
        command("valid", 12000);
        QCOMPARE(commands.size(), 1);
        command("valid", 12000);
        QVERIFY(sent.last()["message"].toString().contains("Duplicate"));
        command("expired", -1);
        QVERIFY(sent.last()["message"].toString().contains("deadline"));
        command("future", 100000);
        QVERIFY(sent.last()["message"].toString().contains("deadline"));
        QCOMPARE(commands.size(), 1);
        worker.stop();
    }

    void shellCapturesOutputAndExit() {
        auto result = ShellCommand::run(
            {{"shell", "cmd"}, {"command", "echo hello\necho problem 1>&2\nexit /b 7"}, {"timeout", 10}},
            [] { return false; });
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        QVERIFY2(result["stdout"].toString().contains("hello"), QJsonDocument(result).toJson().constData());
        QVERIFY(result["stderr"].toString().contains("problem"));
        QCOMPARE(result["exit_code"].toInt(), 7);
        result = ShellCommand::run({{"shell", "powershell"},
                                    {"command", "[Console]::WriteLine('héllo \"world\"'); "
                                                "[Console]::Error.WriteLine('test error'); exit 3"},
                                    {"timeout", 10}},
                                   [] { return false; });
        QVERIFY2(!result.contains("error"), qPrintable(result["error"].toString()));
        QVERIFY2(result["stdout"].toString().contains("héllo \"world\""),
                 qPrintable(result["stdout"].toString()));
        QVERIFY(result["stderr"].toString().contains("test error"));
        QCOMPARE(result["exit_code"].toInt(), 3);
    }

    void shellTimeoutCancellationAndOutputBound() {
        auto result = ShellCommand::run(
            {{"shell", "powershell"}, {"command", "Start-Sleep 10"}, {"timeout", 1}}, [] { return false; });
        QVERIFY(result["timed_out"].toBool());
        QElapsedTimer elapsed;
        elapsed.start();
        result = ShellCommand::run({{"shell", "powershell"}, {"command", "Start-Sleep 10"}, {"timeout", 20}},
                                   [&] { return elapsed.elapsed() > 400; });
        QVERIFY(result["cancelled"].toBool());
        QVERIFY(elapsed.elapsed() < 3000);
        result = ShellCommand::run(
            {{"shell", "powershell"},
             {"command", "[Console]::Write(('x' * 100000)); [Console]::Error.Write(('y' * 100000))"},
             {"timeout", 10}},
            [] { return false; });
        QCOMPARE(result["stdout"].toString().size(), 65536);
        QCOMPARE(result["stderr"].toString().size(), 65536);
        QVERIFY(result["truncated"].toBool());
    }

    void windowsCommandValidation() {
        for (const auto &action : {"play", "pause", "stop", "next", "previous", "sleep", "hibernate",
                                   "shutdown", "reboot", "lock", "monitor_sleep"})
            QVERIFY(WindowsCommands::validate(action, {}).isEmpty());
        QVERIFY(WindowsCommands::validate("volume", {{"value", 0.5}}).isEmpty());
        QVERIFY(!WindowsCommands::validate("volume", {{"value", 2}}).isEmpty());
        QVERIFY(!WindowsCommands::validate("mute", {{"value", 1}}).isEmpty());
        QVERIFY(!WindowsCommands::validate("shutdown", {{"force", true}}).isEmpty());
        QVERIFY(!WindowsCommands::validate("run", {{"value", "cmd.exe"}}).isEmpty());
        for (const auto &url : {"file:///C:/Windows/notepad.exe", "javascript:alert(1)",
                                "https://user:secret@example.org", "https://example.org\n"})
            QVERIFY(!WindowsCommands::validate("open_url", {{"value", url}}).isEmpty());
        QVERIFY(WindowsCommands::validate("open_url", {{"value", "https://example.org/path?a=b"}}).isEmpty());
        QVERIFY(!WindowsCommands::validate("seek", {{"value", -1}}).isEmpty());
    }

    void haFeedbackIsNegotiatedAndRejectsStaleReplies() {
        NetworkWorker worker;
        worker.initialize();
        QSignalSpy trace(&worker, &NetworkWorker::diagnostic);
        worker.setDiagnostics(true);
        worker.stopped = false;
        worker.token = "fixture-token";
        worker.device = "fixture-pc";
        auto *peer = new WirePeer(false, &worker);
        QList<QJsonObject> sent;
        peer->send = [&](const QString &text) {
            sent.append(QJsonDocument::fromJson(text.toUtf8()).object());
            return text.size();
        };
        worker.attachPeer(peer);
        const auto receive = [&](QJsonObject message) {
            emit peer->textMessageReceived(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
        };
        receive({{"type", "auth"}, {"protocol", 2}, {"token", worker.token}, {"ui_feedback_v1", true}});
        QVERIFY(sent.first()["ui_feedback_v1"].toBool());
        QVERIFY(trace.last()[0].toString().contains("Home Assistant connected"));
        bool negotiated = false;
        for (const auto &line : trace)
            negotiated |= line[0].toString().contains("protocol authentication accepted");
        QVERIFY(negotiated);
        QSignalSpy activity(&worker, &NetworkWorker::haCommunication);
        QSignalSpy results(&worker, &NetworkWorker::entityLookupResult);
        worker.setEntityLookup("first", {"first"});
        worker.probeHomeAssistant();
        const auto firstProbe = sent.last();
        QCOMPARE(firstProbe["type"].toString(), QString("ui_probe"));
        QCOMPARE(firstProbe["keys"].toArray(), QJsonArray{"first"});
        const auto pendingCount = sent.size();
        worker.probeHomeAssistant();
        QCOMPARE(sent.size(), pendingCount); // No accumulating probes during a slow reply.
        worker.setEntityLookup("second", {"second"});
        worker.probeHomeAssistant();
        const auto secondProbe = sent.last();
        receive({{"type", "ui_feedback"}, {"id", firstProbe["id"]}, {"entities", QJsonArray{"sensor.old"}}});
        QVERIFY(activity.isEmpty());
        QVERIFY(results.isEmpty());
        receive({{"type", "ui_feedback"}, {"id", secondProbe["id"]}, {"entities", QJsonArray{"number.renamed_by_user"}}});
        QCOMPARE(activity.count(), 1);
        QCOMPARE(worker.mqtt->traffic.feedback, quint64(1));
        QVERIFY(worker.mqtt->traffic.lastRoundTripMs >= 0);
        QCOMPARE(results.first()[0].toString(), QString("second"));
        QCOMPARE(results.first()[1].toStringList(), QStringList{"number.renamed_by_user"});
        receive({{"type", "ui_feedback"}, {"id", secondProbe["id"]}, {"entities", QJsonArray{}}});
        QCOMPARE(activity.count(), 1); // Replayed responses cannot keep the ECG alive.
        worker.probeHomeAssistant();
        receive({{"type", "ui_feedback"}, {"id", sent.last()["id"]}, {"entities", QJsonArray{"<script>"}}});
        QVERIFY(peer->closed);
        QVERIFY(worker.feedbackPeers.isEmpty());

        auto *legacy = new WirePeer(false, &worker);
        legacy->send = [&](const QString &text) { sent.append(QJsonDocument::fromJson(text.toUtf8()).object()); return text.size(); };
        worker.attachPeer(legacy);
        emit legacy->textMessageReceived(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "auth"}, {"protocol", 2}, {"token", worker.token}}).toJson(QJsonDocument::Compact)));
        const auto legacyCount = sent.size();
        worker.probeHomeAssistant();
        QCOMPARE(sent.size(), legacyCount);
        QVERIFY(!sent.last()["ui_feedback_v1"].toBool());
        worker.stop();
        worker.setDiagnostics(false);
#ifdef QT_NO_DEBUG
        QVERIFY(!worker.diagnosticsEnabled);
        QVERIFY(!worker.mqtt->diagnosticsEnabled);
#else
        QVERIFY(worker.diagnosticsEnabled);
        QVERIFY(worker.mqtt->diagnosticsEnabled);
#endif
    }

    void haFeedbackSendsWhenTelemetryDrains() {
        NetworkWorker worker;
        worker.initialize();
        worker.stopped = false;
        worker.token = "fixture-token";
        worker.device = "fixture-pc";
        auto *peer = new WirePeer(false, &worker);
        QList<QJsonObject> sent;
        qint64 pendingBytes = 0;
        peer->pendingWrites = [&] { return pendingBytes; };
        peer->send = [&](const QString &text) {
            sent.append(QJsonDocument::fromJson(text.toUtf8()).object());
            return text.size();
        };
        worker.attachPeer(peer);
        emit peer->textMessageReceived(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "auth"}, {"protocol", 2}, {"token", worker.token}, {"ui_feedback_v1", true}
        }).toJson(QJsonDocument::Compact)));
        sent.clear();

        pendingBytes = 100;
        worker.probeHomeAssistant();
        emit peer->bytesWritten(50);
        QVERIFY(sent.isEmpty());
        pendingBytes = 0;
        emit peer->bytesWritten(50);
        QCOMPARE(sent.size(), 1); // Send now, without waiting another five seconds.
        QCOMPARE(sent.first()["type"].toString(), QString("ui_probe"));
        emit peer->bytesWritten(100);
        QCOMPARE(sent.size(), 1); // Writing the probe itself must not trigger another.
        QSignalSpy activity(&worker, &NetworkWorker::haCommunication);
        emit peer->textMessageReceived(QString::fromUtf8(QJsonDocument(QJsonObject{
            {"type", "ui_feedback"}, {"id", sent.first()["id"]}, {"entities", QJsonArray{}}
        }).toJson(QJsonDocument::Compact)));
        QCOMPARE(activity.count(), 1);
        emit peer->bytesWritten(0);
        QCOMPARE(sent.size(), 1);
        worker.stop();
    }

    void haEntityCopyUsesConfirmedRegistryId() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)}});
        engine.loadData(R"(
            import QtQuick
            import QtQuick.Controls.Basic
            import "qrc:/qml/sensors"
            ApplicationWindow {
                id: window
                required property var controller
                width: 640; height: 180; visible: true
                HaEntityDetails {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    controller: window.controller
                    row: ({ id: "fixture" })
                }
            }
        )");
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        emit local.server.entityLookupResult("fixture", {"sensor.my_actual_renamed_temperature"}, "");
        QTRY_VERIFY(visualItem(window->contentItem(), "haEntityCopy"));
        auto *copy = visualItem(window->contentItem(), "haEntityCopy");
        auto *entityText = visualItem(copy, "haEntityIdText");
        auto *copyLabel = visualItem(copy, "haEntityCopyLabel");
        QVERIFY(entityText);
        QVERIFY(copyLabel);
        // The confirmation must keep its own right-hand space even when the ID wraps.
        for (int width : {280, 1280}) {
            window->setWidth(width);
            QTRY_COMPARE(copy->width(), qreal(width));
            QTRY_VERIFY(entityText->x() + entityText->width() < copyLabel->x());
            QTRY_COMPARE(copyLabel->x() + copyLabel->width(), copyLabel->parentItem()->width());
            QTRY_VERIFY(copy->height() >= entityText->implicitHeight()
                + copy->property("topPadding").toReal() + copy->property("bottomPadding").toReal());
        }
        const qreal labelWidth = copyLabel->width();
        const qreal labelX = copyLabel->x();
        QVERIFY(writeClipboardWithRetry([&] { QMetaObject::invokeMethod(copy, "clicked"); }));
        QCOMPARE(QApplication::clipboard()->text(), QString("sensor.my_actual_renamed_temperature"));
        QVERIFY(copy->property("copied").toBool());
        QCOMPARE(copyLabel->property("text").toString(), QString::fromUtf8("✓ Copied"));
        QCOMPARE(copyLabel->width(), labelWidth);
        QCOMPARE(copyLabel->x(), labelX);
        QVERIFY(!local.copyHaEntity("sensor.not_in_the_reply"));
        window->hide();
    }

    void authenticatedCommandsEnforcePermissionsAndDuplicates() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.sampleTimer.stop();
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const int port = probe.serverPort();
        probe.close();
        local.saveSettings(1, 1, port, "pro", false);
        QWebSocket socket;
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        socket.setSslConfiguration(ssl);
        QSignalSpy messages(&socket, &QWebSocket::textMessageReceived);
        auto authenticate = [&] {
            socket.abort();
            // Permission changes may discard buffered frames and close the old connection.
            // Wait for the worker's policy barrier, then authenticate a fresh session.
            QMetaObject::invokeMethod(local.server.worker, [] {}, Qt::BlockingQueuedConnection);
            messages.clear();
            socket.open(QUrl(QString("wss://127.0.0.1:%1").arg(port)));
            if (!QTest::qWaitFor([&] { return socket.state() == QAbstractSocket::ConnectedState; }, 5000))
                return false;
            socket.sendTextMessage(QJsonDocument(QJsonObject{{"type", "auth"},
                                                             {"protocol", 2},
                                                             {"token", local.token},
                                                             {"commands_v1", true}})
                                       .toJson(QJsonDocument::Compact));
            return QTest::qWaitFor(
                [&] {
                    bool hello = false, snapshot = false;
                    for (const auto &entry : messages) {
                        const auto frame = QJsonDocument::fromJson(entry[0].toString().toUtf8()).object();
                        hello |= frame["type"] == "hello";
                        snapshot |= frame["type"] == "snapshot";
                    }
                    return hello && snapshot;
                },
                5000);
        };
        QVERIFY(authenticate());
        auto send = [&](QString id, QString action, QJsonObject args) {
            socket.sendTextMessage(
                QJsonDocument(
                    QJsonObject{{"type", "command"}, {"id", id}, {"action", action}, {"args", args}})
                    .toJson(QJsonDocument::Compact));
        };
        auto result = [&](QString id) {
            for (const auto &entry : messages) {
                auto value = QJsonDocument::fromJson(entry[0].toString().toUtf8()).object();
                if (value["type"] == "command_result" && value["id"] == id)
                    return value;
            }
            return QJsonObject{};
        };
        send("blocked", "volume", {{"value", 0.5}});
        QTRY_VERIFY(!result("blocked").isEmpty());
        QVERIFY(!result("blocked")["ok"].toBool());
        local.setControlPermission("media", true);
        QTRY_VERIFY(!local.commandJob.isRunning());
        QVERIFY(authenticate());
        send("allowed", "volume", {{"value", 0.5}});
        QTRY_VERIFY(!result("allowed").isEmpty());
        QVERIFY(result("allowed")["ok"].toBool());
        QVERIFY(local.logs().contains("test mode; no OS action"));
        messages.clear();
        send("allowed", "volume", {{"value", 0.8}});
        QTRY_VERIFY(!result("allowed").isEmpty());
        QVERIFY(!result("allowed")["ok"].toBool());
        send("badvalue", "volume", {{"value", -1}});
        QTRY_VERIFY(!result("badvalue").isEmpty());
        QVERIFY(!result("badvalue")["ok"].toBool());
        const QJsonObject shellArgs{{"shell", "cmd"}, {"command", "echo sensitive-fixture"}, {"timeout", 30}};
        send("shell-denied", "run_command", shellArgs);
        QTRY_VERIFY(!result("shell-denied").isEmpty());
        QVERIFY(!result("shell-denied")["ok"].toBool());
        local.setControlPermission("shell", true);
        QVERIFY(authenticate());
        send("shell-allowed", "run_command", shellArgs);
        QTRY_VERIFY(!result("shell-allowed").isEmpty());
        QVERIFY(result("shell-allowed")["ok"].toBool());
        QVERIFY(result("shell-allowed")["result"].toObject().contains("stdout"));
        QCOMPARE(result("shell-allowed")["result"].toObject().size(), 6);
        QVERIFY(!local.logs().contains("sensitive-fixture"));
        local.setModuleEnabled("windows", false);
        QVERIFY(authenticate());
        send("disabled", "play", {});
        QTRY_VERIFY(!result("disabled").isEmpty());
        QVERIFY(!result("disabled")["ok"].toBool());

        // App controls have their own opt-in and continue working with Windows disabled.
        send("app-denied", "app_logging", {{"value", true}});
        QTRY_VERIFY(!result("app-denied").isEmpty());
        QVERIFY(!result("app-denied")["ok"].toBool());
        QVERIFY(!local.settings.value("diagnostics/logging", false).toBool());
        local.setAppControlEnabled(true);
        QVERIFY(authenticate());
        send("app-invalid", "app_logging", {{"value", "true"}});
        QTRY_VERIFY(!result("app-invalid").isEmpty());
        QVERIFY(!result("app-invalid")["ok"].toBool());
        send("app-log", "app_logging", {{"value", true}});
        QTRY_VERIFY(!result("app-log").isEmpty());
        QVERIFY(result("app-log")["ok"].toBool());
        QVERIFY(local.appState()["logging"].toBool());
        QVERIFY(!local.settings.value("diagnostics/dumps", false).toBool());
        send("app-rate-invalid", "app_publish_interval", {{"value", 99}});
        QTRY_VERIFY(!result("app-rate-invalid").isEmpty());
        QVERIFY(!result("app-rate-invalid")["ok"].toBool());
        send("app-rate", "app_publish_interval", {{"value", 2345}});
        QTRY_VERIFY(!result("app-rate").isEmpty());
        QVERIFY(result("app-rate")["ok"].toBool());
        QCOMPARE(local.appState()["publishing_interval_ms"].toInt(), 2345);
        QCOMPARE(local.publishTimer.interval(), 1000); // The heartbeat is unchanged.
        local.setPublishingIntervals(3000, 2000);
        QVERIFY(authenticate());
        send("app-rate-below-high", "app_publish_interval", {{"value", 1500}});
        QTRY_VERIFY(!result("app-rate-below-high").isEmpty());
        QVERIFY(!result("app-rate-below-high")["ok"].toBool());
        send("app-rate-fraction", "app_publish_interval", {{"value", 2345.5}});
        QTRY_VERIFY(!result("app-rate-fraction").isEmpty());
        QVERIFY(!result("app-rate-fraction")["ok"].toBool());
        bool launched = false;
        local.restartLauncher = [&] { launched = true; return QString(); };
        local.restartBlocked = [] { return true; };
        send("app-restart-busy", "app_restart", {});
        QTRY_VERIFY(!result("app-restart-busy").isEmpty());
        QVERIFY(!result("app-restart-busy")["ok"].toBool());
        local.restartBlocked = [] { return false; };
        QTRY_VERIFY(!local.appJob.isRunning());
        send("app-restart", "app_restart", {});
        QTRY_VERIFY(!result("app-restart").isEmpty());
        QVERIFY(result("app-restart")["ok"].toBool());
        QVERIFY(result("app-restart")["message"].toString().contains("test mode"));
        QVERIFY(!launched); // No real process restart in the command test.
        send("app-restart-injection", "app_restart", {{"executable", "other.exe"}});
        QTRY_VERIFY(!result("app-restart-injection").isEmpty());
        QVERIFY(!result("app-restart-injection")["ok"].toBool());

        // Exercise the real restart dispatch with a fake launcher; never quit the test runner.
        local.testMode = false;
        local.restartLauncher = [] { return QString("Launch failure fixture"); };
        send("app-restart-failed", "app_restart", {});
        QTRY_VERIFY(!result("app-restart-failed").isEmpty());
        QVERIFY(!result("app-restart-failed")["ok"].toBool());
        QVERIFY(!local.appRestartPending);
        std::atomic_bool launchStarted{false};
        QSemaphore finishLaunch;
        local.restartLauncher = [&] {
            launchStarted = true;
            finishLaunch.tryAcquire(1, 2000);
            return QString();
        };
        send("app-restart-cancelled", "app_restart", {});
        QTRY_VERIFY(launchStarted.load());
        local.setAppControlEnabled(false);
        finishLaunch.release();
        QTRY_VERIFY(!local.appRestartPending);
        local.testMode = true;
        local.restartLauncher = {};
        local.setAppControlEnabled(false);
        QVERIFY(authenticate());
        send("app-revoked", "app_logging", {{"value", false}});
        QTRY_VERIFY(!result("app-revoked").isEmpty());
        QVERIFY(!result("app-revoked")["ok"].toBool());
        QVERIFY(local.appState()["logging"].toBool());
        socket.abort();
    }

    void producerQueuePreservesEventOrdering() {
        NetworkService service;
        QSemaphore entered, resume;
        QMetaObject::invokeMethod(service.worker, [&] {
            entered.release();
            resume.acquire();
        });
        entered.acquire();
        service.setSnapshot({{"sample", 1}});
        service.setEventSnapshot({{"sample", 2}});
        service.setSnapshot({{"sample", 3}});
        service.setSnapshot({{"sample", 4}});
        int queued;
        {
            QMutexLocker lock(&service.mailbox->mutex);
            queued = service.mailbox->values.size();
        }
        resume.release();
        int latest = 0;
        QMetaObject::invokeMethod(
            service.worker, [&] { latest = service.worker->latest["sample"].toInt(); },
            Qt::BlockingQueuedConnection);
        QCOMPARE(queued, 3);
        QCOMPARE(latest, 4);
    }

    void policyBarrierDiscardsPrivateBacklog() {
        NetworkService service;
        QSemaphore entered, resume;
        QMetaObject::invokeMethod(service.worker, [&] {
            entered.release();
            resume.acquire();
        });
        entered.acquire();
        service.setEventSnapshot({{"private", "OLD SECRET"}});
        // Seed a slow client's unsent queue as well as the producer queue.
        resume.release();
        QMetaObject::invokeMethod(
            service.worker,
            [&] { service.worker->pendingEvents[nullptr].enqueue({{"private", "OLD SECRET"}}); },
            Qt::BlockingQueuedConnection);
        service.replaceSnapshot({{"sensors", QJsonArray{}}});
        bool empty = false, secret = true;
        QMetaObject::invokeMethod(
            service.worker,
            [&] {
                empty = service.worker->pendingEvents.isEmpty();
                secret = QJsonDocument(service.worker->latest).toJson().contains("OLD SECRET");
            },
            Qt::BlockingQueuedConnection);
        QVERIFY(empty);
        QVERIFY(!secret);
    }

    void workerResultsCanBeReleasedOffGui() {
        BackgroundWorker worker("Result ownership fixture");
        QSemaphore released;
        std::atomic<QThread *> releasedOn{nullptr};
        worker.submit(
            [&] {
                return std::shared_ptr<int>(new int(42), [&](int *value) {
                    releasedOn = QThread::currentThread();
                    delete value;
                    released.release();
                });
            },
            [&](std::shared_ptr<int> result) {
                QCOMPARE(*result, 42);
                worker.post([result = std::move(result)] {});
                // Keep the GUI callback alive until cleanup completes. A hidden
                // result copy in delivery would keep the object on the GUI thread.
                QVERIFY(released.tryAcquire(1, 3000));
                QVERIFY(releasedOn.load());
                QVERIFY(releasedOn.load() != QThread::currentThread());
            });
        QTRY_VERIFY(!worker.isRunning());
    }

    void workerExceptionsRecoverAndReleaseBusyState() {
        BackgroundWorker worker("Failure fixture");
        QStringList states;
        worker.onStatus = [&](QString error) { states.append(error); };
        bool incorrectlyDelivered = false;
        worker.submit([]() -> int { throw std::runtime_error("Fixture failure"); },
                      [&](int) { incorrectlyDelivered = true; });
        QTRY_VERIFY(!worker.isRunning());
        QCOMPARE(states, QStringList{"Fixture failure"});
        QVERIFY(!incorrectlyDelivered);
        int result = 0;
        worker.submit([] { return 42; }, [&](int value) { result = value; });
        QTRY_COMPARE(result, 42);
        QVERIFY(states.last().isEmpty());
        worker.submit([]() -> int { throw 7; }, [&](int) { incorrectlyDelivered = true; });
        QTRY_VERIFY(!worker.isRunning());
        QVERIFY(states.last().contains("Unknown"));
        QVERIFY(!incorrectlyDelivered);
    }

    void storageFreshnessIsIndependentOfFastCollector() {
        QTRY_VERIFY_WITH_TIMEOUT(!c->inventoryJob.isRunning(), 15000);
        c->mergeReadings("windows", {row("win_disk_stale_capacity", 500)}, 3);
        c->inventoryClock.start();
        c->windowsClock.start();
        QVERIFY(find("win_disk_stale_capacity")["available"].toBool());
        c->inventoryClock.invalidate();
        c->windowsClock.restart();
        QVERIFY(!find("win_disk_stale_capacity")["available"].toBool());
        QVERIFY(find("win_disk_stale_capacity")["value"].isNull());
        c->inventoryClock.start();
        QVERIFY(find("win_disk_stale_capacity")["available"].toBool());
    }

    void collectorErrorsAreLoggedOnceAndRecoveryIsVisible() {
        const auto before = c->logLines.size();
        c->inventoryJob.onStatus("Fixture disk error");
        c->inventoryJob.onStatus("Fixture disk error");
        QCOMPARE(c->logLines.size(), before + 1);
        QVERIFY(c->diagnostics().contains("Fixture disk error"));
        QVERIFY(!c->inventoryClock.isValid());
        c->inventoryJob.onStatus({});
        QVERIFY(c->logs().contains("Storage inventory recovered"));
        QVERIFY(!c->diagnostics().contains("Fixture disk error"));
        c->inventoryClock.start();
    }

    void coreSearchIncludesUnitsAndUsageAliases() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.sampleTimer.stop();
        QJsonArray rows;
        auto add = [&](QString id, QString name, QString unit) {
            rows.append(QJsonObject{{"id", id},
                                    {"group", "CPU [#0]: Fixture: DTS"},
                                    {"name", name},
                                    {"unit", unit},
                                    {"type", 1},
                                    {"value", 42.0}});
        };
        add("core_temp", "Core 0", QString::fromUtf8("°C"));
        add("core_usage", "Core 0 T0 Usage", "%");
        add("core_clock", "Core 0 Clock", "MHz");
        local.mergeReadings("hwinfo", rows);
        local.setBrowserFilters("CPU Temperature", "hwinfo", false, false);
        QCOMPARE(local.browser().size(), 2);
        QCOMPARE(local.browser().last().toMap()["id"].toString(), QString("core_temp"));
        local.selectFiltered(true);
        QVERIFY(local.selected.contains("core_temp"));
        QVERIFY(!local.selected.contains("core_usage"));
        local.setBrowserFilters("CPU core load", "hwinfo", false, false);
        QCOMPARE(local.browser().size(), 2);
        QCOMPARE(local.browser().last().toMap()["id"].toString(), QString("core_usage"));
        local.selectFiltered(true);
        QVERIFY(local.selected.contains("core_usage"));
        QVERIFY(!local.selected.contains("core_clock"));
    }

    void specsCacheAvoidsAnotherCollection() {
        QTemporaryDir dir;
        Controller cached(dir.path(), true);
        cached.specsText = "Cached hardware report";
        cached.specsLoaded = true;
        QSignalSpy changes(&cached, &Controller::specsChanged);
        cached.ensureSpecs();
        cached.ensureSpecs();
        QCOMPARE(cached.specs(), QString("Cached hardware report"));
        QVERIFY(!cached.specsBusy);
        QCOMPARE(changes.count(), 0);
    }

    void browserFoldingPreservesHierarchyAndHiddenValues() {
        QVariantList readings;
        const QStringList names{"A", "A / child", "A / child / grandchild", "B"};
        for (int i = 0; i < names.size(); ++i) {
            readings.append(QVariantMap{{"id", names[i]},
                                        {"source", "icue"},
                                        {"name", i % 2 ? "match" : "other"},
                                        {"value", i},
                                        {"available", true},
                                        {"categoryKey", names[i]},
                                        {"categoryName", names[i]},
                                        {"parentCategory", i == 1 || i == 2 ? names[i - 1] : QString()}});
        }
        for (const auto &query : {QString(), QString("match"), QString("nothing")}) {
            BrowserModel model;
            QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
            const QVariantMap filters{{"query", query}, {"source", "all"}};
            auto install = [&](const QSet<QString> &closed) {
                auto categories =
                    BrowserModel::prepareCategories(Controller::formatBrowser(readings, filters, {}));
                auto visible = BrowserModel::visibleCategoryRows(categories, closed);
                model.update(BrowserModel::categorySnapshot(categories, visible, closed));
                model.swapCategories(categories, visible, closed);
            };
            install({});
            QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
            QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
            QPersistentModelIndex anchor;
            for (int i = 0; i < model.rowCount(); ++i) {
                const auto row = model.data(model.index(i), BrowserModel::Row).toMap();
                if (row["header"].toBool() && row["category"] == "B")
                    anchor = model.index(i);
            }
            for (int mask = 0; mask < 16; ++mask) {
                QSet<QString> closed;
                for (int i = 0; i < names.size(); ++i)
                    if (mask & (1 << i))
                        closed.insert(names[i]);
                QVERIFY(model.setCollapsed(closed));
                QCOMPARE(model.snapshot(), Controller::formatBrowser(readings, filters, closed));
                if (query != "nothing") {
                    QVERIFY(anchor.isValid());
                    QCOMPARE(anchor.data(BrowserModel::Row).toMap()["category"].toString(), QString("B"));
                }
            }
            for (const auto &change : changes)
                QCOMPARE(qvariant_cast<QList<int>>(change[2]), QList<int>{BrowserModel::Collapsed});
            QCOMPARE(resets.count(), 0);
            // Updating while closed must replace cached values before the next expansion.
            auto updated = readings[1].toMap();
            updated["value"] = 999;
            readings[1] = updated;
            install({"A", "B"});
            QVERIFY(model.setCollapsed({}));
            QCOMPARE(model.snapshot(), Controller::formatBrowser(readings, filters, {}));
            model.replace({});
            QVERIFY(!model.setCollapsed({"A"}));
            QCOMPARE(model.rowCount(), 0);
        }
    }

    void sensorCategoriesStartCollapsedEachSession() {
        QTemporaryDir dir;
        for (int launch = 0; launch < 2; ++launch) {
            Controller local(dir.path(), true);
            local.stopService();
            local.sampleTimer.stop();
            local.hwinfoTimer.stop();
            local.setModuleEnabled("windows", false);
            auto a = row("fold_a", 1), b = row("fold_b", 2), late = row("fold_late", 3);
            for (auto *reading : {&a, &b, &late}) {
                (*reading)["source"] = "hwinfo";
                (*reading)["group"] = (*reading)["id"];
            }
            local.mergeReadings("hwinfo", {a, b});
            local.setBrowserFilters("", "hwinfo", false, false);
            QTRY_COMPARE(local.browserModel.rowCount(), 2);
            for (const auto &value : local.browserModel.snapshot()) {
                QVERIFY(value.toMap()["header"].toBool());
                QVERIFY(value.toMap()["collapsed"].toBool());
            }
            QVERIFY(!local.settings.contains("collapsed"));
            QVERIFY(!local.settings.contains("icue/led_groups"));
            local.setCategoryCollapsed("hwinfo / fold_a", false);
            QCOMPARE(local.browserModel.rowCount(), 3);
            local.setBrowserVisible(false);
            local.setBrowserVisible(true);
            QTRY_COMPARE(local.browserModel.rowCount(), 3);
            local.mergeReadings("hwinfo", {a, b, late});
            emit local.sensorsChanged();
            QTRY_COMPARE(local.browserModel.rowCount(), 4);
            QVERIFY(local.browserFolding.isCollapsed("hwinfo / fold_late"));
            local.collapseCategories(false);
            QCOMPARE(local.browserModel.rowCount(), 6);
            QVERIFY(!local.settings.contains("collapsed"));
            if (launch == 0) {
                // Old profiles must not restore their saved group expansion.
                local.settings.setValue("collapsed", QStringList{"hwinfo / fold_b"});
                local.settings.setValue("icue/led_groups", QStringList{"old-device/leds"});
                QVERIFY(!ConfigurationFile::snapshot(local.settings).contains("collapsed"));
            }
        }
    }

    void browserFoldingDoesNotWaitForWorker() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.stopService();
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.setModuleEnabled("windows", false);
        auto item = row("fold_fixture", 42);
        item["source"] = "hwinfo";
        item["group"] = "Fixture";
        local.mergeReadings("hwinfo", {item});
        local.setBrowserFilters("", "hwinfo", false, false);
        local.setBrowserVisible(true);
        QTRY_COMPARE(local.browserModel.rowCount(), 2);
        local.browserRefreshTimer.stop();
        QTRY_VERIFY(!local.browserJob.isRunning() && !local.browserSearchJob.isRunning());
        auto entered = std::make_shared<QSemaphore>();
        auto resume = std::make_shared<QSemaphore>();
        local.browserJob.submit(
            [entered, resume] {
                entered->release();
                resume->acquire();
                return true;
            },
            [](bool) {});
        const auto release = qScopeGuard([resume] { resume->release(); });
        QVERIFY(entered->tryAcquire(1, 5000));
        for (int n = 0; n < 3; ++n) {
            local.setCategoryCollapsed("hwinfo / Fixture", true);
            QCOMPARE(local.browserModel.rowCount(), 1);
            local.setCategoryCollapsed("hwinfo / Fixture", false);
            QCOMPARE(local.browserModel.rowCount(), 2);
            local.collapseCategories(true);
            QCOMPARE(local.browserModel.rowCount(), 1);
            local.collapseCategories(false);
            QCOMPARE(local.browserModel.rowCount(), 2);
        }
        QVERIFY(local.browserJob.isRunning());
    }

    void browserKeepsSurvivingRowsOnCollapse() {
        BrowserModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const QVariant a = QVariantMap{{"header", true}, {"category", "A"}};
        const QVariant b = QVariantMap{{"header", true}, {"category", "B"}};
        const QVariant sensor = QVariantMap{{"header", false}, {"id", "a1"}};
        model.update({a, sensor, b});
        QPersistentModelIndex anchor(model.index(2));
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        model.update({a, b});
        QVERIFY(anchor.isValid());
        QCOMPARE(anchor.row(), 1);
        model.update({a, sensor, b});
        QVERIFY(anchor.isValid());
        QCOMPARE(anchor.row(), 2);
        model.update({b, a, sensor});
        QVERIFY(anchor.isValid());
        QCOMPARE(anchor.row(), 0);
        QCOMPARE(resets.count(), 0);
    }

    void initTestCase() {
        QSettings settings(directory.path() + "/test-settings.yaml", SettingsStore::format());
        settings.setValue("port", 18767);
        settings.setValue("sources/hwinfo", false);
        settings.setValue("license", "pro");
        settings.sync();
        c = std::make_unique<Controller>(directory.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(c->windowsClock.isValid(), 15000);
        c->sampleTimer.stop();
        c->publishTimer.stop();
    }

    void wifiPermissionRecovery() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!local.windowsJob.isRunning(), 15000);
        local.setAnimationsEnabled(false);
        local.settings.setValue("sources/hwinfo", false);
        local.wifiIssue = "Windows denied access to Wi-Fi details. Allow location access in Windows Location Settings.";
        const auto issue = local.wifiIssue;
        QCOMPARE(local.moduleIssues()["windows"].toString(), issue);
        QVERIFY(local.sourceOptions()["wifi"].toBool());

        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->resize(1100, 820);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto item = [&](const QString &name) { return visualItem(window->contentItem(), name); };
        window->setProperty("previewPage", 0);
        QTRY_VERIFY(item("overview-windowsWarning") && item("overview-windowsWarning")->isVisible());
        QVERIFY(QMetaObject::invokeMethod(item("overview-windowsWarning"), "actionTriggered"));
        QTRY_VERIFY(item("wifiSubmodule"));
        item("wifiSubmodule")->setProperty("expanded", true);
        QTRY_VERIFY(item("wifiLocationSettings") && item("wifiLocationSettings")->isVisible());
        QCOMPARE(item("wifiPermissionWarning")->property("text").toString(), issue);

        // Disabling Wi-Fi must hide only its readings, retain selections and stop sharing values.
        const auto ssid = row("win_net_fixture_ssid", "Fixture", "text");
        const auto link = row("win_net_fixture_connected", true, "binary");
        local.mergeReadings("windows", {ssid, link}, 1);
        local.windowsClock.restart();
        local.selected.insert("win_net_fixture_ssid");
        local.settings.setValue("sources/wifi", false);
        QVERIFY(!local.readingBrowserEnabled(ssid));
        QVERIFY(local.readingBrowserEnabled(link));
        QVERIFY(!local.moduleIssues().contains("windows"));
        for (const auto &value : local.allReadings()) {
            const auto reading = value.toObject();
            if (reading["id"] == "win_net_fixture_ssid") {
                QVERIFY(!reading["available"].toBool());
                QVERIFY(reading["value"].isNull());
            }
        }
        QVERIFY(local.selected.contains("win_net_fixture_ssid"));
        local.settings.setValue("sources/wifi", true);
        local.settings.setValue("modules/windows/enabled", false);
        QVERIFY(!local.moduleIssues().contains("windows"));
        local.settings.setValue("modules/windows/enabled", true);
        QCOMPARE(local.moduleIssues()["windows"].toString(), issue);

        // The next successful worker result removes both warnings without recreating the UI.
        local.wifiIssue.clear();
        emit local.changed();
        QTRY_VERIFY(!item("wifiPermissionWarning")->isVisible());
        QVERIFY(!local.moduleIssues().contains("windows"));
        QVERIFY(local.readingBrowserEnabled(ssid));

        WindowsCollector collector;
        const auto readings = collector.collect({{"wifi", false}, {"_fast_only", true}});
        QVERIFY(collector.wifiIssue().isEmpty());
        for (const auto &value : readings) {
            const auto id = value.toObject()["id"].toString();
            QVERIFY(!id.endsWith("_ssid") && !id.endsWith("_wifi_signal"));
        }
        QTRY_VERIFY(!item("windowsModuleWarning")->isVisible());
        // Let any page loaders finish before destroying their shared engine.
        if (auto *incubation = engine.incubationController())
            QTRY_COMPARE_WITH_TIMEOUT(incubation->incubatingObjectCount(), 0, 5000);
        window->hide();
    }

    void windowsWithoutHwinfo() {
        auto snapshot = c->snapshot();
        QVERIFY(!snapshot["hwinfo_enabled"].toBool());
        QVERIFY(snapshot["windows_healthy"].toBool());
        QVERIFY(find("win_uptime")["value"].toDouble() > 0);
        QVERIFY(find("win_ram_total")["value"].toDouble() > 0);
        QCOMPARE(find("win_last_boot")["kind"].toString(), "timestamp");
        QVERIFY(!c->healthy());
        QVERIFY(c->sourceStatus().contains("disabled"));
        QVERIFY(!c->sourceOptions()["window_titles"].toBool());
        QVERIFY(!c->sourceOptions()["notification_contents"].toBool());
    }

    void removalAndReinsertion() {
        c->inventoryClock.start();
        const auto connected = "win_disk_fixture_connected", capacity = "win_disk_fixture_capacity";
        c->mergeReadings("windows", {row(connected, true, "binary"), row(capacity, 12345)});
        c->setSensorEnabled(capacity, true);
        QVERIFY(find(capacity)["available"].toBool());
        c->mergeReadings("windows", {row(connected, false, "binary")});
        QVERIFY(find(connected)["available"].toBool());
        QVERIFY(!find(connected)["value"].toBool());
        QVERIFY(!find(capacity)["available"].toBool());
        QVERIFY(find(capacity)["value"].isNull());
        QVERIFY(c->selected.contains(capacity));
        c->mergeReadings("windows", {row(connected, true, "binary"), row(capacity, 23456)});
        QCOMPARE(find(capacity)["value"].toInt(), 23456);
        QVERIFY(c->selected.contains(capacity));
        WindowsCollector fresh;
        fresh.restoreVolumes(QJsonArray{row(connected, true, "binary")});
        const auto rows = fresh.collect({});
        bool found = false;
        for (auto value : rows)
            if (value.toObject()["id"] == connected) {
                found = true;
                QVERIFY(!value.toObject()["value"].toBool());
            }
        QVERIFY(found);
    }

    void nativeMediaWorkerKeepsTrackMetadataPrivate() {
        BackgroundWorker worker{"Media collection test"};
        for (int pass = 0; pass < 2; ++pass) {
            bool finished = false;
            worker.submit([] { return readMediaSessions(false); },
                          [&](QJsonArray rows) {
                              QVERIFY(!rows.isEmpty());
                              for (auto value : rows) {
                                  const auto id = value.toObject()["id"].toString();
                                  QVERIFY(id.startsWith("win_media_"));
                                  QVERIFY(id != "win_media_title" && id != "win_media_artist");
                              }
                              finished = true;
                          });
            QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
        }
    }

    void mediaAndNotificationSnapshotsStayIndependent() {
        c->mergeReadings("windows", {row("win_media_state", "Playing", "text")}, 7);
        c->mergeReadings("windows", {row("win_notifications_count", 2)}, 2);
        QCOMPARE(c->catalog.value("win_media_state")["value"].toString(), QString("Playing"));
        QVERIFY(c->catalog.value("win_media_state")["available"].toBool());
        c->mergeReadings("windows", {}, 7);
        QVERIFY(!c->catalog.value("win_media_state")["available"].toBool());
        QVERIFY(c->catalog.value("win_notifications_count")["available"].toBool());
    }

    void privacyRevocationMasksCachedAndInFlightValues() {
        for (const auto id : {"win_foreground_title", "win_notifications_title", "win_notifications_body",
                              "win_events_system_message", "win_media_title"}) {
            auto r = row(id, "PRIVATE FIXTURE", "text");
            c->mergeReadings("windows", {r});
            c->setSensorEnabled(id, true);
            QVERIFY(!find(id)["available"].toBool());
            QVERIFY(find(id)["value"].isNull());
            QVERIFY(!QJsonDocument(c->snapshot()).toJson().contains("PRIVATE FIXTURE"));
        }
    }

    void browserFiltersCollapseAndFavourites() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        ++local.collectionGeneration;
        local.mergeReadings("windows", {row("win_fixture_a", 4), row("win_fixture_b", 5)});
        local.setSensorEnabled("win_fixture_a", true);
        local.setBrowserFilters("", "windows", false, false);
        auto selected = local.selected;
        local.collapseCategories(true);
        for (const auto &r : local.browser())
            QVERIFY(r.toMap()["header"].toBool());
        QCOMPARE(local.selected, selected);
        local.collapseCategories(false);
        local.setFavourite("win_fixture_a", true);
        local.setBrowserFilters("Fixture", "windows", true, true);
        auto visible = local.browser();
        QCOMPARE(visible.size(), 2);
        QCOMPARE(visible.last().toMap()["id"].toString(), "win_fixture_a");
        local.setCategoryEnabled("windows / Storage / Fixture", false);
        QCOMPARE(local.browser().size(), 0);
        local.setBrowserFilters("", "hwinfo", false, false);
        QCOMPARE(local.browser().size(), 0);
    }

    void favouriteFilterDefaultsOffAndPersists() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        QVERIFY(!local.info()["browserFavourites"].toBool());
        local.setBrowserFilters("", "all", false, true);
        {
            QSettings saved(local.settings.fileName(), SettingsStore::format());
            QTRY_VERIFY(saved.value("browser/favourites").toBool());
        }
        local.setBrowserFilters("", "all", false, false);
        {
            QSettings saved(local.settings.fileName(), SettingsStore::format());
            QTRY_VERIFY(!saved.value("browser/favourites").toBool());
        }
    }

    void browserSearchIsSessionOnly() {
        QTemporaryDir dir;
        const auto path = SettingsStore::profilePath(dir.path(), true);
        {
            QSettings oldSettings(path, SettingsStore::format());
            oldSettings.setValue("browser/query", "previous launch");
        }
        {
            Controller local(dir.path(), true);
            local.stopService();
            local.setBrowserVisible(false);
            QVERIFY(local.browserFilters()["query"].toString().isEmpty());
            QVERIFY(!local.settings.contains("browser/query"));
            local.setBrowserFilters("temperature", "hwinfo", true, false);
            local.flushSettingsAsync();
            // Saving preferences or quitting during a pending save must not persist search text.
            local.setBrowserFilters("final query", "icue", false, true, true);
            QCOMPARE(local.browserFilters().value("query").toString(), QString("final query"));
        }
        QSettings saved(path, SettingsStore::format());
        QTRY_COMPARE(saved.value("browser/source").toString(), QString("icue"));
        QVERIFY(!saved.contains("browser/query"));
        QVERIFY(!saved.value("browser/selected").toBool());
        QVERIFY(saved.value("browser/favourites").toBool());
        QVERIFY(saved.value("browser/unshared").toBool());
        Controller restarted(dir.path(), true);
        QVERIFY(restarted.info()["browserQuery"].toString().isEmpty());
        QCOMPARE(restarted.info()["browserSource"].toString(), QString("icue"));
        QVERIFY(restarted.info()["browserFavourites"].toBool());
        QVERIFY(restarted.info()["browserUnshared"].toBool());
    }

    void iniBooleanStringsStayFalseInQml() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        const QList<QPair<QString, QString>> fields{
            {"browser/selected", "browserSelected"}, {"browser/favourites", "browserFavourites"},
            {"start_minimised", "startMinimised"},   {"startup_notice", "startupNotice"},
            {"sources/hwinfo", "hwinfoEnabled"}};
        for (const auto &field : fields) {
            const auto previous = local.settings.value(field.first);
            local.settings.setValue(field.first, QString("false"));
            const auto exposed = local.info().value(field.second);
            QCOMPARE(exposed.metaType().id(), int(QMetaType::Bool));
            QVERIFY(!exposed.toBool());
            local.settings.setValue(field.first, QString("true"));
            QVERIFY(local.info().value(field.second).toBool());
            if (previous.isValid())
                local.settings.setValue(field.first, previous);
            else
                local.settings.remove(field.first);
        }
    }

    void searchMatchesWordsAcrossCategoryAndSensor() {
        c->collapseCategories(false);
        auto reading = row("win_search", 12);
        reading["name"] = "SearchUnique";
        c->mergeReadings("windows", {reading});
        c->setBrowserFilters("SEARCHUNIQUE   storage", "windows", false, false);
        QCOMPARE(c->browser().size(), 2);
        c->selectSensors("none");
        c->selectFiltered(true);
        QVERIFY(c->selected.contains("win_search"));
        c->setBrowserFilters("no_such_sensor", "windows", false, false);
        QCOMPARE(c->browser().size(), 0);
        c->setBrowserFilters("", "all", false, false);
    }

    void hwinfoFailureDoesNotDisableWindows() {
        c->settings.setValue("sources/hwinfo", true);
        c->windowsClock.restart();
        c->mergeReadings("windows", {row("win_fixture_live", 10)});
        c->latest.valid = false;
        QVERIFY(!c->healthy());
        QVERIFY(find("win_fixture_live")["available"].toBool());
        c->windowsClock.invalidate();
        QVERIFY(!find("win_fixture_live")["available"].toBool());
    }

    void hwinfoAvailabilityAndDiagnostics() {
        c->settings.setValue("sources/hwinfo", true);
        c->latest.valid = true;
        c->latest.pollTime = QDateTime::currentSecsSinceEpoch();
        c->latest.status = "Shared memory connected";
        c->latest.readings = {QJsonObject{{"id", "hw_fixture"},
                                          {"name", "Read Activity"},
                                          {"group", "Drive"},
                                          {"value", 12.5},
                                          {"type", 5},
                                          {"unit", "%"}}};
        c->initialAge = 0;
        c->pollClock.restart();
        c->mergeReadings("hwinfo", c->latest.readings);
        QVERIFY(find("hw_fixture")["available"].toBool());
        QCOMPARE(find("hw_fixture")["value"].toDouble(), 12.5);
        c->mergeReadings("windows", {row("win_fixture", 1)});
        QVERIFY(find("hw_fixture")["available"].toBool());
        c->stopService();
        QVERIFY(find("hw_fixture")["available"].toBool());
        c->initialAge = 3600;
        QVERIFY(!find("hw_fixture")["available"].toBool());
        QVERIFY(find("hw_fixture")["unavailable_reason"].toString().contains("stale"));
        QVERIFY(c->diagnostics().contains("Available: 0"));
        c->initialAge = 0;
        QVERIFY(find("hw_fixture")["available"].toBool());
        QVERIFY(c->startService());
    }

    void selectAllAndFilteredAreIndependent() {
        c->mergeReadings("windows", {row("win_selection_a", 4), row("win_selection_b", 5)});
        c->selectSensors("none");
        c->setBrowserFilters("no matching reading", "hwinfo", true, true);
        c->selectSensors("all");
        QVERIFY(c->selected.contains("win_selection_a"));
        QVERIFY(c->selected.contains("win_selection_b"));
        c->selectSensors("none");
        c->setFavourite("win_selection_a", true);
        c->setBrowserFilters("Fixture", "windows", false, true);
        c->collapseCategories(true);
        c->selectFiltered(true);
        QVERIFY(c->selected.contains("win_selection_a"));
        QVERIFY(!c->selected.contains("win_selection_b"));
        c->selectFiltered(false);
        QVERIFY(!c->selected.contains("win_selection_a"));
    }

    void livePortChangeAndBusyPortRollback() {
        c->settings.setValue("sources/hwinfo", false);
        QCOMPARE(QSslSocket::activeBackend(), "openssl");
        const auto identity = c->deviceId, fingerprint = c->fingerprint, token = c->token;
        QWebSocket client;
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        client.setSslConfiguration(ssl);
        QSignalSpy messages(&client, &QWebSocket::textMessageReceived),
            disconnected(&client, &QWebSocket::disconnected);
        auto connectClient = [&] { client.open(QUrl(QString("wss://127.0.0.1:%1").arg(c->activePort))); };
        connectClient();
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QAbstractSocket::ConnectedState, 5000);
        client.sendTextMessage(QJsonDocument(QJsonObject{{"type", "auth"}, {"protocol", 2}, {"token", token}})
                                   .toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(messages.size() >= 2, 5000);
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost, 0));
        const int oldPort = c->activePort;
        c->saveSettings(1, 1, occupied.serverPort(), "pro", false);
        QCOMPARE(c->activePort, oldPort);
        QCOMPARE(c->settings.value("port", oldPort).toInt(), oldPort);
        QVERIFY(c->server.isListening());
        QCOMPARE(disconnected.size(), 0);
        QVERIFY(c->settingsStatus.contains("unavailable"));
        QTcpServer available;
        QVERIFY(available.listen(QHostAddress::LocalHost, 0));
        const int newPort = available.serverPort();
        available.close();
        c->saveSettings(1, 1, newPort, "pro", false);
        QVERIFY(c->server.isListening());
        QCOMPARE(c->activePort, newPort);
        QTRY_VERIFY_WITH_TIMEOUT(disconnected.size() > 0, 5000);
        QSettings persisted(directory.path() + "/test-settings.yaml", SettingsStore::format());
        QCOMPARE(persisted.value("port").toInt(), newPort);
        QCOMPARE(c->deviceId, identity);
        QCOMPARE(c->fingerprint, fingerprint);
        QCOMPARE(c->token, token);
        messages.clear();
        connectClient();
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QAbstractSocket::ConnectedState, 5000);
        client.sendTextMessage(QJsonDocument(QJsonObject{{"type", "auth"}, {"protocol", 2}, {"token", token}})
                                   .toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(messages.size() >= 2, 5000);
        const auto hello = QJsonDocument::fromJson(messages.first().first().toString().toUtf8()).object();
        QCOMPARE(hello["device_id"].toString(), identity);
        client.abort();
    }

    void startupRegistrationUsesCurrentExecutable() {
#ifdef QT_DEBUG
        QVERIFY(!StartupRegistration::supported());
#else
        QVERIFY(StartupRegistration::supported());
#endif
        QTemporaryDir folder;
        QSettings run(folder.path() + "/run.yaml", SettingsStore::format());
        const QString executable = "C:/Apps/WHAT/WHAT.exe";
        QVERIFY(!StartupRegistration::update(run, executable, {}).enabled);
        auto state = StartupRegistration::update(run, executable, {}, true);
        if (StartupRegistration::supported()) {
            QVERIFY(state.error.isEmpty());
            QVERIFY(state.enabled);
            QVERIFY(StartupRegistration::update(run, executable, {}).enabled);
            QVERIFY(!StartupRegistration::update(run, "C:/Other/WHAT.exe", {}).enabled);
            QVERIFY(!StartupRegistration::update(run, executable, {}, false).enabled);
        } else {
            QVERIFY(!state.enabled);
            QVERIFY(state.error.contains("Debug"));
            QVERIFY(!run.contains("WHAT"));
            run.setValue("WHAT", '"' + QDir::toNativeSeparators(executable) + '"');
            state = StartupRegistration::update(run, executable, {});
            QVERIFY(!state.enabled);
            QVERIFY(state.error.isEmpty());
            QVERIFY(!run.contains("WHAT"));
            // A Debug session must not change another portable or installed build.
            const QString releaseCommand = "\"C:\\Release\\WHAT.exe\"";
            run.setValue("WHAT", releaseCommand);
            for (const bool enabled : {false, true}) {
                QVERIFY(!StartupRegistration::update(run, executable, {}, enabled).enabled);
                QCOMPARE(run.value("WHAT").toString(), releaseCommand);
            }
            const QString packageCommand = "explorer.exe shell:AppsFolder\\WHAT.Desktop_test!App";
            run.setValue("WHAT", packageCommand);
            QVERIFY(!StartupRegistration::update(run, executable, "WHAT.Desktop_test", true).enabled);
            QCOMPARE(run.value("WHAT").toString(), packageCommand);
            run.remove("WHAT");
        }
        QVERIFY(!run.contains("WHAT"));
        run.setValue("WHAT", "unrelated.exe");
        StartupRegistration::update(run, executable, {}, false);
        QCOMPARE(run.value("WHAT").toString(), QString("unrelated.exe"));
        QVERIFY(StartupRegistration::isWhatCommand("\"C:/Old Path/WHAT.exe\"", "WHAT.Desktop_test"));
        QVERIFY(StartupRegistration::isWhatCommand("explorer.exe \"shell:AppsFolder\\WHAT.Desktop_test!App\"", "WHAT.Desktop_test"));
        QVERIFY(!StartupRegistration::isWhatCommand("explorer.exe \"shell:AppsFolder\\Other!App\"", "WHAT.Desktop_test"));
        QVERIFY(!StartupRegistration::isWhatCommand("WHAT.exe --unexpected", {}));
    }

    void powerModeKeysAndLabelsAreAccepted() {
        for (const auto *value : {"efficiency", "balanced", "performance", "Best power efficiency", "Balanced", "Best performance"})
            QVERIFY(WindowsCommands::validate("power_mode_ac", {{"value", value}}).isEmpty());
        QVERIFY(!WindowsCommands::validate("power_mode_dc", {{"value", "unknown"}}).isEmpty());
    }

    void incompleteIdentityRequiresExplicitRenewal() {
        QTemporaryDir folder;
        auto original = TlsIdentity::renew(folder.path(), true);
        QVERIFY(QFile::remove(folder.path() + "/test-identity.json"));
        QVERIFY_EXCEPTION_THROWN(TlsIdentity::loadOrCreate(folder.path(), true), std::runtime_error);
        QVERIFY(!QFile::exists(folder.path() + "/test-identity.json"));
        QFile certificate(folder.path() + "/test-certificate.pem");
        QVERIFY(certificate.open(QIODevice::ReadOnly));
        QCOMPARE(certificate.readAll(), original.certificate.toPem());
        certificate.close();
        auto renewed = TlsIdentity::renew(folder.path(), true);
        QVERIFY(renewed.fingerprint != original.fingerprint);
        QCOMPARE(TlsIdentity::loadOrCreate(folder.path(), true).fingerprint, renewed.fingerprint);
    }

    void serviceStopStartRestartAndPersistence() {
        c->sampleTimer.start();
        c->stopService();
        QVERIFY(!c->server.isListening());
        QVERIFY(c->sampleTimer.isActive());
        QVERIFY(!c->publishTimer.isActive());
        QVERIFY(!c->settings.value("service_enabled").toBool());
        c->sampleWindows();
        QTRY_VERIFY_WITH_TIMEOUT(c->windowsClock.isValid(), 15000);
        QVERIFY(!c->info()["startMinimised"].toBool());
        c->setStartMinimised(true);
        const auto identity = c->deviceId;
        c.reset();
        c = std::make_unique<Controller>(directory.path(), true);
        QVERIFY(!c->server.isListening());
        QCOMPARE(c->deviceId, identity);
        QVERIFY(c->sampleTimer.isActive());
        QVERIFY(c->info()["startMinimised"].toBool());
        QVERIFY(c->startService());
        QVERIFY(c->sampleTimer.isActive());
        QVERIFY(c->publishTimer.isActive());
        QTRY_VERIFY_WITH_TIMEOUT(c->windowsClock.isValid(), 15000);
        QVERIFY(c->restartService());
        QCOMPARE(c->deviceId, identity);
        QTRY_VERIFY_WITH_TIMEOUT(c->windowsClock.isValid(), 15000);
        c->stopService();
    }

    void certificateWarningsAndRecovery() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.stopService();
        local.sampleTimer.stop();
        local.hwinfoTimer.stop();
        local.setModuleEnabled("windows", false);
        const auto deviceId = local.deviceId;
        const auto token = local.token;
        const auto fingerprint = local.fingerprint;
        QVERIFY(local.saveInstallation({{"name", "Other House"}, {"type", "api"},
                                        {"enabled", false}, {"controls", true}, {"url", ""}}));
        const auto installations = local.settings.value("connections/installations");
        QVariantMap credentials;
        for (const auto &value : local.installations()) {
            const auto key = "connections/credentials/" + value.toMap()["id"].toString();
            credentials[key] = local.settings.value(key);
        }
        local.setSensorEnabled("win_certificate_fixture", true);
        const auto selected = local.selected;
        QVERIFY(local.info()["certificateWarning"].toString().isEmpty());

        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"controller", QVariant::fromValue(&local)},
                                     {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *warning = window->findChild<QQuickItem *>("overviewCertificateWarning");
        auto *renew = window->findChild<QQuickItem *>("overviewRenewCertificate");
        auto *dialog = window->findChild<QObject *>("certificateDialog");
        QVERIFY(warning && renew && dialog);
        QVERIFY(!warning->isVisible());

        const auto pem = local.tlsConfiguration.localCertificate().toPem();
        std::unique_ptr<BIO, decltype(&BIO_free)> input(
            BIO_new_mem_buf(pem.constData(), int(pem.size())), BIO_free);
        QVERIFY(input);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(
            PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
        QVERIFY(certificate);
        const auto keyPem = local.tlsConfiguration.privateKey().toPem();
        std::unique_ptr<BIO, decltype(&BIO_free)> keyInput(
            BIO_new_mem_buf(keyPem.constData(), int(keyPem.size())), BIO_free);
        QVERIFY(keyInput);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            PEM_read_bio_PrivateKey(keyInput.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
        QVERIFY(key);
        // These date-only fixtures are never served or saved. Renewal below
        // generates and persists a real signed certificate through the worker.
        for (const long remaining : {45L * 86400, -60L}) {
            QVERIFY(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), remaining));
            QVERIFY(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);
            QByteArray der(i2d_X509(certificate.get(), nullptr), '\0');
            auto *bytes = reinterpret_cast<unsigned char *>(der.data());
            QVERIFY(i2d_X509(certificate.get(), &bytes) > 0);
            local.tlsConfiguration.setLocalCertificate(QSslCertificate(der, QSsl::Der));
            QVERIFY(!local.info()["certificateWarning"].toString().isEmpty());
            QCOMPARE(local.info()["certificateExpired"].toBool(), remaining < 0);
            emit local.changed();
            QTRY_VERIFY(warning->isVisible());
            // Verify the severity role without pinning the test to one palette.
            QQmlExpression themeColour(qmlContext(warning), warning,
                                       remaining < 0 ? "Theme.errorSurface" : "Theme.warningSurface");
            const auto expectedColour = themeColour.evaluate().value<QColor>();
            QVERIFY2(!themeColour.hasError(), qPrintable(themeColour.error().toString()));
            QVERIFY(expectedColour.isValid());
            QTRY_COMPARE(warning->property("color").value<QColor>(), expectedColour);
        }

        // Simulate the startup state after an unreadable identity, with the
        // connection still enabled. A successful renewal must resume listening.
        local.tlsReady = false;
        local.tlsConfiguration = QSslConfiguration{};
        local.settings.setValue("service_enabled", true);
        emit local.changed();
        QTRY_VERIFY(window->property("status").toMap()["certificateWarning"]
                        .toString().contains("could not be loaded"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          renew->mapToScene(QPointF(renew->width() / 2, renew->height() / 2)).toPoint());
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(!dialog->property("renewed").toBool());
        QSignalSpy finished(&local, &Controller::certificateRenewalFinished);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY(finished.first().first().toBool());
        QVERIFY(local.tlsReady);
        QVERIFY(local.server.isListening());
        QCOMPARE(local.tlsConfiguration.protocol(), QSsl::TlsV1_2OrLater);
        QCOMPARE(local.deviceId, deviceId);
        QCOMPARE(local.token, token);
        QCOMPARE(local.selected, selected);
        QCOMPARE(local.settings.value("connections/installations"), installations);
        for (auto it = credentials.cbegin(); it != credentials.cend(); ++it)
            QCOMPARE(local.settings.value(it.key()), it.value());
        QVERIFY(local.fingerprint != fingerprint);
        QCOMPARE(TlsIdentity::loadOrCreate(dir.path(), true).fingerprint, local.fingerprint);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(dialog->property("renewed").toBool());
        auto *copy = window->findChild<QQuickItem *>("renewedCertificateCopyFingerprint");
        QVERIFY(copy && copy->isVisible());
        QTRY_VERIFY(!warning->isVisible());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));

        local.stopService();
        local.renewCertificate();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 10000);
        QVERIFY(finished.last().first().toBool());
        QVERIFY(!local.server.isListening());
        QVERIFY(!local.settings.value("service_enabled").toBool());
        QCOMPARE(local.deviceId, deviceId);
        QCOMPARE(local.token, token);
        QCOMPARE(local.selected, selected);
        QCOMPARE(local.settings.value("connections/installations"), installations);
        for (auto it = credentials.cbegin(); it != credentials.cend(); ++it)
            QCOMPARE(local.settings.value(it.key()), it.value());
    }

    void atomicCertificateRenewalPreservesProfile() {
        QTemporaryDir temp;
        auto first = TlsIdentity::loadOrCreate(temp.path(), true);
        auto second = TlsIdentity::renew(temp.path(), true);
        QCOMPARE(second.certificate.subjectInfo(QSslCertificate::CommonName),
                 QStringList{QStringLiteral("WHAT")});
        QVERIFY(first.fingerprint != second.fingerprint);
        QCOMPARE(TlsIdentity::loadOrCreate(temp.path(), true).fingerprint, second.fingerprint);
        QVERIFY(QDateTime::currentDateTimeUtc().daysTo(second.certificate.expiryDate()) > 3600);
        // A conflicting legacy key cannot partially replace the authoritative bundle.
        QFile legacy(temp.path() + "/test-private-key.pem");
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        legacy.write(first.key.toPem());
        legacy.close();
        QCOMPARE(TlsIdentity::loadOrCreate(temp.path(), true).fingerprint, second.fingerprint);
    }

    void networkKeepsPublishingWhileGuiIsBlocked() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        QVERIFY(local.startService());
        // Seed the network worker before deliberately blocking the GUI. This
        // tests network independence, not completion of GUI-delivered captures,
        // and must not inherit an empty privacy barrier from an earlier test.
        local.server.setSnapshot(local.snapshot());
        local.server.setInterval(100);
        const int port = local.activePort;
        const QString token = local.token;
        std::atomic<int> snapshots{0};
        auto *thread = QThread::create([&] {
            QWebSocket socket;
            auto ssl = QSslConfiguration::defaultConfiguration();
            ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
            socket.setSslConfiguration(ssl);
            QEventLoop loop;
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
            QObject::connect(&socket, &QWebSocket::connected, &loop, [&] {
                socket.sendTextMessage(
                    QJsonDocument(QJsonObject{{"type", "auth"}, {"protocol", 2}, {"token", token}})
                        .toJson(QJsonDocument::Compact));
            });
            QObject::connect(&socket, &QWebSocket::textMessageReceived, &loop, [&](QString text) {
                if (QJsonDocument::fromJson(text.toUtf8()).object()["type"] == "snapshot")
                    ++snapshots;
            });
            socket.open(QUrl(QString("wss://127.0.0.1:%1").arg(port)));
            deadline.start(1200);
            loop.exec();
            socket.abort();
        });
        thread->start();
        QThread::msleep(1500); // Deliberately no GUI event processing.
        QVERIFY(thread->wait(2000));
        delete thread;
        QVERIFY2(snapshots.load() >= 3, "Telemetry was blocked by the GUI thread");
        local.stopService();
    }

    void nativeLockEventRespectsPrivacy() {
        c->settings.setValue("sources/session", true);
        c->mergeReadings("windows", {row("win_locked", false, "binary")});
        c->windowsClock.restart();
        MSG message{};
        message.message = WM_WTSSESSION_CHANGE;
        message.wParam = WTS_SESSION_LOCK;
        c->nativeEventFilter({}, &message, nullptr);
        QVERIFY(find("win_locked")["value"].toBool());
        c->settings.setValue("sources/session", false);
        message.wParam = WTS_SESSION_UNLOCK;
        c->nativeEventFilter({}, &message, nullptr);
        QVERIFY(!find("win_locked")["available"].toBool());
        c->settings.setValue("sources/session", true);
    }

    void collectionScopesDoNotEraseOtherWorkers() {
        c->mergeReadings("windows", {row("win_disk_fixture", 1)}, 3);
        c->mergeReadings("windows", {row("win_fast_fixture", 2)}, 1);
        c->mergeReadings("windows", {}, 2);
        QVERIFY(c->catalog["win_disk_fixture"]["available"].toBool());
        QVERIFY(c->catalog["win_fast_fixture"]["available"].toBool());
        c->mergeReadings("windows", {}, 3);
        QVERIFY(!c->catalog["win_disk_fixture"]["available"].toBool());
        QVERIFY(c->catalog["win_fast_fixture"]["available"].toBool());
    }

    void deltasArriveBeforeHeartbeatAndRemoveUnselectedReadings() {
        c->sampleTimer.stop();
        c->publishTimer.stop();
        QTRY_VERIFY_WITH_TIMEOUT(
            !c->windowsJob.isRunning() && !c->inventoryJob.isRunning() && !c->slowJob.isRunning(), 15000);
        c->settings.setValue("sources/hwinfo", false);
        c->windowsClock.restart();
        c->mergeReadings("windows", {row("win_stream", 1)}, 1);
        c->selected = {"win_stream"};
        c->settings.setValue("publishing/win_stream", QVariantMap{{"scope", 1}, {"high", true}});
        c->refreshPublishingPolicies();
        QVERIFY(c->startService());
        c->publishTimer.setInterval(15000);
        c->publish();
        QWebSocket socket;
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        socket.setSslConfiguration(ssl);
        QSignalSpy messages(&socket, &QWebSocket::textMessageReceived);
        socket.open(QUrl(QString("wss://127.0.0.1:%1").arg(c->activePort)));
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::ConnectedState, 5000);
        socket.sendTextMessage(
            QJsonDocument(
                QJsonObject{{"type", "auth"}, {"protocol", 2}, {"token", c->token}, {"delta_v1", true}})
                .toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(messages.size() >= 2, 5000);
        const auto hello = QJsonDocument::fromJson(messages[0][0].toString().toUtf8()).object();
        QVERIFY(hello["delta_v1"].toBool());
        const auto initial = QJsonDocument::fromJson(messages[1][0].toString().toUtf8()).object();
        QCOMPARE(initial["type"].toString(), "snapshot");
        QCOMPARE(initial["sequence"].toInt(), 1);
        messages.clear();
        c->catalog["win_stream"]["value"] = 2;
        QElapsedTimer elapsed;
        elapsed.start();
        c->publish();
        auto receivedValue = [&](int expected) {
            for (const auto &message : messages) {
                const auto frame = QJsonDocument::fromJson(message[0].toString().toUtf8()).object();
                for (const auto &reading : frame["sensors"].toArray())
                    if (reading.toObject()["id"] == "win_stream" &&
                        reading.toObject()["value"].toInt() == expected)
                        return true;
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(receivedValue(2), 1000);
        const auto delta = QJsonDocument::fromJson(messages.last()[0].toString().toUtf8()).object();
        QCOMPARE(delta["type"].toString(), "delta");
        QCOMPARE(delta["sensors"].toArray().size(), 1);
        QCOMPARE(delta["sensors"].toArray()[0].toObject()["value"].toInt(), 2);
        qInfo("Loopback sample-to-frame latency: %lld ms", elapsed.elapsed());
        messages.clear();
        c->selected.clear();
        c->publish();
        QTRY_VERIFY_WITH_TIMEOUT(!messages.empty(), 1000);
        const auto removed = QJsonDocument::fromJson(messages.last()[0].toString().toUtf8()).object();
        QVERIFY(removed["removed"].toArray().contains("win_stream"));
        messages.clear();
        c->selected = {"win_stream"};
        c->publishEvent({"win_stream"});
        QTRY_VERIFY_WITH_TIMEOUT(!messages.empty(), 1000);
        QVERIFY(QJsonDocument::fromJson(messages.last()[0].toString().toUtf8())
                    .object()["sensors"]
                    .toArray()
                    .size() == 1);
        messages.clear();
        for (int value : {3, 4, 5}) {
            c->catalog["win_stream"]["value"] = value;
            c->publishEvent({"win_stream"});
        }
        auto eventCount = [&] {
            int count = 0;
            for (const auto &message : messages) {
                const auto frame = QJsonDocument::fromJson(message[0].toString().toUtf8()).object();
                for (const auto &reading : frame["sensors"].toArray())
                    if (reading.toObject()["id"] == "win_stream" && reading.toObject()["value"].toInt() >= 3)
                        ++count;
            }
            return count;
        };
        QTRY_VERIFY_WITH_TIMEOUT(eventCount() >= 3, 2000);
        QList<int> values;
        for (const auto &message : messages) {
            const auto frame = QJsonDocument::fromJson(message[0].toString().toUtf8()).object();
            for (const auto &reading : frame["sensors"].toArray())
                if (reading.toObject()["id"] == "win_stream")
                    values.append(reading.toObject()["value"].toInt());
        }
        QCOMPARE(values.mid(0, 3), QList<int>({3, 4, 5}));
        socket.abort();
        c->stopService();
    }

    void modulesHideReadingsRetainSelectionsAndPersist() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.collapseCategories(false);
        local.sampleTimer.stop();
        local.publishTimer.stop();
        local.setBrowserFilters("module fixture", "all", false, false);
        auto windowsRow = row("win_module_fixture", 10);
        windowsRow["name"] = "module fixture";
        auto hwRow = row("hw_module_fixture", 40);
        hwRow["name"] = "module fixture";
        local.mergeReadings("windows", {windowsRow});
        local.mergeReadings("hwinfo", {hwRow});
        local.selected = {"win_module_fixture", "hw_module_fixture"};
        local.setFavourite("hw_module_fixture", true);
        local.setModuleEnabled("hwinfo", false);
        local.setModuleEnabled("windows", false);
        QVERIFY(local.browser().isEmpty());
        QVERIFY(local.snapshot()["sensors"].toArray().isEmpty());
        QVERIFY(local.selected.contains("hw_module_fixture"));
        QVERIFY(local.favourites.contains("hw_module_fixture"));
        QVERIFY(!local.moduleEnabled("windows"));
        QVERIFY(!local.snapshot()["windows_healthy"].toBool());
        QSettings saved(dir.path() + "/test-settings.yaml", SettingsStore::format());
        QVERIFY(!saved.value("modules/windows/enabled", true).toBool());
        QVERIFY(!saved.value("sources/hwinfo", true).toBool());
        local.setModuleEnabled("hwinfo", true);
        QCOMPARE(local.browser().size(), 2);
        QCOMPARE(local.browser().last().toMap()["id"].toString(), "hw_module_fixture");
        local.setModuleEnabled("windows", true);
        QCOMPARE(local.browser().size(), 4);
        local.setPollingInterval("windows", 750);
        local.setPollingInterval("hwinfo", 300);
        QCOMPARE(local.sampleTimer.interval(), 750);
        QCOMPARE(local.hwinfoTimer.interval(), 300);
        QCOMPARE(local.settings.value("sample_seconds").toDouble(), 0.75);
        QCOMPARE(local.settings.value("hwinfo_check_ms").toInt(), 300);
        local.setPollingInterval("hwinfo", 1);
        QCOMPARE(local.hwinfoTimer.interval(), 100);
        local.setModuleEnabled("unknown", false);
        QVERIFY(local.moduleEnabled("windows"));
    }

    void qmlDynamicLighting() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        // Check package permissions without depending on render-thread page transitions.
        local.setAnimationsEnabled(false);
        UpdateManager updater(dir.path(), true);
        local.setLicense("pro");
        local.sampleTimer.stop();
        local.publishTimer.stop();
        local.setDynamicLightingOption("enabled", true);
        local.lighting->data = {
            {"devices", QJsonArray{QJsonObject{{"id", "win_lighting_aaaaaaaaaaaaaaaaaaaaaaaa"},
                                               {"name", "Keyboard fixture"},
                                               {"available", true},
                                               {"lamp_count", 104}}}},
            {"status", "Fixture ready"}};
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 5);
        auto *module = window->findChild<QObject *>("dynamicLightingModule");
        QVERIFY(module);
        QCOMPARE(module->property("title").toString(), QString("Dynamic Lighting"));
        // Portable restrictions are runtime state, not a different build.
        QVERIFY(window->property("portable").toBool());
        QVERIFY(!module->property("available").toBool());
        auto *toggle = module->findChild<QQuickItem *>("submoduleToggle");
        QVERIFY(toggle);
        QVERIFY(!toggle->isEnabled());
        QVERIFY(!toggle->property("checked").toBool());
        window->setProperty("previewPage", 2);
        auto *installButton = window->findChild<QQuickItem *>("installUpdateButton");
        QVERIFY(installButton);
        QVERIFY(!installButton->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(window, "openPortableSettings"));
        QCOMPARE(window->property("previewPage").toInt(), 2);
        auto *portableSection = window->findChild<QObject *>("portableSection");
        QVERIFY(portableSection);
        QVERIFY(portableSection->property("expanded").toBool());
        auto *executable = window->findChild<QObject *>("installationExecutablePath");
        QVERIFY(executable);
        QCOMPARE(executable->property("text").toString(),
                 QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
        auto installedStatus = local.info();
        installedStatus["packaged"] = true;
        // Keep this simulated package identity when the controller next updates.
        QVERIFY(QQmlProperty::write(window, "status", installedStatus));
        QTRY_VERIFY(!window->property("portable").toBool());
        QVERIFY(module->property("available").toBool());
        QVERIFY(toggle->property("checked").toBool());
        QCOMPARE(portableSection->property("title").toString(), QString("Installation"));
        QCOMPARE(executable->property("text").toString(), local.executablePath());
        window->setProperty("previewPage", 5);
        QTRY_VERIFY(toggle->isEnabled());

        std::function<QQuickItem *(QQuickItem *)> findLighting = [&](QQuickItem *parent) -> QQuickItem * {
            if (parent->objectName() == "lightingDevice_win_lighting_aaaaaaaaaaaaaaaaaaaaaaaa")
                return parent;
            for (auto *child : parent->childItems())
                if (auto *found = findLighting(child))
                    return found;
            return nullptr;
        };
        QTRY_VERIFY(findLighting(window->contentItem()) != nullptr);
        QVERIFY(module->property("active").toBool());
        module->setProperty("expanded", true);
        local.setDynamicLightingOption("enabled", false);
        QTRY_VERIFY(!module->property("active").toBool());
    }

    void nativeCrashReportsAndDumpsRespectOptIn_data() {
        QTest::addColumn<bool>("logging");
        QTest::addColumn<bool>("dumps");
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<bool>("expectDump");
        QTest::addColumn<int>("attempts");
        QTest::addColumn<QByteArray>("error");
        QTest::newRow("disabled") << false << false << QString("crash") << false << 0 << QByteArray();
        QTest::newRow("dumps-without-logging") << false << true << QString("crash") << false << 0 << QByteArray();
        QTest::newRow("text-only") << true << false << QString("crash") << false << 0 << QByteArray();
        QTest::newRow("dump-enabled") << true << true << QString("crash") << true << 0 << QByteArray();
        QTest::newRow("transient-partial-copy") << true << true << QString("partial-copy-once") << true << 2 << QByteArray();
        QTest::newRow("persistent-partial-copy") << true << true << QString("partial-copy-always") << false << 3 << QByteArray("0x8007012B");
        QTest::newRow("access-denied") << true << true << QString("denied") << false << 1 << QByteArray("0x80070005");
        QTest::newRow("missing-dump-api") << true << true << QString("missing-api") << false << 0 << QByteArray("0x0000007F");
    }

    void nativeCrashReportsAndDumpsRespectOptIn() {
        QFETCH(bool, logging);
        QFETCH(bool, dumps);
        QFETCH(QString, scenario);
        QFETCH(bool, expectDump);
        QFETCH(int, attempts);
        QFETCH(QByteArray, error);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto marker = [&](const QString &name) {
            QFile file(dir.filePath(name));
            return file.open(QIODevice::WriteOnly);
        };
        if (logging)
            QVERIFY(marker("logging.enabled"));
        if (dumps)
            QVERIFY(marker("dumps.enabled"));
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + "/diagnostic_crash_fixture.exe",
                      {dir.path(), scenario});
        QVERIFY2(process.waitForStarted(3000), qPrintable(process.errorString()));
        QVERIFY2(process.waitForFinished(15000), qPrintable(process.errorString()));
        const auto reports = QDir(dir.path()).entryList({"crash-*.txt"}, QDir::Files);
        const auto dumpFiles = QDir(dir.path()).entryList({"crash-*.dmp"}, QDir::Files);
        auto read = [&](const QString &name) {
            QFile file(dir.filePath(name));
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto report = reports.isEmpty() ? QByteArray() : read(reports.first());
        const auto activity = read("activity.log");
        const auto output = process.readAllStandardError();
        // The temporary profile is deleted on return. Put its safe text evidence
        // in QtTest's failure output, which CI already retains. Never upload dumps.
        const auto detail = QString("Exit=0x%1; reports=%2; dumps=%3; %4")
            .arg(quint32(process.exitCode()), 8, 16, QLatin1Char('0'))
            .arg(reports.size()).arg(dumpFiles.size())
            .arg(QString::fromUtf8(report + activity + output).simplified());
        QVERIFY2(quint32(process.exitCode()) == EXCEPTION_ACCESS_VIOLATION, qPrintable(detail));
        QVERIFY2(reports.size() == (logging ? 1 : 0), qPrintable(detail));
        QVERIFY2(dumpFiles.size() == (expectDump ? 1 : 0), qPrintable(detail));
        QCOMPARE(output.count("Dump attempt "), attempts);
        if (logging) {
            QVERIFY(report.contains("0xC0000005"));
            QVERIFY(report.contains("WindowsHelper"));
            QVERIFY(report.contains("Dynamic Lighting"));
            QVERIFY(report.count("+0x") >= 2);
            QVERIFY(!report.contains(dir.path().toUtf8()));
            QVERIFY(activity.contains("[WindowsHelper][Dynamic Lighting][PID"));
        } else {
            QVERIFY(activity.isEmpty());
        }
        if (!error.isEmpty()) {
            QVERIFY2(report.contains(error), qPrintable(detail));
            QVERIFY2(activity.contains(error), qPrintable(detail));
            QVERIFY(!report.contains("Memory dump: written"));
        }
        if (expectDump) {
            QVERIFY2(report.contains("Memory dump: written"), qPrintable(detail));
            const auto bytes = read(dumpFiles.first());
            QVERIFY(bytes.size() >= qsizetype(sizeof(MINIDUMP_HEADER)));
            MINIDUMP_HEADER header{};
            memcpy(&header, bytes.constData(), sizeof(header));
            QCOMPARE(header.Signature, ULONG32(MINIDUMP_SIGNATURE));
            QCOMPARE(header.Version & 0xffff, ULONG32(MINIDUMP_VERSION));
            QVERIFY(quint64(header.StreamDirectoryRva) + quint64(header.NumberOfStreams) * sizeof(MINIDUMP_DIRECTORY)
                    <= quint64(bytes.size()));
            bool foundException = false;
            for (ULONG32 i = 0; i < header.NumberOfStreams; ++i) {
                MINIDUMP_DIRECTORY stream{};
                memcpy(&stream, bytes.constData() + header.StreamDirectoryRva + i * sizeof(stream), sizeof(stream));
                QVERIFY(quint64(stream.Location.Rva) + stream.Location.DataSize <= quint64(bytes.size()));
                if (stream.StreamType != ExceptionStream)
                    continue;
                QVERIFY(stream.Location.DataSize >= sizeof(MINIDUMP_EXCEPTION_STREAM));
                MINIDUMP_EXCEPTION_STREAM exception{};
                memcpy(&exception, bytes.constData() + stream.Location.Rva, sizeof(exception));
                QCOMPARE(exception.ExceptionRecord.ExceptionCode, ULONG32(EXCEPTION_ACCESS_VIOLATION));
                QCOMPARE(exception.ExceptionRecord.NumberParameters, ULONG32(2));
                foundException = true;
            }
            QVERIFY(foundException);
        }
    }

    void diagnosticActivityRedactsAndLabels() {
        QTemporaryDir dir;
        LocalDiagnostics logs(dir.path());
        QVERIFY(logs.setEnabled(true));
        logs.event("BEACN", "BEACN failed at C:/Users/private/device.log");
        logs.event("Media", "Media command failed with token private-token");
        QTRY_VERIFY(QFileInfo(dir.filePath("activity.log")).size() > 0);
        QTest::qWait(300);
        QFile file(dir.filePath("activity.log"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = file.readAll();
        QVERIFY(data.contains("[WHAT][BEACN][PID"));
        QVERIFY(data.contains("[ERROR]"));
        QVERIFY(!data.contains("Users/private"));
        QVERIFY(!data.contains("private-token"));
        QVERIFY(logs.setEnabled(false));
        const auto size = file.size();
        logs.event("BEACN", "Should not be saved");
        QTest::qWait(250);
        QCOMPARE(file.size(), size);
    }

    void localDiagnosticFilesAreOptInAndBounded() {
        QTemporaryDir dir;
        for (int i = 0; i < 6; ++i) {
            LocalDiagnostics logs(dir.path());
            logs.snapshot({{"should_not_exist", true}});
            if (i == 0)
                QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
            QVERIFY(logs.setEnabled(true));
            logs.snapshot({{"module_enabled", true}});
            QVERIFY(logs.setEnabled(false));
            logs.snapshot({{"should_not_exist", true}});
            QTest::qWait(2);
        }
        const auto files = QDir(dir.path()).entryList({"session-*.log"}, QDir::Files);
        QCOMPARE(files.size(), 5);
        QFile file(dir.filePath(files.last()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto text = file.readAll();
        QVERIFY(text.contains("logging_enabled"));
        QVERIFY(text.contains("Qt"));
        QVERIFY(text.contains("module_enabled"));
        QVERIFY(!text.contains("should_not_exist"));
        QVERIFY(!text.contains(dir.path().toUtf8()));
    }

    void qmlUpdateSigningWarning() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.setAnimationsEnabled(false);
        local.setLicense("pro");
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 0);

        auto *overview = window->findChild<QQuickItem *>("overviewUpdateSigningWarning");
        auto *settings = window->findChild<QQuickItem *>("updateSigningWarning");
        QVERIFY(overview);
        QVERIFY(settings);
        QVERIFY(!overview->isVisible());
        updater.reportInstallFailure("certificate_changed", "Signer changed", "0.0.1-beta.2");
        QTRY_VERIFY(overview->isVisible());
        QVERIFY(overview->property("critical").toBool());
        QVERIFY(overview->property("detail").toString().contains("No uninstall or settings reset"));
        QVERIFY(overview->height() > 0);

        ReleaseUrlCapture links;
        QDesktopServices::setUrlHandler("https", &links, "open");
        const auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("https"); });
        QVERIFY(QMetaObject::invokeMethod(overview, "actionTriggered"));
        QCOMPARE(links.urls, QList<QUrl>{QUrl(
                                 "https://github.com/Chuffnugget/what-windows/releases/tag/v0.0.1-beta.2")});
        QVERIFY(QMetaObject::invokeMethod(window, "openUpdateSettings"));
        QTRY_VERIFY(settings->isVisible());
        QCOMPARE(settings->property("detail"), overview->property("detail"));

        // Untrusted downloads lead to the official release list, never a download-supplied URL.
        updater.reportInstallFailure("signature_untrusted", "Untrusted", "0.0.1-beta.2");
        QVERIFY(QMetaObject::invokeMethod(settings, "actionTriggered"));
        QCOMPARE(links.urls.last(), QUrl("https://github.com/Chuffnugget/what-windows/releases"));
    }

    void qmlCompletedUpdate() {
        const auto savedVersion = QCoreApplication::applicationVersion();
        const auto restoreVersion =
            qScopeGuard([&] { QCoreApplication::setApplicationVersion(savedVersion); });
        QCoreApplication::setApplicationVersion("0.0.1-beta.11");
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.setAnimationsEnabled(false);
        UpdateManager updater(dir.path(), true);
        updater.family = "WHAT.Desktop_fixture";
        const QString attempt = "9e8490c1-67cf-4d10-a3ba-a7949efcf05c";
        QVERIFY(QDir().mkpath(dir.filePath("updates/" + attempt)));
        const QString notes =
            "Changes in this release\n\n- Clearer update progress\n- Installer layout fixes\n- Update confirmation with release notes";
        QVERIFY(UpdateManager::saveUpdateNotice(dir.filePath("updates/pending.json"),
                                                {{"attempt", attempt},
                                                 {"family", updater.family},
                                                 {"previous", "0.0.1-beta.10"},
                                                 {"current", "0.0.1-beta.11"},
                                                 {"notes", notes}}));
        QFile result(dir.filePath("updates/" + attempt + "/result.txt"));
        QVERIFY(result.open(QIODevice::WriteOnly));
        result.write("Installed 0.0.1.11\r\n");
        result.close();
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        updater.checkCompletedUpdate();
        auto *notice = window->findChild<QObject *>("updateAvailablePrompt");
        QVERIFY(notice);
        QTRY_VERIFY(notice->property("opened").toBool());
        QVERIFY(notice->property("completed").toBool());
        QCOMPARE(notice->property("title").toString(), QString("Update Complete"));
        QCOMPARE(notice->property("previousVersion").toString(), QString("0.0.1-beta.10"));
        QCOMPARE(notice->property("version").toString(), QString("0.0.1-beta.11"));
        auto *notesText = window->findChild<QQuickItem *>("completedUpdateNotes");
        QVERIFY(notesText);
        QCOMPARE(notesText->property("text").toString(), notes);
        auto *review = window->findChild<QQuickItem *>("reviewUpdatePrompt");
        QVERIFY(review);
        QCOMPARE(review->property("text").toString(), QString("View release on GitHub"));
        const auto buttonBottom = review->mapToScene(QPointF(0, review->height()));
        QVERIFY(buttonBottom.y() <= window->height());
        if (!qEnvironmentVariableIsEmpty("WHAT_COMPLETED_UPDATE_PREVIEW")) {
            QTest::qWait(200);
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_COMPLETED_UPDATE_PREVIEW")));
        }
        // Automatic update checks must not replace a confirmation awaiting dismissal.
        emit updater.updateAvailable("0.0.1-beta.12");
        QCOMPARE(notice->property("version").toString(), QString("0.0.1-beta.11"));
        ReleaseUrlCapture links;
        QDesktopServices::setUrlHandler("https", &links, "open");
        const auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("https"); });
        QVERIFY(QMetaObject::invokeMethod(review, "clicked"));
        QCOMPARE(links.urls, QList<QUrl>{QUrl(
                                 "https://github.com/Chuffnugget/what-windows/releases/tag/v0.0.1-beta.11")});
        QTRY_VERIFY(!notice->property("visible").toBool());
        QTRY_VERIFY(!QFile::exists(dir.filePath("updates/pending.json")));
        updater.openReleases("../../elsewhere");
        QCOMPARE(links.urls.size(), 1);
        updater.openReleases();
        QCOMPARE(links.urls.last(), QUrl("https://github.com/Chuffnugget/what-windows/releases"));
    }

    void qmlReleaseBrowser() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.setLicense("pro");
        local.setWindowAnimationsEnabled(false);
        local.sampleTimer.stop();
        local.publishTimer.stop();
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 5);
        auto *browser = window->findChild<QObject *>("releaseBrowser");
        QVERIFY(browser);
        // Populate a deterministic catalog; opening this UI test never contacts GitHub.
        updater.setBetaUpdates(true);
        updater.busy = true;
        QVERIFY(QMetaObject::invokeMethod(browser, "open"));
        updater.busy = false;
        updater.releases =
            QJsonArray{QJsonObject{{"tag_name", "v0.7.5-beta.2"},
                                   {"prerelease", true},
                                   {"published_at", "2026-09-19T12:00:00Z"},
                                   {"body", "Beta release notes"}},
                       QJsonObject{{"tag_name", "v0.7.3"}, {"published_at", "2026-09-18T12:00:00Z"}}};
        updater.status("Select a release to view its notes and prepare installation.");
        auto *list = window->findChild<QObject *>("releaseList");
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), 2);
        QVERIFY(browser->property("visible").toBool());
        browser->setProperty("selectedVersion", "0.7.5-beta.2");
        browser->setProperty("selectedNotes",
                             "Beta release notes\n\n- Improvements and fixes\n- Experimental features");
        if (!qEnvironmentVariableIsEmpty("WHAT_UPDATER_PREVIEW")) {
            QTest::qWait(300);
            window->grabWindow().save(qEnvironmentVariable("WHAT_UPDATER_PREVIEW"));
        }
        QVERIFY(QMetaObject::invokeMethod(browser, "close"));
        auto *notice = window->findChild<QObject *>("updateAvailablePrompt");
        QVERIFY(notice);
        emit updater.updateAvailable("0.7.8-beta.2");
        QTRY_VERIFY(notice->property("visible").toBool());
        QCOMPARE(notice->property("version").toString(), QString("0.7.8-beta.2"));
        auto *review = window->findChild<QObject *>("reviewUpdatePrompt");
        QVERIFY(review);
        QVERIFY(QMetaObject::invokeMethod(review, "clicked"));
        QCOMPARE(window->property("previewPage").toInt(), 2);
        auto *section = window->findChild<QObject *>("updatesSection");
        QVERIFY(section);
        QVERIFY(section->property("expanded").toBool());
        auto *scroll = window->findChild<QObject *>("settingsScroll");
        QVERIFY(scroll);
        auto *flick = qvariant_cast<QObject *>(scroll->property("contentItem"));
        QVERIFY(flick);
        QTRY_VERIFY2(flick->property("contentY").toReal() > 0,
                     qPrintable(QString("scroll height=%1 content=%2 y=%3 section y=%4")
                                    .arg(scroll->property("availableHeight").toReal())
                                    .arg(scroll->property("contentHeight").toReal())
                                    .arg(flick->property("contentY").toReal())
                                    .arg(section->property("y").toReal())));
        updater.downloading = true;
        updater.downloadBytes = 50;
        updater.downloadTotal = 100;
        updater.progress = 0.5;
        updater.busy = true;
        emit updater.changed();
        auto *bar = window->findChild<QObject *>("updateDownloadProgress");
        QVERIFY(bar);
        QCOMPARE(bar->property("value").toReal(), 0.5);
        QVERIFY(!bar->property("indeterminate").toBool());
        if (!qEnvironmentVariableIsEmpty("WHAT_UPDATER_PREVIEW")) {
            QTest::qWait(300);
            window->grabWindow().save(qEnvironmentVariable("WHAT_UPDATER_PREVIEW") + ".download.png");
        }
        updater.downloading = false;
        emit updater.changed();
        QVERIFY(bar->property("indeterminate").toBool());
        window->setProperty("previewPage", 5);
        QVERIFY(QMetaObject::invokeMethod(window, "openModule", Q_ARG(QVariant, "beacn")));
        auto *beacn = window->findChild<QObject *>("beacnModuleCard");
        QVERIFY(beacn);
        QVERIFY(beacn->property("expanded").toBool());
        auto *wallpaper = window->findChild<QObject *>("wallpaperModuleCard");
        auto *expand = window->findChild<QObject *>("wallpaperExpand");
        QVERIFY(wallpaper && expand);
        QVERIFY(QMetaObject::invokeMethod(expand, "clicked"));
        QVERIFY(!beacn->property("expanded").toBool());
        QVERIFY(wallpaper->property("expanded").toBool());
        QVERIFY(QMetaObject::invokeMethod(expand, "clicked"));
        QVERIFY(!wallpaper->property("expanded").toBool());
        QVERIFY(QMetaObject::invokeMethod(notice, "close"));
        window->hide();
    }

    void moduleOrderPersists() {
        QTemporaryDir dir;
        QStringList expected;
        {
            Controller local(dir.path(), true);
            const auto defaults = local.moduleOrder();
            QCOMPARE(defaults.size(), 9);
            expected = defaults;
            expected.removeAll("haweb");
            expected.prepend("haweb");
            QSignalSpy changes(&local, &Controller::moduleOrderChanged);
            local.setModuleOrder({"haweb", "unknown", "haweb"});
            QCOMPARE(local.moduleOrder(), expected);
            QCOMPARE(changes.size(), 1);
            local.setModuleOrder(expected);
            QCOMPARE(changes.size(), 1);
            local.settings.sync();
            QSettings disk(local.settings.fileName(), SettingsStore::format());
            QCOMPARE(disk.value("ui/module_order").toStringList(), expected);
            const auto backup = ConfigurationFile::decode(
                ConfigurationFile::encode(ConfigurationFile::snapshot(disk)));
            QVERIFY2(backup.error.isEmpty(), qPrintable(backup.error));
            QCOMPARE(backup.values["ui/module_order"].toArray(), QJsonArray::fromStringList(expected));
        }
        Controller restarted(dir.path(), true);
        QCOMPARE(restarted.moduleOrder(), expected);
    }

    void qmlModuleReordering_data() {
        QTest::addColumn<bool>("overview");
        QTest::newRow("overview") << true;
        QTest::newRow("modules") << false;
    }

    void qmlModuleReordering() {
        QFETCH(bool, overview);
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        local.setAnimationsEnabled(false);
        local.setLicense("pro");
        UpdateManager updater(dir.path(), true);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const int pageIndex = overview ? 0 : 5;
        window->setProperty("previewPage", pageIndex);
        auto *page = visualItem(window->contentItem(), overview ? "overviewReorderArea" : "modulesReorderArea");
        auto *beacn = visualItem(window->contentItem(), overview ? "overview-beacn" : "beacnModuleCard");
        auto *wallpaper = visualItem(window->contentItem(), overview ? "overview-wallpaper" : "wallpaperModuleCard");
        auto *grip = visualItem(window->contentItem(), overview ? "overview-beacnReorderGrip" : "beacnReorderGrip");
        auto *scroll = visualItem(window->contentItem(), overview ? "overviewScroll" : "modulesScroll");
        QVERIFY(page && beacn && wallpaper && grip && scroll);
        QTRY_VERIFY(grip->isVisible() && wallpaper->y() > beacn->y());
        window->grabWindow();
        auto *flick = scroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(flick);
        const auto maximum = qMax(0.0, flick->property("contentHeight").toDouble() - flick->height());
        flick->setProperty("contentY", qBound(0.0,
            flick->property("contentY").toDouble() + beacn->mapToItem(flick, QPointF()).y() - 16, maximum));
        window->grabWindow();
        const auto original = local.moduleOrder();
        QSignalSpy saved(&local, &Controller::moduleOrderChanged);
        auto start = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2)).toPoint();
        const auto end = wallpaper->mapToScene(QPointF(24, wallpaper->height() - 5)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, start + QPoint(0, 24), 20);
        QTest::mouseMove(window, end, 20);
        QTRY_VERIFY(page->property("dragging").toBool());
        QCOMPARE(local.moduleOrder(), original); // No saves or card recreation while moving.
        QCOMPARE(saved.size(), 0);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
        QTRY_COMPARE(local.moduleOrder()[0], QString("wallpaper"));
        QCOMPARE(local.moduleOrder()[1], QString("beacn"));
        QCOMPARE(saved.size(), 1);
        QTRY_VERIFY(wallpaper->y() < beacn->y());
        QCOMPARE(window->property("previewPage").toInt(), pageIndex);
        if (!overview)
            QVERIFY(!beacn->property("expanded").toBool());
        QCOMPARE(visualItem(window->contentItem(), beacn->objectName()), beacn);

        // Keyboard access uses the same persisted order.
        grip->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Up, Qt::AltModifier);
        QTRY_COMPARE(local.moduleOrder(), original);
        QTRY_VERIFY(beacn->y() < wallpaper->y());
        window->grabWindow();
        start = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, start + QPoint(0, 24), 20);
        QTRY_VERIFY(page->property("dragging").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start + QPoint(0, 24));
        QCOMPARE(local.moduleOrder(), original);
        QVERIFY(!page->property("dragging").toBool());

        // Edge scrolling must work while the grip owns the pointer grab.
        const auto scrollBefore = flick->property("contentY").toDouble();
        window->grabWindow();
        start = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2)).toPoint();
        const auto edge = scroll->mapToScene(QPointF(24, scroll->height() - 8)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, start + QPoint(0, 24), 20);
        QTest::mouseMove(window, edge, 20);
        QTRY_VERIFY(flick->property("contentY").toDouble() > scrollBefore);
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, edge);
        QCOMPARE(local.moduleOrder(), original);
        flick->setProperty("contentY", scrollBefore);
        if (qEnvironmentVariableIsSet("WHAT_MODULE_REORDER_PREVIEW")) {
            window->grabWindow();
            QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_MODULE_REORDER_PREVIEW")
                + (overview ? "/overview-grips.png" : "/module-grips.png")));
        }
        QCOMPARE(warnings.size(), 0);
    }

    void qmlModuleSwitchesSearchAndAutomaticSaving_data() {
        QTest::addColumn<int>("windowHeight");
        QTest::addColumn<int>("windowWidth");
        QTest::newRow("compact-desktop") << 700 << 1000;
        QTest::newRow("normal-desktop") << 884 << 1160;
    }

    void qmlModuleSwitchesSearchAndAutomaticSaving() {
        QTemporaryDir dir;
        Controller local(dir.path(), true);
        UpdateManager updater(dir.path(), true);
        local.setLicense("pro");
        local.sampleTimer.stop();
        local.publishTimer.stop();
        QTcpServer initialPort;
        QVERIFY(initialPort.listen(QHostAddress::LocalHost, 0));
        const int firstPort = initialPort.serverPort();
        initialPort.close();
        local.saveSettings(1, 5, firstPort, "pro", false);
        QVERIFY(local.startService());
        local.setWindowAnimationsEnabled(
            false); // This fixture tests controls; window motion has its own test.
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        // Native runners must expose the window so QML polish/layout can run.
        window->setPosition(0, 0);
        // Exercise clipping independently of the runner's desktop resolution.
        QFETCH(int, windowHeight);
        QFETCH(int, windowWidth);
        window->setMinimumHeight(0);
        window->resize(windowWidth, windowHeight);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->setProperty("previewPage", 5);
        std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual =
            [&](QQuickItem *parent, const QString &name) -> QQuickItem * {
            if (parent->objectName() == name)
                return parent;
            for (auto *child : parent->childItems())
                if (auto *found = findVisual(child, name))
                    return found;
            return nullptr;
        };
        auto item = [&](const char *name) {
            auto *found = window->findChild<QQuickItem *>(name);
            return found ? found : findVisual(window->contentItem(), QString::fromLatin1(name));
        };
        auto click = [&](const char *name) {
            auto *target = item(name);
            if (!target)
                return false;
            QPoint point;
            // Layout and scroll geometry can settle over several frames, especially
            // on the release runner. Wait for a hittable position, not a fixed delay.
            const bool ready = QTest::qWaitFor(
                [&] {
                    window->grabWindow();
                    auto *scroll = item("modulesScroll");
                    auto *flick = scroll ? scroll->property("contentItem").value<QQuickItem *>() : nullptr;
                    const QPointF center(target->width() / 2, target->height() / 2);
                    if (flick && flick->isAncestorOf(target)) {
                        const qreal origin = flick->property("originY").toReal();
                        const qreal maximum =
                            origin +
                            qMax(qreal(0), flick->property("contentHeight").toReal() - flick->height());
                        const qreal centerY = target->mapToItem(flick, center).y();
                        flick->setProperty("contentY", qBound(origin,
                                                              flick->property("contentY").toReal() + centerY -
                                                                  flick->height() / 2,
                                                              maximum));
                        window->grabWindow();
                        if (!flick->contains(target->mapToItem(flick, center)))
                            return false;
                    }
                    point = target->mapToScene(center).toPoint();
                    return target->width() > 0 && target->height() > 0 && target->isVisible() &&
                           target->isEnabled() && window->contentItem()->contains(point);
                },
                3000);
            if (!ready) {
                for (auto *ancestor = target; ancestor; ancestor = ancestor->parentItem())
                    qWarning() << ancestor->metaObject()->className() << ancestor->objectName()
                               << ancestor->size() << ancestor->position();
                auto *scroll = item("modulesScroll");
                auto *flick = scroll ? scroll->property("contentItem").value<QQuickItem *>() : nullptr;
                qWarning() << name << "not hittable after layout/scroll" << "point" << point << "window"
                           << window->size() << "target" << target->size() << "viewport"
                           << (flick ? flick->size() : QSizeF()) << "contentY"
                           << (flick ? flick->property("contentY") : QVariant()) << "contentHeight"
                           << (flick ? flick->property("contentHeight") : QVariant());
                return false;
            }
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
            return true;
        };
        QTest::qWait(150);
        qInfo("UI stage: loaded");
        QVERIFY(item("beacnModuleCard"));
        QVERIFY(item("beacnModuleToggle"));
        {
            ReleaseUrlCapture links;
            QDesktopServices::setUrlHandler("https", &links, "open");
            const auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("https"); });
            QVERIFY(click("requestModule"));
            QCOMPARE(links.urls.size(), 1);
            QCOMPARE(links.urls.first().path(), QString("/Chuffnugget/what-windows/issues/new"));
            if (!qEnvironmentVariableIsEmpty("WHAT_MODULE_REQUEST_PREVIEW"))
                QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_MODULE_REQUEST_PREVIEW")));
        }
        window->setProperty("previewPage", 1);
        QVERIFY(item("coretempFilter"));
        item("coretempFilter")->setProperty("checked", false);
        QVERIFY(QMetaObject::invokeMethod(window, "applyFilters"));
        QVERIFY(local.info()["browserSource"].toString() != "all");
        QVERIFY(!local.info()["browserSource"].toString().contains("coretemp"));
        item("coretempFilter")->setProperty("checked", true);
        QVERIFY(QMetaObject::invokeMethod(window, "applyFilters"));
        window->setProperty("previewPage", 5);
        QVERIFY(item("coretempModuleToggle"));
        QVERIFY(click("coretempModuleToggle"));
        QTRY_VERIFY(local.moduleEnabled("coretemp"));
        QVERIFY(click("coretempModuleToggle"));
        QTRY_VERIFY(!local.moduleEnabled("coretemp"));
        QVERIFY(item("windowsModuleToggle"));
        QVERIFY(item("hwinfoModuleToggle"));
        QVERIFY(click("windowsModuleToggle"));
        QTRY_VERIFY(!local.moduleEnabled("windows"));
        qInfo("UI stage: Windows disabled");
        QVERIFY(click("hwinfoModuleToggle"));
        QTRY_VERIFY(!local.moduleEnabled("hwinfo"));
        window->setProperty("previewPage", 1);
        QTest::qWait(100);
        qInfo("UI stage: both disabled");
        QTRY_COMPARE(item("sensorList")->property("count").toInt(), 0);
        QTRY_VERIFY(!item("windowsFilter")->isVisible());
        QTRY_VERIFY(!item("hwinfoFilter")->isVisible());
        QCOMPARE(local.browserModel.rowCount(), 0);
        window->setProperty("previewPage", 5);
        auto *search = item("moduleSearch");
        QVERIFY(search);
        search->setProperty("text", "notification");
        QTRY_VERIFY(!item("hwinfoModuleCard")->isVisible());
        QVERIFY(item("windowsModuleCard")->isVisible());
        QVERIFY(!item("windowsModuleCard")->property("expanded").toBool());
        if (!qEnvironmentVariableIsEmpty("WHAT_UI_PREVIEW")) {
            QTest::qWait(250);
            window->grabWindow().save(qEnvironmentVariable("WHAT_UI_PREVIEW"));
        }
        qInfo("UI stage: search filtered without expansion");
        search->setProperty("text", "");
        QVERIFY(click("windowsModuleToggle"));
        QTRY_VERIFY(local.moduleEnabled("windows"));
        QVERIFY(click("windowsHeader"));
        QTRY_VERIFY(item("windowsModuleCard")->property("expanded").toBool());
        QVERIFY(click("windowsHeader"));
        QTRY_VERIFY(!item("windowsModuleCard")->property("expanded").toBool());
        QVERIFY(local.moduleEnabled("windows"));
        QVERIFY(click("windowsExpand"));
        QTRY_VERIFY(item("windowsModuleCard")->property("expanded").toBool());
        QTest::qWait(200);
        auto *scroll = item("modulesScroll");
        QVERIFY(scroll);
        auto *flick = scroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(flick);
        // Offscreen windows need rendered frames to advance the reveal animation.
        QTRY_VERIFY_WITH_TIMEOUT(
            (window->grabWindow(), flick->property("contentHeight").toReal() - flick->height() >= 180), 2000);
        flick->setProperty("contentY", 0);
        // Wheel over the viewport, even when the Windows header is below its fold.
        const QPointF wheelPoint = flick->mapToScene(QPointF(flick->width() / 2, flick->height() / 2));
        QWheelEvent wheel(wheelPoint, window->mapToGlobal(wheelPoint.toPoint()), QPoint(), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(window, &wheel);
        QTRY_COMPARE(flick->property("contentY").toInt(), 180);
        flick->setProperty("contentY", 0);
        auto *polling = item("windowsPolling");
        QVERIFY(polling);
        polling->setProperty("value", 1500);
        QVERIFY(QMetaObject::invokeMethod(polling, "valueModified"));
        QTRY_COMPARE(local.sampleTimer.interval(), 1500);
        auto *slider = item("windowsPollingSlider");
        QVERIFY(slider);
        QCOMPARE(slider->property("from").toInt(), 1000);
        QCOMPARE(slider->property("to").toInt(), 10000);
        slider->setProperty("value", 2500);
        QVERIFY(QMetaObject::invokeMethod(slider, "moved"));
        QTRY_COMPARE(local.sampleTimer.interval(), 2500);
        polling->setProperty("value", 30000);
        QVERIFY(QMetaObject::invokeMethod(polling, "valueModified"));
        QTRY_COMPARE(local.sampleTimer.interval(), 30000);
        QCOMPARE(polling->property("value").toInt(), 30000);
        polling->setProperty("value", 500);
        QVERIFY(QMetaObject::invokeMethod(polling, "valueModified"));
        QTRY_COMPARE(local.sampleTimer.interval(), 500);
        QVERIFY(!item("updateBadge")->isVisible());
        updater.available = "99.0.0";
        emit updater.changed();
        QTRY_VERIFY(item("updateBadge")->isVisible());
        if (!qEnvironmentVariableIsEmpty("WHAT_UI_SLIDERS")) {
            QTest::qWait(200);
            window->grabWindow().save(qEnvironmentVariable("WHAT_UI_SLIDERS"));
        }
        QVERIFY(click("updateBadge"));
        QCOMPARE(window->property("previewPage").toInt(), 2);
        updater.available.clear();
        emit updater.changed();
        QTRY_VERIFY(!item("updateBadge")->isVisible());
        // Compact coverage above targets module scrolling; run the remaining
        // sidebar/settings checks at the regular application height.
        window->resize(window->width(), 884);
        QTRY_VERIFY(window->contentItem()->contains(
            item("settingsNavigation")
                ->mapToScene(QPointF(item("settingsNavigation")->width() / 2,
                                     item("settingsNavigation")->height() / 2))));
        QVERIFY(click("settingsNavigation"));
        QCOMPARE(window->property("previewPage").toInt(), 2);
        auto *port = item("portInput");
        QVERIFY(port);
        const int oldPort = local.activePort;
        port->setProperty("text", "12");
        QVERIFY(QMetaObject::invokeMethod(port, "textEdited"));
        QTest::qWait(750);
        QCOMPARE(local.activePort, oldPort);
        auto *heartbeat = item("heartbeatInput");
        QVERIFY(heartbeat);
        heartbeat->setProperty("value", 7);
        QVERIFY(QMetaObject::invokeMethod(heartbeat, "valueModified"));
        QTRY_COMPARE(local.publishTimer.interval(), 7000);
        auto *heartbeatSlider = item("heartbeatInput");
        QVERIFY(heartbeatSlider);
        heartbeatSlider->setProperty("value", 15);
        QVERIFY(QMetaObject::invokeMethod(heartbeatSlider, "valueModified"));
        QTRY_COMPARE(local.publishTimer.interval(), 15000);
        heartbeat->setProperty("value", 120);
        QVERIFY(QMetaObject::invokeMethod(heartbeat, "valueModified"));
        QTRY_COMPARE(local.publishTimer.interval(), 120000);
        QCOMPARE(local.activePort, oldPort);
        QTcpServer freePort;
        QVERIFY(freePort.listen(QHostAddress::LocalHost, 0));
        const int nextPort = freePort.serverPort();
        freePort.close();
        port->setProperty("text", QString::number(nextPort));
        QVERIFY(QMetaObject::invokeMethod(port, "textEdited"));
        QTRY_COMPARE(local.activePort, nextPort);
        QVERIFY(local.server.isListening());
        QCOMPARE(local.settings.value("port").toInt(), nextPort);
        window->setProperty("previewPage", 5);
        auto *moduleQuery = item("moduleSearch");
        QVERIFY(moduleQuery);
        moduleQuery->setProperty("text", "commands");
        auto *commandModule = item("commandsSubmodule");
        QVERIFY(commandModule);
        QTRY_VERIFY(commandModule->isVisible());
        QVERIFY(QMetaObject::invokeMethod(commandModule, "toggled", Q_ARG(bool, true)));
        QVERIFY(local.controlPermissions()["shell"].toBool());
        local.appendCommandStream("powershell> echo fixture\nfixture\n");
        auto *stream = item("commandStream");
        QVERIFY(stream);
        QTRY_VERIFY(stream->property("text").toString().contains("fixture"));
        if (!qEnvironmentVariableIsEmpty("WHAT_COMMANDS_PREVIEW")) {
            QTest::qWait(250);
            window->grabWindow().save(qEnvironmentVariable("WHAT_COMMANDS_PREVIEW"));
        }
        QVERIFY(QMetaObject::invokeMethod(commandModule, "toggled", Q_ARG(bool, false)));
        QVERIFY(!local.controlPermissions()["shell"].toBool());
        window->hide();
        qInfo("UI stage: finished");
    }

    void cleanupTestCase() {
        c.reset();
    }
};

void ControllerTests::beacnFileVerificationExplainsFailure() {
    QTemporaryDir dir;
    const auto app = dir.filePath("BEACN.exe");
    const auto driver = dir.filePath("BEACNUsbAudioapi_x64.dll");
    auto files = discoverInstalledFiles(app, driver, false);
    QVERIFY(!files["appValid"].toBool());
    QVERIFY(!files["driverValid"].toBool());
    QVERIFY(files["status"].toString().contains("BEACN app was not found"));
    QVERIFY(files["status"].toString().contains("BEACN driver API was not found"));
    for (const auto &path : {app, driver}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("unrecognised vendor build");
    }
    files = discoverInstalledFiles(app, driver, false);
    QVERIFY(!compatibleBeacnAppFile(app));
    QVERIFY(!verifiedDriverFile(driver));
    QVERIFY(beacnFileVersion(app).isEmpty());
    QVERIFY(files["status"].toString().contains("BEACN app is not supported"));
    QVERIFY(files["status"].toString().contains("BEACN driver API is not supported"));
    QVERIFY(files["status"].toString().contains("1.4.2.12"));
    QVERIFY(!files["status"].toString().contains("was not found"));
    QVERIFY(!files["appSha256"].toString().isEmpty());
    QVERIFY(!files["driverSha256"].toString().isEmpty());
}

void ControllerTests::webReadinessAndShellWarningColours() {
    QQmlApplicationEngine engine;
    QSignalSpy warnings(&engine, &QQmlEngine::warnings);
    engine.loadData(R"QML(
        import QtQuick
        import "qrc:/qml/controls"
        import "qrc:/qml/modules"
        import "qrc:/qml/modules/ModuleStatus.js" as ModuleStatus
        Window {
            width: 700; height: 500; visible: true
            QtObject {
                id: controller
                property var info: ({moduleIssues: {}})
                property var homeAssistantWeb: ({enabled: true, ready: false, loading: false,
                                                url: 'https://homeassistant.example'})
            }
            property color accent: Theme.accent
            property color error: Theme.error
            property color warning: Theme.warning
            property color readingColour: Theme.permissionReading
            property color muted: Theme.muted
            property color webColour: ModuleStatus.statusColour(controller, 'haweb', Theme)
            property string webStatus: ModuleStatus.connectionText(controller, 'haweb')
            function failWeb() { controller.info = {moduleIssues: {haweb: 'Browser failed'}}; }
            PermissionBadge {
                objectName: 'shellBadge'
                access: PermissionBadge.Shell
                text: 'Home Assistant Shell Access'
            }
            Row {
                y: 30; spacing: 8
                PermissionBadge { objectName: 'readingBadge'; access: PermissionBadge.Readings; text: 'LOCAL' }
                PermissionBadge { objectName: 'cloudBadge'; access: PermissionBadge.Cloud; text: 'CLOUD' }
                PermissionBadge { objectName: 'controlBadge'; access: PermissionBadge.Controls; text: 'Can Run' }
                PermissionBadge { objectName: 'blockedBadge'; access: PermissionBadge.Blocked; text: 'Blocked' }
            }
            BeacnSettings {
                y: 80; width: 680
                controller: QtObject {
                    signal changed()
                    property var info: ({moduleIssues: {}})
                    property var beacn: ({enabled: true, ready: false, status: 'Unsupported app',
                        installation: {appVersion: '1.4.2.12', driverVersion: '6.16.0.0'}})
                }
                collapsed: false
            }
        }
    )QML");
    QVERIFY(!engine.rootObjects().isEmpty());
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QCOMPARE(window->property("webStatus").toString(), QString("Ready to Open"));
    QCOMPARE(window->property("webColour").value<QColor>(), window->property("accent").value<QColor>());
    auto *badge = window->findChild<QObject *>("shellBadge");
    QVERIFY(badge);
    QCOMPARE(badge->property("foreground").value<QColor>(), window->property("error").value<QColor>());
    for (const auto &role : {qMakePair("readingBadge", "readingColour"), qMakePair("cloudBadge", "warning"),
                             qMakePair("controlBadge", "warning"), qMakePair("blockedBadge", "muted")}) {
        auto *access = window->findChild<QObject *>(role.first);
        QVERIFY(access);
        QCOMPARE(access->property("foreground").value<QColor>(), window->property(role.second).value<QColor>());
    }
    auto *label = badge->property("contentItem").value<QObject *>();
    QVERIFY(label);
    QCOMPARE(label->property("text").toString(), badge->property("text").toString());
    auto *versions = window->findChild<QObject *>("beacnVersions");
    QVERIFY(versions);
    QVERIFY(versions->property("text").toString().contains("1.4.2.12"));
    QVERIFY(versions->property("text").toString().contains("6.16.0.0"));
    QVERIFY(QMetaObject::invokeMethod(window, "failWeb"));
    QCOMPARE(window->property("webColour").value<QColor>(), window->property("warning").value<QColor>());
    QCOMPARE(warnings.count(), 0);
}

void ControllerTests::applicationDialogsKeepModalOwnership() {
    QTemporaryDir dir;
    Controller local(dir.path(), true);
    UpdateManager updater(dir.path(), true);
    local.setAnimationsEnabled(false);
    QQmlApplicationEngine engine;
    QSignalSpy warnings(&engine, &QQmlEngine::warnings);
    engine.setInitialProperties(
        {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
    engine.load(QUrl("qrc:/qml/Main.qml"));
    QVERIFY(!engine.rootObjects().isEmpty());
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QVERIFY(!window->property("webModalOpen").toBool());

    for (const auto *name : {"trayQuitConfirmation", "entityCleanupDialog", "clearWebDataDialog",
                             "configurationImportDialog", "uiTimingDialog", "certificateDialog",
                             "wallpaperControlsDialog", "hwinfoSetupDialog", "diagnosticReportDialog",
                             "pairingTokenDialog"}) {
        auto *dialog = window->findChild<QObject *>(name);
        QVERIFY2(dialog, name);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        // Wallpaper controls are non-modal and must not block the web view.
        QVERIFY2(window->property("webModalOpen").toBool() == dialog->property("modal").toBool(), name);
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY2(!window->property("webModalOpen").toBool(), name);
    }

    // Closing one prompt must not reveal the native browser beneath another.
    auto *quit = window->findChild<QObject *>("trayQuitConfirmation");
    auto *certificate = window->findChild<QObject *>("certificateDialog");
    QVERIFY(QMetaObject::invokeMethod(quit, "open"));
    QVERIFY(QMetaObject::invokeMethod(certificate, "open"));
    QTRY_VERIFY(quit->property("opened").toBool() && certificate->property("opened").toBool());
    QVERIFY(QMetaObject::invokeMethod(quit, "close"));
    QTRY_VERIFY(!quit->property("visible").toBool());
    QVERIFY(window->property("webModalOpen").toBool());
    QVERIFY(QMetaObject::invokeMethod(certificate, "close"));
    QTRY_VERIFY(!certificate->property("visible").toBool());
    QVERIFY(!window->property("webModalOpen").toBool());
    QCOMPARE(warnings.count(), 0);
    window->hide();
}

void ControllerTests::moduleWarningsFollowHealth() {
    if (Architecture::arm64)
        QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
    QTemporaryDir dir;
    Controller local(dir.path(), true);
    local.sampleTimer.stop();
    local.hwinfoTimer.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!local.hwinfoJob.isRunning() && !local.windowsJob.isRunning()
                             && !local.slowJob.isRunning() && !local.inventoryJob.isRunning(), 15000);
    for (auto module : {"beacn", "wallpaper", "icue", "coretemp", "nvidia"})
        local.settings.setValue(QString("modules/%1/enabled").arg(module), true);
    local.latest = {};
    local.beacnData = {{"ready", false}, {"discovering", true}};
    local.wallpaperData = {};
    local.coretempClock.invalidate();
    local.nvidiaClock.invalidate();
    local.icueClock.invalidate();
    local.automationClock.invalidate();
    local.workerErrors.clear();
    QVERIFY(local.moduleIssues().isEmpty()); // No completed first probes yet.

    local.latest.status = "Shared memory access denied";
    local.coretempData = {{"connected", false}, {"status", "Core Temp is not running"}};
    local.coretempClock.start();
    local.nvidiaData = {{"connected", false}, {"status", "Driver unavailable"}};
    local.nvidiaClock.start();
    local.icueData = {{"connected", true}};
    local.icueClock.start();
    local.settings.setValue("modules/icue/automation", true);
    local.automationData = {{"connected", false}, {"status", "Connection refused"}};
    local.automationClock.start();
    local.beacnData = {{"ready", false}, {"status", "No BEACN device"}};
    local.wallpaperData = {{"running", true}, {"error", "Query timed out"}, {"status", "Query timed out"}};
    local.workerErrors["Windows sensors"] = "Worker timed out";
    const auto issues = local.moduleIssues();
    QCOMPARE(issues.size(), 7);
    QVERIFY(issues["hwinfo"].toString().contains("Shared memory access denied"));
    QCOMPARE(issues["coretemp"].toString(), "Core Temp is not running");
    QCOMPARE(issues["nvidia"].toString(), "Driver unavailable");
    QCOMPARE(issues["icue"].toString(), "Automation: Connection refused");
    QCOMPARE(issues["beacn"].toString(), "No BEACN device");
    QCOMPARE(issues["wallpaper"].toString(), "Query timed out");
    QVERIFY(issues["windows"].toString().contains("Worker timed out"));
    QCOMPARE(local.info()["moduleIssues"].toMap(), issues);
    local.beacnData["ready"] = true;
    local.beacnData["compatibility_warning"] = "Working on an unsupported BEACN build; live checks passed.";
    QCOMPARE(local.moduleIssues()["beacn"].toString(), local.beacnData["compatibility_warning"].toString());
    local.beacnData.remove("compatibility_warning");
    QVERIFY(!local.moduleIssues().contains("beacn"));

    // Keep real query errors separate from readings an API never supported.
    local.nvidiaData = {{"connected", true}, {"devices", QJsonArray{
        QJsonObject{{"name", "GPU"}, {"unsupported", QJsonArray{"Fan speed"}},
                    {"errors", QJsonArray{"Temperature: GPU lost"}}}}}};
    QCOMPARE(local.moduleIssues()["nvidia"].toString(), "GPU: Temperature: GPU lost");
    local.nvidiaData["devices"] = QJsonArray{QJsonObject{{"unsupported", QJsonArray{"Fan speed"}}}};
    QVERIFY(!local.moduleIssues().contains("nvidia"));
    local.latest.valid = true;
    local.initialAge = 0;
    local.pollClock.start();
    local.coretempData["connected"] = true;
    local.automationData["connected"] = true;
    local.beacnData["ready"] = true;
    local.wallpaperData.remove("error");
    local.workerErrors.clear();
    QVERIFY(local.moduleIssues().isEmpty());
    local.initialAge = 1000;
    QVERIFY(local.moduleIssues()["hwinfo"].toString().contains("stale"));
    local.settings.setValue("sources/hwinfo", false);

    local.settings.setValue("sources/media", true);
    local.mediaClock.start();
    local.catalog.insert("win_media_status", {{"value", "No media session"}});
    QVERIFY(local.moduleIssues().isEmpty());
    local.catalog.insert("win_media_status", {{"value", "Media API unavailable or access denied"}});
    QVERIFY(local.moduleIssues()["windows"].toString().contains("access denied"));
    local.settings.setValue("sources/media", false);
    QVERIFY(local.moduleIssues().isEmpty());

    local.workerErrors["Windows sensors"] = "Failed";
    local.coretempData["connected"] = false;
    local.settings.setValue("modules/windows/enabled", false);
    local.settings.setValue("modules/coretemp/enabled", false);
    QVERIFY(local.moduleIssues().isEmpty());
    local.homeAssistantWeb->setEnabled(true);
    QVERIFY(local.moduleIssues()["haweb"].toString().contains("dashboard address"));
    QVERIFY(local.homeAssistantWeb->configure("https://example.test", "windowed", 100, ""));
    QVERIFY(!local.homeAssistantWeb->ready());
    QVERIFY(!local.moduleIssues().contains("haweb")); // Configured, but not opened.

    auto *custom = local.customSources;
    custom->setEnabled(true);
    auto definition = custom->newDefinition("action");
    definition["name"] = "Warning fixture";
    definition["shell"] = "cmd";
    definition["command"] = "exit /b 1";
    QVERIFY(!custom->save(definition).contains("error"));
    QSignalSpy finished(custom, &CustomModule::actionFinished);
    custom->runAction(definition["id"].toString(), "fixture");
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(local.moduleIssues()["custom"].toString().contains("Warning fixture"));
    definition["enabled"] = false;
    QVERIFY(!custom->save(definition).contains("error"));
    QVERIFY(!local.moduleIssues().contains("custom"));
    definition["enabled"] = true;
    definition["command"] = "exit /b 0";
    QVERIFY(!custom->save(definition).contains("error"));
    custom->runAction(definition["id"].toString(), "fixture");
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
    QVERIFY(!local.moduleIssues().contains("custom"));
}

void ControllerTests::moduleWarningsOnBothPages() {
    if (Architecture::arm64)
        QSKIP("This test exercises enabled vendor modules unavailable in native ARM64 WHAT.");
    QTemporaryDir dir;
    Controller local(dir.path(), true);
    local.stopService();
    local.setAnimationsEnabled(false);
    local.setLicense("pro");
    local.settings.setValue("modules/coretemp/enabled", true);
    local.acceptCoreTemp({{"connected", false}, {"status", "Core Temp shared memory access denied"}});
    UpdateManager updater(dir.path(), true);
    QQmlApplicationEngine engine;
    QSignalSpy warnings(&engine, &QQmlEngine::warnings);
    engine.setInitialProperties(
        {{"controller", QVariant::fromValue(&local)}, {"updater", QVariant::fromValue(&updater)}});
    engine.load(QUrl("qrc:/qml/Main.qml"));
    QVERIFY(!engine.rootObjects().isEmpty());
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    window->resize(1100, 820);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto item = [&](const QString &name) { return visualItem(window->contentItem(), name); };
    window->setProperty("previewPage", 0);
    QTRY_VERIFY(item("overview-coretempWarning") && item("overview-coretempWarning")->isVisible());
    auto *overviewWarning = item("overview-coretempWarning");
    auto *overviewRow = item("overview-coretemp");
    QVERIFY(item("overview-haweb"));
    QVERIFY(overviewWarning->height() > 50);
    QVERIFY(overviewWarning->y() + overviewWarning->height() <= overviewRow->height());
    const auto message = overviewWarning->property("detail").toString();
    QVERIFY(message.contains("shared memory access denied"));
    QVERIFY(QMetaObject::invokeMethod(overviewWarning, "actionTriggered"));
    QTRY_VERIFY(item("coretempModuleWarning") && item("coretempModuleWarning")->isVisible());
    auto *card = item("coretempModuleCard");
    auto *moduleWarning = item("coretempModuleWarning");
    QCOMPARE(moduleWarning->property("detail").toString(), message);
    QTRY_VERIFY(card->property("expanded").toBool());
    card->setProperty("collapsed", true);
    QTRY_VERIFY(!card->property("expanded").toBool());
    QVERIFY(moduleWarning->isVisible());
    QTRY_VERIFY(moduleWarning->y() + moduleWarning->height() <= card->height());
    if (!qEnvironmentVariableIsEmpty("WHAT_MODULE_WARNING_PREVIEW")) {
        QTest::qWait(300);
        QVERIFY(window->grabWindow().save(qEnvironmentVariable("WHAT_MODULE_WARNING_PREVIEW")));
    }

    // Recovery updates the existing row and removes both warnings without navigation.
    local.acceptCoreTemp({{"connected", true}, {"status", "Connected"}});
    QTRY_VERIFY(!moduleWarning->isVisible());
    window->setProperty("previewPage", 0);
    QTRY_VERIFY(!overviewWarning->isVisible());
    QCOMPARE(item("overview-coretemp"), overviewRow);
    local.acceptCoreTemp({{"connected", false}, {"status", "Core Temp stopped"}});
    QTRY_VERIFY(overviewWarning->isVisible());
    QVERIFY(local.moduleIssues().contains("coretemp"));
    local.acceptCoreTemp({{"connected", false}, {"status", "Core Temp stopped"}});
    QVERIFY(overviewWarning->isVisible());
    local.acceptCoreTemp({{"connected", false}, {"status", "Core Temp access denied"}});
    QTRY_VERIFY(overviewWarning->isVisible());
    local.setModuleEnabled("coretemp", false);
    QTRY_VERIFY(!overviewWarning->isVisible());
    QCOMPARE(warnings.count(), 0);
    window->hide();
}

void ControllerTests::nestedSettingsExpandTogether_data() {
    QTest::addColumn<QByteArray>("parentDefinition");
    QTest::newRow("settings") << QByteArray("SettingsSection { title: 'Installations'; expanded: true;");
    QTest::newRow("module") << QByteArray(
        "ModuleCard { moduleKey: 'windows'; heading: 'Windows'; summary: ''; keywords: ''; collapsed: false;"
        "controller: QtObject { property var info: ({windowsEnabled: true, windowsHealthy: true}) }");
    QTest::newRow("submodule") << QByteArray("ModuleSection { title: 'Windows'; detail: ''; expanded: true;");
    QTest::newRow("commands") << QByteArray("CommandSection { title: 'Commands'; expanded: true; Layout.fillWidth: true;");
}

void ControllerTests::nestedSettingsExpandTogether() {
    QFETCH(QByteArray, parentDefinition);
    QQmlApplicationEngine engine;
    QSignalSpy warnings(&engine, &QQmlEngine::warnings);
    QByteArray qml = R"QML(
        import QtQuick
        import QtQuick.Layouts
        import "qrc:/qml/controls"
        import "qrc:/qml/modules"
        import "qrc:/qml/custom"
        Window {
            visible: true; width: 640; height: 760
            ColumnLayout {
                width: parent.width; spacing: 0
                PARENT_DEFINITION
                    objectName: "parentSection"
                    SettingsSection {
                        objectName: "childSection"
                        title: "Sensor Updates"
                        Rectangle { implicitHeight: 240; Layout.fillWidth: true }
                    }
                }
                Rectangle { objectName: "backup"; implicitHeight: 64; Layout.fillWidth: true }
            }
        }
    )QML";
    engine.loadData(qml.replace("PARENT_DEFINITION", parentDefinition));
    QVERIFY(!engine.rootObjects().isEmpty());
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    window->hide(); window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *parent = visualItem(window->contentItem(), "parentSection");
    auto *child = visualItem(window->contentItem(), "childSection");
    auto *backup = visualItem(window->contentItem(), "backup");
    QVERIFY(parent && child && backup);
    QTest::qWait(500);
    const auto parentBase = parent->height() - child->height();
    const auto collapsed = child->height();
    auto *blur = child->property("expansionBlur").value<QObject *>();
    QVERIFY(blur);
    child->setProperty("expanded", true);
    QTest::qWait(70);
    QVERIFY(child->height() > collapsed);
    QVERIFY(child->height() < collapsed + 240);
    QVERIFY(blur->property("active").toBool());
    QVERIFY(qAbs(parent->height() - child->height() - parentBase) < 2);
    QVERIFY(qAbs(backup->y() - parent->height()) < 2);
    QTest::qWait(500);
    QVERIFY(!blur->property("active").toBool());
    child->setProperty("expanded", false);
    QTest::qWait(70);
    QVERIFY(child->height() > collapsed);
    QVERIFY(blur->property("active").toBool());
    QVERIFY(qAbs(parent->height() - child->height() - parentBase) < 2);
    QVERIFY(qAbs(backup->y() - parent->height()) < 2);
    QCOMPARE(warnings.count(), 0);
}

void ControllerTests::connectionHeartbeatsKeepIndependentSweeps() {
    QTest::failOnWarning(QRegularExpression(".*(Binding loop|ReferenceError|TypeError|Required property).*"));
    QQmlApplicationEngine engine;
    engine.loadData(R"QML(
        import QtQuick
        import "qrc:/qml/components"
        import "qrc:/qml/controls"
        Window {
            width: 260; height: 100; visible: true
            color: Theme.inset
            QtObject {
                id: backend
                property var installationConfiguration: [{id: "first", colour: "#59c6f5", enabled: true}]
                property var installationStatus: ({first: {connected: true, feedback: true, lastCommunication: 0},
                    second: {connected: true, feedback: true, lastCommunication: 0}})
            }
            function reply(id) {
                const states = Object.assign({}, backend.installationStatus);
                states[id] = Object.assign({}, states[id], {lastCommunication: Date.now()});
                backend.installationStatus = states;
            }
            function addSecond() {
                backend.installationConfiguration = backend.installationConfiguration.concat(
                    [{id: "second", colour: "#f5b45c", enabled: true}]);
            }
            function removeFirst() {
                backend.installationConfiguration = backend.installationConfiguration.slice(1);
            }
            ConnectionHeartbeats {
                controller: backend
                width: 220; height: 70; x: 20; y: 10
                millisecondsPerPixel: 8
            }
        }
    )QML");
    QVERIFY(!engine.rootObjects().isEmpty());
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    window->hide();
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QPointer<QQuickItem> first = visualItem(window->contentItem(), "installationHeartbeat_first");
    QVERIFY(first);
    QTRY_VERIFY(first->property("sweepStarted").toDouble() > 0);
    const auto started = first->property("sweepStarted").toDouble();
    QTest::qWait(100);
    QVERIFY(QMetaObject::invokeMethod(window, "reply", Q_ARG(QVariant, "first")));
    QTest::qWait(300);
    QVERIFY(QMetaObject::invokeMethod(window, "addSecond"));
    QPointer<QQuickItem> second;
    QTRY_VERIFY(second = visualItem(window->contentItem(), "installationHeartbeat_second"));
    QCOMPARE(visualItem(window->contentItem(), "installationHeartbeat_first"), first.data());
    QCOMPARE(first->property("sweepStarted").toDouble(), started);
    QTRY_VERIFY(second->property("sweepStarted").toDouble() > started);
    const auto secondStarted = second->property("sweepStarted").toDouble();
    QTest::qWait(100);
    QVERIFY(QMetaObject::invokeMethod(window, "reply", Q_ARG(QVariant, "second")));
    QTest::qWait(650);
    const auto secondPath = second->property("currentPath").toString();
    const auto amberPeakPixels = [&](const QImage &image) {
        int pixels = 0;
        const auto bottom = qRound(second->mapToScene(QPointF(0,
            second->property("baselineY").toReal() - 8)).y() * window->devicePixelRatio());
        for (int y = 0; y < bottom; ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto colour = image.pixelColor(x, y);
                pixels += colour.red() > 150 && colour.red() > colour.green() * 1.1
                    && colour.green() > colour.blue() * 1.15;
            }
        return pixels;
    };
    const auto before = window->grabWindow();
    QVERIFY(amberPeakPixels(before) > 5);
    QTRY_VERIFY(first->property("sweepStarted").toDouble() > started);
    QCOMPARE(second->property("sweepStarted").toDouble(), secondStarted);
    QCOMPARE(second->property("currentPath").toString(), secondPath);
    const auto after = window->grabWindow();
    // Render-thread translations can change a few antialiased edge pixels.
    // The other installation's peak must remain visible, with comparable coverage.
    const auto beforePixels = amberPeakPixels(before);
    const auto afterPixels = amberPeakPixels(after);
    QVERIFY2(afterPixels > 5 && qAbs(afterPixels - beforePixels) <= qMax(3, beforePixels / 5),
        qPrintable(QString("Amber peak coverage changed from %1 to %2 pixels").arg(beforePixels).arg(afterPixels)));
    const auto firstPrevious = first->property("previousPath").toString();
    QVERIFY(QMetaObject::invokeMethod(window, "reply", Q_ARG(QVariant, "second")));
    QTRY_VERIFY(second->property("currentPath").toString() != secondPath);
    QCOMPARE(first->property("previousPath").toString(), firstPrevious);
    if (!qEnvironmentVariableIsEmpty("WHAT_MULTI_ECG_PREVIEW"))
        QVERIFY(after.save(qEnvironmentVariable("WHAT_MULTI_ECG_PREVIEW")));
    QVERIFY(QMetaObject::invokeMethod(window, "removeFirst"));
    QTRY_VERIFY(first.isNull());
    QCOMPARE(visualItem(window->contentItem(), "installationHeartbeat_second"), second.data());
    QCOMPARE(second->property("sweepStarted").toDouble(), secondStarted);
}

int main(int argc, char **argv) {
    // A pipe-only helper fixture exercises graceful EOF and a stuck child without hardware.
    if (argc == 2 && QByteArray(argv[1]).startsWith("--helper-")) {
        DWORD count = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "ready", 5, &count, nullptr);
        if (QByteArray(argv[1]) == "--helper-ignore-eof")
            Sleep(INFINITE);
        char buffer[128];
        while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &count, nullptr) && count)
            ;
        return 0;
    }
    OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr);
    // Keep the GUI COM apartment alive across successive QML engine fixtures.
    // Qt caches WinRT language factories between engines.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto comCleanup = qScopeGuard([com] {
        if (SUCCEEDED(com))
            CoUninitialize();
    });
    QApplication app(argc, argv);
    QQuickStyle::setStyle("WHATStyle");
    QCoreApplication::setApplicationVersion(WHAT_VERSION);
    ControllerTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "controller_tests.moc"
