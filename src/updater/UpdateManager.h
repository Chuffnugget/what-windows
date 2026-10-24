#pragma once
#include <QObject>
#include "core/BackgroundWorker.h"
#include <QSettings>
#include <QNetworkAccessManager>
#include <QVariantMap>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QTimer>
#include <functional>

// GUI-thread update state. Network requests are asynchronous; download verification and
// update receipts run on downloadWorker. Installation itself belongs to the detached helper.
class UpdateManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap info READ info NOTIFY changed)
public:
    explicit UpdateManager(const QString &profile, bool testMode, QObject *parent = nullptr);
    QVariantMap info() const;
    // Published release metadata only; never credentials or the user's dropdown selection.
    QJsonObject sharedStatus() const;
    bool isBusy() const {
        return busy;
    }
    Q_INVOKABLE void check();
    // If supplied, expectedVersion must still be the selected release when confirmation arrives.
    Q_INVOKABLE void install(const QString &expectedVersion = QString());
    Q_INVOKABLE void browseReleases();
    Q_INVOKABLE void selectRelease(const QString &version);
    Q_INVOKABLE void setBetaUpdates(bool enabled);
    static QString packageVersion(const QString &version);
    Q_INVOKABLE void setAutomatic(bool enabled);
    Q_INVOKABLE void openReleases(const QString &version = QString());
    // Call after wiring the startup UI. Emit only for a matching version and successful helper receipt.
    void checkCompletedUpdate();
    // Consume the receipt on dismissal so an interrupted startup can show it again.
    Q_INVOKABLE void dismissCompletedUpdate();
    static bool validVersion(const QString &version);
    static bool newer(const QString &candidate, const QString &current);
    static bool safeUrl(const QUrl &url);
    static QNetworkRequest updateRequest(const QUrl &url, bool asset = false);
    static QString validateManifest(const QJsonObject &manifest, const QString &version);
signals:
    void changed();
    void updateAvailable(const QString &version);
    void updateCompleted(const QVariantMap &details);
    void diagnostic(const QString &message);

private:
    friend class UpdateTests;
    friend class ControllerTests;
    static bool saveUpdateNotice(const QString &path, const QJsonObject &notice);
    static QDateTime signingCertificateExpiry(const QString &signaturePath);
    QVariantMap signingWarning() const;
    void reportInstallFailure(const QString &code, const QString &details, const QString &version);
    QDateTime certificateExpiry;
    QVariantMap signingFailure;
    QVariantMap completedUpdate;
    void fetch(const QUrl &url, qint64 limit, std::function<void(QByteArray)> done, bool asset = false,
               int redirects = 0);
    void checkRelease();
    void fetchReleaseList(int page = 1);
    void prepareRelease(const QJsonObject &candidate);
    QJsonArray releases;
    bool browsing = false;
    QUrl assetUrl(const QJsonObject &asset, const QString &tag) const;
    void status(const QString &message, bool isError = false);
    BackgroundWorker downloadWorker{"Update download and verification"};
    QSettings settings;
    QNetworkAccessManager network;
    QNetworkAccessManager *transport = &network;
    QTimer automaticCheckTimer;
    QString announcedVersion;
    QString profile, family, available, message = "Updates have not been checked yet.";
    QJsonObject release, manifest;
    QString latestCheckedVersion, latestCheckedAt;
    void rememberLatest(const QJsonObject &candidate);
    QUrl installerUrl;
    bool busy = false, testMode, error = false;
    double progress = 0;
    bool downloading = false;
    qint64 downloadBytes = 0, downloadTotal = 0, installerSize = 0;
};
