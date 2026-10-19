#include "app/Architecture.h"
#include "updater/UpdateManager.h"
#include "updater/UpdateDownload.h"
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QtTest>
#include <QTemporaryDir>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <cstring>
#include <QScopeGuard>

class FixtureReply : public QNetworkReply {
    QByteArray payload;
    qint64 offset = 0;

public:
    FixtureReply(const QNetworkRequest &request, QByteArray bytes, QObject *parent, int statusCode = 200,
                 QUrl redirect = {})
        : QNetworkReply(parent), payload(bytes) {
        setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, statusCode);
        if (!redirect.isEmpty())
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, redirect);
        if (statusCode >= 400)
            setError(QNetworkReply::ContentNotFoundError, "HTTP failure fixture");
        QTimer::singleShot(0, this, [this] {
            emit readyRead();
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {
        setError(OperationCanceledError, "Aborted");
    }

    qint64 bytesAvailable() const override {
        return payload.size() - offset + QNetworkReply::bytesAvailable();
    }

    qint64 readData(char *data, qint64 maximum) override {
        auto count = qMin(maximum, payload.size() - offset);
        if (!count)
            return -1;
        memcpy(data, payload.constData() + offset, size_t(count));
        offset += count;
        return count;
    }
};

class FixtureNetwork : public QNetworkAccessManager {
public:
    int statusCode = 200;
    QList<QByteArray> responses;
    QList<QUrl> requests;
    QList<QNetworkRequest> headers;
    QList<QPair<int, QUrl>> redirects;

    QNetworkReply *createRequest(Operation, const QNetworkRequest &r, QIODevice *) override {
        requests.append(r.url());
        headers.append(r);
        if (!redirects.isEmpty()) {
            const auto step = redirects.takeFirst();
            if (step.first != 200)
                return new FixtureReply(r, {}, this, step.first, step.second);
        }
        return new FixtureReply(r, responses.isEmpty() ? QByteArray() : responses.takeFirst(), this,
                                statusCode);
    }
};

class UpdateTests : public QObject {
    Q_OBJECT
private slots:
    void signingExpiryWarnings() {
        QTemporaryDir dir;
        UpdateManager updater(dir.path(), true);
        updater.family = "WHAT.Desktop_fixture";
        updater.certificateExpiry = QDateTime::currentDateTimeUtc().addDays(91);
        QVERIFY(updater.info()["signingWarning"].toMap().isEmpty());
        updater.certificateExpiry = QDateTime::currentDateTimeUtc().addDays(89);
        auto warning = updater.info()["signingWarning"].toMap();
        QCOMPARE(warning["title"].toString(), QString("App Signing Certificate Expires Soon"));
        QVERIFY(!warning["critical"].toBool());
        updater.certificateExpiry = QDateTime::currentDateTimeUtc().addSecs(-1);
        warning = updater.info()["signingWarning"].toMap();
        QCOMPARE(warning["title"].toString(), QString("App Signing Certificate Expired"));
        QVERIFY(warning["critical"].toBool());
        QVERIFY(warning["detail"].toString().contains("No uninstall"));
        QVERIFY(warning["detail"].toString().contains("Home Assistant pairing will not help"));

        // Expiry is advisory: a correctly timestamped package may still pass Windows validation.
        updater.testMode = false;
        updater.available = "9.0.0";
        updater.manifest = {{"version", "9.0.0"}};
        QVERIFY(updater.info()["canInstall"].toBool());
        updater.family.clear();
        QVERIFY(updater.info()["signingWarning"].toMap().isEmpty());
    }

    void signingFailureRecovery_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<bool>("hasWarning");
        QTest::newRow("changed") << QString("certificate_changed") << true;
        QTest::newRow("expired") << QString("certificate_expired") << true;
        QTest::newRow("untrusted") << QString("signature_untrusted") << true;
        QTest::newRow("unrelated") << QString("disk_full") << false;
    }

    void signingFailureRecovery() {
        QFETCH(QString, code);
        QFETCH(bool, hasWarning);
        const auto savedVersion = QCoreApplication::applicationVersion();
        const auto restore = qScopeGuard([&] { QCoreApplication::setApplicationVersion(savedVersion); });
        QCoreApplication::setApplicationVersion("0.0.1-beta.1");
        QTemporaryDir dir;
        UpdateManager updater(dir.path(), true);
        updater.family = "WHAT.Desktop_fixture";
        updater.reportInstallFailure(code, "Failure details", "0.0.1-beta.2");
        const auto warning = updater.info()["signingWarning"].toMap();
        QCOMPARE(!warning.isEmpty(), hasWarning);
        QVERIFY(updater.info()["error"].toBool());
        // A later automatic check must not erase instructions for a blocked installation.
        updater.status("Checking for updates...");
        QCOMPARE(updater.info()["signingWarning"].toMap(), warning);

        const QString attempt = "9e8490c1-67cf-4d10-a3ba-a7949efcf05c";
        const auto folder = dir.filePath("updates/" + attempt);
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(UpdateManager::saveUpdateNotice(dir.filePath("updates/pending.json"),
                                                {{"attempt", attempt},
                                                 {"family", updater.family},
                                                 {"previous", "0.0.1-beta.1"},
                                                 {"current", "0.0.1-beta.2"}}));
        QFile failure(folder + "/error.txt");
        QVERIFY(failure.open(QIODevice::WriteOnly));
        failure.write("Failure details");
        failure.close();
        QFile reason(folder + "/error-code.txt");
        QVERIFY(reason.open(QIODevice::WriteOnly));
        reason.write(QByteArray::fromHex("efbbbf") + code.toUtf8()); // Windows PowerShell UTF-8 BOM.
        reason.close();
        UpdateManager restarted(dir.path(), true);
        restarted.family = updater.family;
        QSignalSpy completed(&restarted, &UpdateManager::updateCompleted);
        restarted.checkCompletedUpdate();
        QTRY_VERIFY(!restarted.busy);
        QCOMPARE(completed.size(), 0);
        QCOMPARE(restarted.info()["signingWarning"].toMap(), warning);

        // A manual installer has already resolved the failed attempt. Do not show stale warnings.
        QCoreApplication::setApplicationVersion("0.0.1-beta.2");
        UpdateManager installed(dir.path(), true);
        installed.family = updater.family;
        installed.checkCompletedUpdate();
        QTRY_VERIFY(!installed.busy);
        QVERIFY(installed.info()["signingWarning"].toMap().isEmpty());
    }

    void signingCertificateFromPackage() {
        QTemporaryDir dir;
        QFile malformed(dir.filePath("AppxSignature.p7x"));
        QVERIFY(malformed.open(QIODevice::WriteOnly));
        malformed.write("PKCXinvalid");
        malformed.close();
        QVERIFY(!UpdateManager::signingCertificateExpiry(malformed.fileName()).isValid());
        const auto packageSignature = qEnvironmentVariable("WHAT_TEST_PACKAGE_SIGNATURE");
        if (!packageSignature.isEmpty()) {
            const auto expiry = UpdateManager::signingCertificateExpiry(packageSignature);
            QVERIFY(expiry.isValid());
            QCOMPARE(expiry.date().toString(Qt::ISODate), qEnvironmentVariable("WHAT_TEST_SIGNER_EXPIRY"));
        }
    }

    void completedUpdateRequiresInstalledTarget_data() {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<bool>("expected");
        for (const auto *scenario :
             {"success", "downgrade", "not-installed", "failed", "wrong-result", "wrong-version",
              "wrong-family", "portable", "invalid-attempt", "same-version", "oversized", "malformed"}) {
            QTest::newRow(scenario) << QString(scenario)
                                    << (QString(scenario) == "success" || QString(scenario) == "downgrade");
        }
    }

    void completedUpdateRequiresInstalledTarget() {
        QFETCH(QString, scenario);
        QFETCH(bool, expected);
        const auto savedVersion = QCoreApplication::applicationVersion();
        const auto restoreVersion =
            qScopeGuard([&] { QCoreApplication::setApplicationVersion(savedVersion); });
        const QString current = "0.0.1-beta.11";
        QCoreApplication::setApplicationVersion(current);
        QTemporaryDir dir;
        const QString attempt = "9e8490c1-67cf-4d10-a3ba-a7949efcf05c";
        const auto folder = dir.filePath("updates/" + attempt);
        QVERIFY(QDir().mkpath(folder));
        auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        const auto pending = dir.filePath("updates/pending.json");
        QJsonObject notice{{"attempt", attempt},
                           {"family", "WHAT.Desktop_fixture"},
                           {"previous", "0.0.1-beta.10"},
                           {"current", current},
                           {"notes", "Changes for this release"}};
        if (scenario == "downgrade")
            notice["previous"] = "0.0.2";
        if (scenario == "wrong-version")
            notice["current"] = "0.0.1-beta.12";
        if (scenario == "wrong-family")
            notice["family"] = "WHAT.Desktop_other";
        if (scenario == "invalid-attempt")
            notice["attempt"] = "../outside";
        if (scenario == "same-version")
            notice["previous"] = current;
        if (scenario == "oversized")
            notice["notes"] = QString(70000, 'x');
        QVERIFY(UpdateManager::saveUpdateNotice(pending, notice));
        if (scenario == "malformed")
            QVERIFY(write(pending, "{broken"));
        if (scenario != "not-installed")
            QVERIFY(write(folder + "/result.txt",
                          scenario == "wrong-result" ? "Installed 0.0.1.10\r\n" : "Installed 0.0.1.11\r\n"));
        if (scenario == "failed")
            QVERIFY(write(folder + "/error.txt", "Deployment failed"));

        UpdateManager updater(dir.path(), true);
        updater.family = scenario == "portable" ? QString() : QString("WHAT.Desktop_fixture");
        QSignalSpy completed(&updater, &UpdateManager::updateCompleted);
        updater.checkCompletedUpdate();
        QTRY_VERIFY(!updater.busy);
        QCOMPARE(completed.size(), expected ? 1 : 0);
        if (!expected)
            return;
        QCOMPARE(updater.info()["completedUpdate"].toMap()["previous"].toString(),
                 notice["previous"].toString());
        QCOMPARE(completed.first().first().toMap()["current"].toString(), current);
        QCOMPARE(completed.first().first().toMap()["notes"].toString(), notice["notes"].toString());
        updater.checkCompletedUpdate();
        QCOMPARE(completed.size(), 1);

        // Keep the confirmation until dismissal, including across an app restart.
        UpdateManager restarted(dir.path(), true);
        restarted.family = "WHAT.Desktop_fixture";
        QSignalSpy repeated(&restarted, &UpdateManager::updateCompleted);
        restarted.checkCompletedUpdate();
        QTRY_COMPARE(repeated.size(), 1);
        restarted.dismissCompletedUpdate();
        QTRY_VERIFY(!QFile::exists(pending));
        QVERIFY(restarted.info()["completedUpdate"].toMap().isEmpty());
        restarted.checkCompletedUpdate();
        QTRY_VERIFY(!restarted.busy);
        QCOMPARE(repeated.size(), 1);
    }

    void eligibleReleasePromptsAndInstallation_data() {
        QTest::addColumn<bool>("beta");
        QTest::newRow("stable") << false;
        QTest::newRow("beta") << true;
    }

    void eligibleReleasePromptsAndInstallation() {
        QFETCH(bool, beta);
        QCoreApplication::setApplicationVersion("0.7.7");
        QTemporaryDir dir;
        FixtureNetwork network;
        UpdateManager updater(dir.path(), false);
        updater.transport = &network;
        updater.family = "WHAT.Desktop_fixture";
        QSignalSpy prompts(&updater, &UpdateManager::updateAvailable);
        const QString version = beta ? "0.7.8-beta.2" : "0.7.8";
        const QString root =
            "https://github.com/Chuffnugget/what-windows/releases/download/v" + version + "/";
        QJsonObject manifest{{"schema", beta ? 2 : 1},
                             {"minimumUpdaterVersion", beta ? 2 : 1},
                             {"version", version},
                             {"architecture", Architecture::name()},
                             {"packageVersion", UpdateManager::packageVersion(version)},
                             {"installer", "WHAT-" + version + "-Windows-" + Architecture::name() + "-UpdatePayload.msix"},
                             {"sha256", QString(64, 'a')}};
        QJsonArray assets{
            QJsonObject{{"name", Architecture::updateManifest()}, {"browser_download_url", root + Architecture::updateManifest()}},
            QJsonObject{{"name", manifest["installer"]},
                        {"size", 41394945},
                        {"browser_download_url", root + manifest["installer"].toString()}}};
        QJsonObject release{{"tag_name", "v" + version}, {"prerelease", beta}, {"assets", assets}};
        auto respond = [&](bool list) {
            network.responses = {list ? QJsonDocument(QJsonArray{release}).toJson()
                                      : QJsonDocument(release).toJson(),
                                 QJsonDocument(manifest).toJson()};
        };
        respond(beta);
        if (beta)
            updater.setBetaUpdates(true); // Switching channel itself must check.
        else
            updater.check();
        QTRY_VERIFY(!updater.busy);
        QVERIFY2(updater.info()["canInstall"].toBool(), qPrintable(updater.message));
        QCOMPARE(updater.available, version);
        QCOMPARE(prompts.size(), 1);
        QCOMPARE(prompts.first().first().toString(), version);
        respond(beta);
        updater.check();
        QTRY_VERIFY(!updater.busy);
        QCOMPARE(prompts.size(), 1); // No repeated popup for the same release this session.
        respond(true);
        updater.browseReleases();
        QTRY_VERIFY(!updater.busy);
        QCOMPARE(updater.available, version); // Newer release is already prepared in the picker.
        QVERIFY(updater.info()["canInstall"].toBool());
        QCOMPARE(prompts.size(), 1); // Browsing does not announce a release.
        updater.family.clear();
        QVERIFY(!updater.info()["canInstall"].toBool());
        QVERIFY(updater.info()["installBlockedReason"].toString().contains("package identity"));
    }

    void requestErrorsClearAfterSuccessfulRetry() {
        QCoreApplication::setApplicationVersion("0.5.6");
        QTemporaryDir dir;
        FixtureNetwork network;
        UpdateManager updater(dir.path(), true);
        updater.transport = &network;
        network.statusCode = 404;
        updater.check();
        QTRY_VERIFY(!updater.busy);
        QVERIFY(updater.info()["error"].toBool());
        QVERIFY(updater.message.startsWith("Update request failed"));
        network.statusCode = 200;
        network.responses = {QJsonDocument(QJsonObject{{"tag_name", "v0.5.6"}}).toJson()};
        updater.check();
        QTRY_VERIFY(!updater.busy);
        QVERIFY(!updater.info()["error"].toBool());
        QVERIFY(updater.message.startsWith("You are up to date"));
    }

    void assetRedirectsAreCheckedAndCarryNoCredentials() {
        QCoreApplication::setApplicationVersion("0.6.2");
        QTemporaryDir dir;
        FixtureNetwork network;
        const QString browser = "https://github.com/Chuffnugget/what-windows/releases/download/v0.6.3/";
        const QUrl cdn("https://release-assets.githubusercontent.com/file?signature=fixture");
        const QByteArray payload = "signed-package-fixture";
        network.redirects = {{302, cdn}};
        network.responses = {payload};
        auto result = downloadUpdate(
            network, QUrl(browser + "package.msix"), dir.path() + "/package",
            QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()));
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        for (const auto &request : network.headers)
            QVERIFY(!request.hasRawHeader("Authorization"));
        network.redirects = {{302, QUrl("https://evil.example/package")}};
        result = downloadUpdate(network, QUrl(browser + "package.msix"), dir.path() + "/bad", QString(64, 'a'));
        QVERIFY(result.error.contains("redirect"));
        QVERIFY(!QFile::exists(dir.path() + "/bad"));
    }

    void versions() {
        QVERIFY(UpdateManager::newer("0.4.10", "0.4.9"));
        QVERIFY(!UpdateManager::newer("0.4.8", "0.4.9"));
        QVERIFY(!UpdateManager::newer("0.4.9", "0.4.9"));
        for (const auto &v : {"1.0.0-beta.0", "1.0.0-beta.65536", "1.0.0b", "v1.0.0", "01.0.0", "1.2",
                              "1.2.3.4", "65536.0.0", "1.0.0\n"})
            QVERIFY(!UpdateManager::validVersion(v));
    }

    void betaVersionsAndManifest() {
        QVERIFY(UpdateManager::validVersion("0.7.5-beta.1"));
        QVERIFY(UpdateManager::newer("0.7.5-beta.1", "0.7.4"));
        QVERIFY(UpdateManager::newer("0.7.5-beta.10", "0.7.5-beta.2"));
        QVERIFY(UpdateManager::newer("0.7.5", "0.7.5-beta.10"));
        QVERIFY(!UpdateManager::newer("0.7.5-beta.10", "0.7.5"));
        QCOMPARE(UpdateManager::packageVersion("0.7.5-beta.10"), QString("0.7.5.10"));
        QCOMPARE(UpdateManager::packageVersion("0.7.5"), QString("0.7.5.0"));
        QJsonObject manifest{{"schema", 2},
                             {"minimumUpdaterVersion", 2},
                             {"version", "0.7.5-beta.1"},
                             {"packageVersion", "0.7.5.1"},
                             {"architecture", Architecture::name()},
                             {"installer", "WHAT-0.7.5-beta.1-Windows-" + Architecture::name() + "-UpdatePayload.msix"},
                             {"sha256", QString(64, 'a')}};
        QVERIFY(UpdateManager::validateManifest(manifest, "0.7.5-beta.1").isEmpty());
        manifest["packageVersion"] = "0.7.6.1";
        QVERIFY(!UpdateManager::validateManifest(manifest, "0.7.5-beta.1").isEmpty());
    }

    void sharedUpdateStatusTracksLatestCheckedRelease() {
        QCoreApplication::setApplicationVersion("0.0.1-beta.16");
        QTemporaryDir dir;
        FixtureNetwork network;
        UpdateManager updater(dir.path(), true);
        updater.transport = &network;
        QVERIFY(updater.sharedStatus()["latest"].isNull());
        updater.setBetaUpdates(true);
        QJsonArray entries{QJsonObject{{"tag_name", "v0.0.1-beta.15"}, {"prerelease", true}},
                           QJsonObject{{"tag_name", "v0.0.1-beta.17"}, {"prerelease", true}}};
        network.responses = {QJsonDocument(entries).toJson()};
        updater.browseReleases();
        QTRY_VERIFY(!updater.busy);
        QCOMPARE(updater.sharedStatus()["latest"].toString(), QString("0.0.1-beta.17"));
        updater.selectRelease("0.0.1-beta.15");
        QCOMPARE(updater.available, QString("0.0.1-beta.15"));
        QCOMPARE(updater.sharedStatus()["latest"].toString(), QString("0.0.1-beta.17"));
        QVERIFY(updater.sharedStatus()["release_url"].toString().endsWith("/v0.0.1-beta.17"));
        QVERIFY(!updater.sharedStatus()["checked_at"].toString().isEmpty());
        network.statusCode = 404;
        updater.check();
        QTRY_VERIFY(!updater.busy);
        QCOMPARE(updater.sharedStatus()["latest"].toString(), QString("0.0.1-beta.17"));
        QVERIFY(!updater.sharedStatus().contains("authSource"));
        updater.setBetaUpdates(false);
        QVERIFY(updater.sharedStatus()["latest"].isNull());
        network.statusCode = 200;
        network.responses = {QJsonDocument(QJsonObject{{"tag_name", "v0.0.0"}}).toJson()};
        updater.check();
        QTRY_VERIFY(!updater.busy);
        QCOMPARE(updater.sharedStatus()["latest"].toString(), QString("0.0.0"));
        QVERIFY(updater.available.isEmpty());
    }

    void releaseBrowserFilteringAndDowngrade() {
        QCoreApplication::setApplicationVersion("0.7.4");
        QTemporaryDir dir;
        FixtureNetwork network;
        UpdateManager updater(dir.path(), true);
        updater.transport = &network;
        QVERIFY(!updater.info()["betaUpdates"].toBool());
        QJsonArray entries{QJsonObject{{"tag_name", "v0.7.3"}},
                           QJsonObject{{"tag_name", "v0.7.5-beta.2"}, {"prerelease", true}},
                           QJsonObject{{"tag_name", "v0.7.5-beta.10"}, {"prerelease", true}},
                           QJsonObject{{"tag_name", "v0.7.6"}, {"draft", true}},
                           QJsonObject{{"tag_name", "v0.7.5-beta.20"}, {"prerelease", false}}};
        network.responses = {QJsonDocument(entries).toJson()};
        updater.browseReleases();
        QTRY_VERIFY(!updater.busy);
        QCOMPARE(updater.releases.size(), 1);
        QVERIFY(updater.available.isEmpty());
        updater.selectRelease("0.7.3");
        QVERIFY(updater.info()["downgrade"].toBool());
        QVERIFY(updater.manifest.isEmpty()); // Missing assets never become installable.
        updater.setBetaUpdates(true);
        network.responses = {QJsonDocument(entries).toJson()};
        updater.browseReleases();
        QTRY_VERIFY(!updater.busy);
        QCOMPARE(updater.releases.size(), 3);
        QCOMPARE(updater.releases.first().toObject()["tag_name"].toString(), QString("v0.7.5-beta.10"));
        network.responses = {QJsonDocument(entries).toJson()};
        updater.check();
        QTRY_VERIFY(!updater.busy);
        QCOMPARE(updater.available, QString("0.7.5-beta.10"));
        updater.setBetaUpdates(false);
        QVERIFY(updater.available.isEmpty());
        QVERIFY(updater.releases.isEmpty());
        UpdateManager reloaded(dir.path(), true);
        QVERIFY(!reloaded.info()["betaUpdates"].toBool());
    }

    void releaseListPagination() {
        QTemporaryDir dir;
        FixtureNetwork network;
        UpdateManager updater(dir.path(), true);
        updater.transport = &network;
        QJsonArray page;
        for (int i = 0; i < 100; ++i)
            page.append(QJsonObject{{"tag_name", "ignored"}});
        network.responses = {QJsonDocument(page).toJson(),
                             QJsonDocument(QJsonArray{QJsonObject{{"tag_name", "v0.7.2"}}}).toJson()};
        updater.browseReleases();
        QTRY_VERIFY(!updater.busy);
        QCOMPARE(network.requests.size(), 2);
        QCOMPARE(updater.releases.size(), 1);
        updater.install("changed-selection");
        QVERIFY(updater.error);
    }

    void urls() {
        QVERIFY(UpdateManager::safeUrl(QUrl("https://release-assets.githubusercontent.com/download?x=1")));
        for (const auto &url :
             {"http://github.com/a", "https://github.com.evil.test/a", "https://evil.test/a",
              "file:///C:/bad.exe", "https://user@github.com/a", "https://github.com:8080/a"})
            QVERIFY(!UpdateManager::safeUrl(QUrl(url)));
    }

    void manifests() {
        QJsonObject good{{"schema", 1},
                         {"minimumUpdaterVersion", 1},
                         {"version", "0.4.10"},
                         {"architecture", Architecture::name()},
                         {"installer", "WHAT-0.4.10-Windows-" + Architecture::name() + "-UpdatePayload.msix"},
                         {"sha256", QString(64, 'a')}};
        QVERIFY(UpdateManager::validateManifest(good, "0.4.10").isEmpty());
        QVERIFY(!UpdateManager::validateManifest(good, "0.4.11").isEmpty());
        for (auto key :
             {"schema", "minimumUpdaterVersion", "sha256", "architecture", "installer", "version"}) {
            auto broken = good;
            broken.remove(key);
            QVERIFY(!UpdateManager::validateManifest(broken, "0.4.10").isEmpty());
        }
        auto broken = good;
        broken["architecture"] = Architecture::arm64 ? "x64" : "arm64";
        QVERIFY(!UpdateManager::validateManifest(broken, "0.4.10").isEmpty());
        broken = good;
        broken["installer"] = "WHAT-0.4.10-Windows-x64.msix";
        QVERIFY(!UpdateManager::validateManifest(broken, "0.4.10").isEmpty());
        broken["installer"] = "../payload.exe";
        QVERIFY(!UpdateManager::validateManifest(broken, "0.4.10").isEmpty());
        broken = good;
        broken["minimumUpdaterVersion"] = 2;
        QVERIFY(!UpdateManager::validateManifest(broken, "0.4.10").isEmpty());
    }

    void preferencesAndNoInstallWithoutManifest() {
        QTemporaryDir dir;
        {
            UpdateManager updater(dir.path(), true);
            QVERIFY(updater.info()["automatic"].toBool());
            updater.setAutomatic(false);
            updater.install();
            QVERIFY(!updater.info()["busy"].toBool());
        }
        UpdateManager updater(dir.path(), true);
        QVERIFY(!updater.info()["automatic"].toBool());
        QVERIFY(!updater.info()["canInstall"].toBool());
    }

    void releaseDiscoveryAndCorruptDownload() {
        QCoreApplication::setApplicationVersion("0.4.9");
        QTemporaryDir dir;
        FixtureNetwork network;
        UpdateManager updater(dir.path(), true);
        updater.transport = &network;
        const QString base = "https://github.com/chuffnugget/what-windows/releases/download/v0.4.10/";
        QJsonObject manifest{{"schema", 1},
                             {"minimumUpdaterVersion", 1},
                             {"version", "0.4.10"},
                             {"architecture", Architecture::name()},
                             {"installer", "WHAT-0.4.10-Windows-" + Architecture::name() + "-UpdatePayload.msix"},
                             {"sha256", QString(64, 'a')}};
        QJsonArray assets{
            QJsonObject{{"name", Architecture::updateManifest()}, {"browser_download_url", base + Architecture::updateManifest()}},
            QJsonObject{{"name", manifest["installer"]},
                        {"browser_download_url", base + manifest["installer"].toString()},
                        {"size", 100}}};
        network.responses = {QJsonDocument(QJsonObject{{"tag_name", "v0.4.10"}, {"assets", assets}}).toJson(),
                             QJsonDocument(manifest).toJson(), "corrupt package"};
        updater.check();
        QTRY_VERIFY(!updater.info()["busy"].toBool());
        QCOMPARE(updater.info()["available"].toString(), QString("0.4.10"));
        QVERIFY(!updater.manifest.isEmpty());
        QVERIFY(!updater.info()["error"].toBool());
        const auto result = downloadUpdate(network, QUrl(base + manifest["installer"].toString()),
                                           dir.path() + "/update.msix", QString(64, 'a'));
        QVERIFY(result.error.contains("checksum mismatch"));
        QCOMPARE(network.requests.size(), 3);
        QCOMPARE(network.requests.first().toString(),
                 QString("https://api.github.com/repos/Chuffnugget/what-windows/releases/latest"));
        QVERIFY(!QFile::exists(dir.path() + "/update.msix"));
        const QByteArray valid(200000, 'a');
        network.responses = {valid};
        QList<qint64> received;
        qint64 expectedSize = 0;
        const auto good = downloadUpdate(
            network, QUrl(base + manifest["installer"].toString()), dir.path() + "/valid.msix",
            QString::fromLatin1(QCryptographicHash::hash(valid, QCryptographicHash::Sha256).toHex()),
            [&](qint64 bytes, qint64 total) {
                received.append(bytes);
                expectedSize = total;
            });
        QVERIFY(received.size() > 1);
        QVERIFY(received.first() < valid.size());
        QCOMPARE(received.last(), valid.size());
        QCOMPARE(expectedSize, valid.size());
        QVERIFY2(good.error.isEmpty(), qPrintable(good.error));
        QFile saved(good.path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), valid);
    }

    void boundedResponsesAndMissingManifest() {
        QTemporaryDir dir;
        FixtureNetwork network;
        UpdateManager updater(dir.path(), true);
        updater.transport = &network;
        network.responses = {QByteArray(100, 'x')};
        updater.busy = true;
        bool called = false;
        updater.fetch(QUrl("https://api.github.com/test"), 10, [&](QByteArray) { called = true; });
        QTRY_VERIFY(!updater.busy);
        QVERIFY(!called);
        QVERIFY(updater.info()["error"].toBool());
        network.responses = {QJsonDocument(QJsonObject{{"tag_name", "v0.4.10"}}).toJson()};
        updater.check();
        QTRY_VERIFY(!updater.busy);
        QVERIFY(updater.manifest.isEmpty());
        QVERIFY(updater.message.contains("manually"));
        QVERIFY(updater.info()["error"].toBool());
        updater.status("Checking again");
        QVERIFY(!updater.info()["error"].toBool());
    }
};
QTEST_GUILESS_MAIN(UpdateTests)
#include "update_tests.moc"
