#include "app/Architecture.h"
#include "app/SettingsStore.h"
#include "updater/UpdateManager.h"
#include "updater/UpdateDownload.h"
#include "app/PackageIdentity.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QElapsedTimer>
#include <QUuid>
#include <memory>
#include <atomic>
#include <algorithm>
#include <QVersionNumber>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTimeZone>
#include <wincrypt.h>

static bool releaseAsset(const QUrl &url, const QString &tag, const QString &name) {
    const auto parts = url.path().split('/', Qt::SkipEmptyParts);
    return UpdateManager::safeUrl(url) && url.host() == "github.com" && url.query().isEmpty() &&
           url.fragment().isEmpty() && parts.size() == 6 &&
           parts[0].compare("Chuffnugget", Qt::CaseInsensitive) == 0 &&
           parts[1].compare("what-windows", Qt::CaseInsensitive) == 0 && parts[2] == "releases" &&
           parts[3] == "download" && parts[4] == tag && parts[5] == name;
}

static const QString repo = "https://github.com/Chuffnugget/what-windows/releases";

UpdateManager::UpdateManager(const QString &path, bool testing, QObject *parent)
    : QObject(parent), settings(SettingsStore::profilePath(path), SettingsStore::format()), profile(path),
      family(packageFamily()), testMode(testing) {
    downloadWorker.onStatus = [this](QString error) {
        if (!error.isEmpty()) {
            busy = false;
            downloading = false;
            status("Update download failed: " + error, true);
        }
    };
    // Left behind by development builds that could sign in to private releases.
    settings.remove("updates/private");
    if (!testMode) {
        QTimer::singleShot(3000, this, [this] {
            if (settings.value("updates/automatic", true).toBool())
                check();
        });
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] {
            emit changed(); // Refresh certificate dates even when automatic checks are off.
            if (settings.value("updates/automatic", true).toBool())
                check();
        });
        timer->start(24 * 60 * 60 * 1000);
    }
}

QVariantMap UpdateManager::info() const {
    QVariantList choices;
    for (const auto &entry : releases) {
        const auto item = entry.toObject();
        const auto version = item["tag_name"].toString().mid(1);
        choices.append(QVariantMap{{"version", version},
                                   {"beta", item["prerelease"].toBool()},
                                   {"date", item["published_at"].toString().left(10)},
                                   {"notes", item["body"].toString().left(12000)},
                                   {"current", version == QCoreApplication::applicationVersion()},
                                   {"downgrade", newer(QCoreApplication::applicationVersion(), version)}});
    }
    QString blocked;
    if (busy)
        blocked = "Wait for the update check or download to finish.";
    else if (family.isEmpty())
        blocked = "This copy is running without Windows package identity (Qt Creator or portable). Open the "
                  "installed WHAT from Start to update, or download Setup from Releases.";
    else if (available.isEmpty())
        blocked = "Select a release or check for updates first.";
    else if (available == QCoreApplication::applicationVersion())
        blocked = "This version is already running.";
    else if (manifest.isEmpty())
        blocked = error ? message : "The selected release does not have a verified update manifest.";
    else if (testMode)
        blocked = "Installation is disabled in test mode.";
    return {{"completedUpdate", completedUpdate},
            {"signingWarning", signingWarning()},
            {"releases", choices},
            {"installBlockedReason", blocked},
            {"betaUpdates", settings.value("updates/beta", false).toBool()},
            {"downgrade", newer(QCoreApplication::applicationVersion(), available)},
            {"error", error},
            {"status", message},
            {"busy", busy},
            {"progress", progress},
            {"downloading", downloading},
            {"downloadBytes", downloadBytes},
            {"downloadTotal", downloadTotal},
            {"available", available},
            {"current", QCoreApplication::applicationVersion()},
            {"packaged", !family.isEmpty()},
            {"canInstall", blocked.isEmpty()},
            {"automatic", settings.value("updates/automatic", true).toBool()}};
}

QJsonObject UpdateManager::sharedStatus() const {
    return {{"installed", QCoreApplication::applicationVersion()},
            {"latest", latestCheckedVersion.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(latestCheckedVersion)},
            {"release_url", latestCheckedVersion.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                          : QJsonValue(repo + "/tag/v" + latestCheckedVersion)},
            {"checked_at", latestCheckedAt.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(latestCheckedAt)},
            {"channel", settings.value("updates/beta", false).toBool() ? "beta" : "stable"},
            {"busy", busy}};
}

void UpdateManager::rememberLatest(const QJsonObject &candidate) {
    latestCheckedVersion = candidate["tag_name"].toString().mid(1);
    latestCheckedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

void UpdateManager::status(const QString &value, bool isError) {
    error = isError;
    message = value;
    emit changed();
    emit diagnostic("Updater: " + value);
}

QVariantMap UpdateManager::signingWarning() const {
    if (!signingFailure.isEmpty())
        return signingFailure;
    if (family.isEmpty() || !certificateExpiry.isValid())
        return {};
    const auto now = QDateTime::currentDateTimeUtc();
    if (certificateExpiry > now.addDays(90))
        return {};
    const bool expired = certificateExpiry <= now;
    return {
        {"title", expired ? "App Signing Certificate Expired" : "App Signing Certificate Expires Soon"},
        {"detail", QString("This copy's app-signing certificate %1 on %2. WHAT can keep running. "
                           "The developer must renew it; renewing Home Assistant pairing will not help. "
                           "If an automatic update is blocked, download the latest Setup.exe for this PC "
                           "from the official GitHub release and run it over this installation. "
                           "No uninstall or settings reset is needed.")
                       .arg(expired ? "expired" : "expires", certificateExpiry.date().toString(Qt::ISODate))},
        {"critical", expired},
        {"version", QString()}};
}

void UpdateManager::reportInstallFailure(const QString &code, const QString &details,
                                         const QString &version) {
    QString reason;
    if (code == "certificate_changed")
        reason =
            "The update uses a different signing certificate. Automatic certificate changes are not supported.";
    else if (code == "certificate_expired")
        reason =
            "Windows could not validate the update's expired signing certificate. Check this PC's date and time.";
    else if (code == "signature_untrusted")
        reason = "Windows could not verify the update's signature. It may be untrusted or damaged.";
    signingFailure = {};
    if (!reason.isEmpty()) {
        signingFailure = {
            {"title", "Update Signature Needs Attention"},
            {"detail", reason +
                           " Nothing was installed. Download the latest Setup.exe for this PC from WHAT's "
                           "official GitHub release and run it over the existing installation. "
                           "No uninstall or settings reset is needed. Do not bypass signature checks."},
            {"critical", true},
            {"version", code == "certificate_changed" && validVersion(version) ? version : QString()}};
    }
    status("Update failed: " + details.left(1000), true);
}

// Read only the installed package's signer, not the timestamp authority. This is
// an expiry advisory; Apply-Update.ps1 still performs every trust/identity check.
QDateTime UpdateManager::signingCertificateExpiry(const QString &signaturePath) {
    QFile file(signaturePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return {};
    auto bytes = file.readAll();
    if (!bytes.startsWith("PKCX") || bytes.size() <= 4)
        return {};
    CRYPT_DATA_BLOB blob{DWORD(bytes.size() - 4), reinterpret_cast<BYTE *>(bytes.data() + 4)};
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_BLOB, &blob, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr, &store, &message,
                          nullptr))
        return {};
    const auto cleanup = qScopeGuard([&] {
        CryptMsgClose(message);
        CertCloseStore(store, 0);
    });
    DWORD count = 0, size = sizeof(count);
    if (!CryptMsgGetParam(message, CMSG_SIGNER_COUNT_PARAM, 0, &count, &size) || count != 1)
        return {};
    size = 0;
    if (!CryptMsgGetParam(message, CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &size) || size > 65536)
        return {};
    QByteArray signer(size, Qt::Uninitialized);
    if (!CryptMsgGetParam(message, CMSG_SIGNER_CERT_INFO_PARAM, 0, signer.data(), &size))
        return {};
    const auto certificate = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                        CERT_FIND_SUBJECT_CERT, signer.constData(), nullptr);
    if (!certificate)
        return {};
    const auto releaseCertificate = qScopeGuard([&] { CertFreeCertificateContext(certificate); });
    ULARGE_INTEGER time;
    time.LowPart = certificate->pCertInfo->NotAfter.dwLowDateTime;
    time.HighPart = certificate->pCertInfo->NotAfter.dwHighDateTime;
    return QDateTime::fromMSecsSinceEpoch(qint64(time.QuadPart / 10000) - 11644473600000LL, QTimeZone::UTC);
}

void UpdateManager::setAutomatic(bool enabled) {
    settings.setValue("updates/automatic", enabled);
    settings.sync();
    emit changed();
}

void UpdateManager::setBetaUpdates(bool enabled) {
    if (busy)
        return;
    settings.setValue("updates/beta", enabled);
    settings.sync();
    releases = {};
    manifest = {};
    available.clear();
    announcedVersion.clear();
    latestCheckedVersion.clear();
    latestCheckedAt.clear();
    // Refresh immediately when changing channel; do not wait for the daily timer.
    if (!testMode)
        check();
    else
        status("Update channel changed.");
}

void UpdateManager::browseReleases() {
    if (busy)
        return;
    browsing = true;
    busy = true;
    releases = {};
    manifest = {};
    available.clear();
    progress = 0;
    checkRelease();
}

void UpdateManager::selectRelease(const QString &version) {
    if (busy || version == QCoreApplication::applicationVersion())
        return;
    for (const auto &entry : releases) {
        if (entry.toObject()["tag_name"].toString() == "v" + version) {
            busy = true;
            manifest = {};
            prepareRelease(entry.toObject());
            return;
        }
    }
}

QNetworkRequest UpdateManager::updateRequest(const QUrl &url, bool asset) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "WHAT-updater");
    request.setRawHeader("Accept", asset ? "application/octet-stream" : "application/vnd.github+json");
    // Redirects are followed by hand so each hop goes through safeUrl().
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    return request;
}

QUrl UpdateManager::assetUrl(const QJsonObject &asset, const QString &tag) const {
    const QUrl browser(asset["browser_download_url"].toString());
    return releaseAsset(browser, tag, asset["name"].toString()) ? browser : QUrl{};
}

void UpdateManager::openReleases(const QString &version) {
    if (version.isEmpty() || validVersion(version))
        QDesktopServices::openUrl(QUrl(version.isEmpty() ? repo : repo + "/tag/v" + version));
}

bool UpdateManager::saveUpdateNotice(const QString &path, const QJsonObject &notice) {
    QSaveFile file(path);
    const auto data = QJsonDocument(notice).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

void UpdateManager::checkCompletedUpdate() {
    if (busy || !completedUpdate.isEmpty() || !family.startsWith("WHAT.Desktop_"))
        return;
    busy = true;
    const auto directory = profile + "/updates";
    const auto current = QCoreApplication::applicationVersion();
    const auto expectedFamily = family;
    downloadWorker.submit(
        [directory, current, expectedFamily, testing = testMode]() -> QVariantMap {
            QVariantMap outcome;
            if (!testing) {
                UINT32 size = 0;
                if (GetCurrentPackagePath(&size, nullptr) == ERROR_INSUFFICIENT_BUFFER) {
                    std::wstring path(size, L'\0');
                    if (GetCurrentPackagePath(&size, path.data()) == ERROR_SUCCESS)
                        outcome["certificateExpiry"] = signingCertificateExpiry(
                            QString::fromWCharArray(path.c_str()) + "/AppxSignature.p7x");
                }
            }
            const auto readReceipt = [&]() -> QVariantMap {
                QFile file(directory + "/pending.json");
                if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
                    return {};
                const auto notice = QJsonDocument::fromJson(file.readAll()).object();
                const auto previous = notice["previous"].toString();
                const auto target = notice["current"].toString();
                const auto attempt = notice["attempt"].toString();
                if (!validVersion(previous) || !validVersion(target) || previous == target ||
                    (target != current && previous != current) ||
                    notice["family"].toString() != expectedFamily || QUuid(attempt).isNull() ||
                    QUuid(attempt).toString(QUuid::WithoutBraces) != attempt)
                    return {};
                if (previous == current) {
                    QFile failure(directory + "/" + attempt + "/error.txt");
                    QFile code(directory + "/" + attempt + "/error-code.txt");
                    if (!failure.open(QIODevice::ReadOnly) || !code.open(QIODevice::ReadOnly) ||
                        code.size() > 128)
                        return {};
                    return {{"failureCode", QString::fromUtf8(code.readAll()).trimmed()},
                            {"failureDetails", QString::fromUtf8(failure.read(4000)).trimmed()},
                            {"version", target}};
                }
                // A downloaded package alone is not a completed installation.
                QFile result(directory + "/" + attempt + "/result.txt");
                if (QFile::exists(directory + "/" + attempt + "/error.txt") ||
                    !result.open(QIODevice::ReadOnly) || result.size() > 128 ||
                    result.readAll().trimmed() != ("Installed " + packageVersion(current)).toUtf8())
                    return {};
                return {{"previous", previous},
                        {"current", current},
                        {"notes", notice["notes"].toString().left(12000)}};
            };
            outcome["receipt"] = readReceipt();
            return outcome;
        },
        [this](QVariantMap outcome) {
            busy = false;
            certificateExpiry = outcome.value("certificateExpiry").toDateTime();
            const auto receipt = outcome.value("receipt").toMap();
            if (receipt.contains("failureCode")) {
                reportInstallFailure(receipt["failureCode"].toString(), receipt["failureDetails"].toString(),
                                     receipt["version"].toString());
                return;
            }
            completedUpdate = receipt;
            emit changed();
            if (!completedUpdate.isEmpty())
                emit updateCompleted(completedUpdate);
        });
}

void UpdateManager::dismissCompletedUpdate() {
    if (completedUpdate.isEmpty())
        return;
    completedUpdate.clear();
    const auto path = profile + "/updates/pending.json";
    downloadWorker.post([path] { QFile::remove(path); });
    emit changed();
}

bool UpdateManager::validVersion(const QString &v) {
    static const QRegularExpression re(
        R"(^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})(?:-beta\.([1-9][0-9]{0,4}))?\z)");
    const auto match = re.match(v);
    if (!match.hasMatch())
        return false;
    for (int i = 1; i <= 4; ++i)
        if (match.captured(i).toInt() > 65535)
            return false;
    return true;
}

QString UpdateManager::packageVersion(const QString &version) {
    if (!validVersion(version))
        return {};
    const auto parts = version.split("-beta.");
    return parts[0] + "." + (parts.size() == 2 ? parts[1] : QStringLiteral("0"));
}

bool UpdateManager::newer(const QString &candidate, const QString &current) {
    if (!validVersion(candidate) || !validVersion(current))
        return false;
    const auto a = candidate.split("-beta.");
    const auto b = current.split("-beta.");
    const int base =
        QVersionNumber::compare(QVersionNumber::fromString(a[0]), QVersionNumber::fromString(b[0]));
    if (base != 0)
        return base > 0;
    if (a.size() != b.size())
        return a.size() == 1;
    return a.size() == 2 && a[1].toInt() > b[1].toInt();
}

bool UpdateManager::safeUrl(const QUrl &url) {
    return url.scheme() == "https" && url.userInfo().isEmpty() && (url.port() == -1 || url.port() == 443) &&
           QStringList{"api.github.com", "github.com", "release-assets.githubusercontent.com",
                       "objects.githubusercontent.com"}
               .contains(url.host());
}

QString UpdateManager::validateManifest(const QJsonObject &m, const QString &version) {
    if (m["version"].toString() != version || !validVersion(version))
        return "Update manifest version does not match the release tag.";
    if (!((m["schema"].toInt() == 1 && m["minimumUpdaterVersion"].toInt() == 1 && !version.contains("-")) ||
          (m["schema"].toInt() == 2 && m["minimumUpdaterVersion"].toInt() == 2 &&
           m["packageVersion"].toString() == packageVersion(version))))
        return "This release needs a newer updater. Install it manually from Releases.";
    if (m["architecture"] != Architecture::name() ||
        m["installer"] != "WHAT-" + version + "-Windows-" + Architecture::name() + "-UpdatePayload.msix")
        return "Unsupported update package.";
    if (!QRegularExpression("^[a-fA-F0-9]{64}\\z").match(m["sha256"].toString()).hasMatch())
        return "Missing or invalid package SHA-256.";
    return {};
}

void UpdateManager::fetch(const QUrl &url, qint64 limit, std::function<void(QByteArray)> done, bool asset,
                          int redirects) {
    if (!safeUrl(url)) {
        busy = false;
        status("Rejected an untrusted update URL.", true);
        return;
    }
    const auto request = updateRequest(url, asset);
    auto *reply = transport->get(request);
    reply->setReadBufferSize(1024 * 1024);
    auto data = std::make_shared<QByteArray>();
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    deadline->start(10 * 60 * 1000);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::readyRead, this, [reply, data, limit] {
        data->append(reply->readAll());
        if (data->size() > limit) {
            reply->setProperty("oversized", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 read, qint64 total) {
        progress = total > 0 ? double(read) / total : 0;
        emit changed();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, data, limit, done, asset, redirects] {
        data->append(reply->readAll());
        reply->deleteLater();
        if (reply->property("oversized").toBool() || data->size() > limit) {
            busy = false;
            status("Update response exceeded the size limit.", true);
            return;
        }
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http == 301 || http == 302 || http == 303 || http == 307 || http == 308) {
            const auto next =
                reply->url().resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
            if (redirects >= 5 || !safeUrl(next)) {
                busy = false;
                status("Rejected update redirect.", true);
                return;
            }
            fetch(next, limit, done, asset, redirects + 1);
            return;
        }
        if (reply->error() != QNetworkReply::NoError || http != 200) {
            busy = false;
            status(QString("Update request failed (HTTP %1). ").arg(http) +
                       (http == 401 || http == 403 || http == 404
                            ? "No published release was found. Try again later or open Releases."
                            : "Check connectivity and try again."),
                   true);
            return;
        }
        done(*data);
    });
}

void UpdateManager::check() {
    if (busy)
        return;
    browsing = false;
    busy = true;
    releases = {};
    available.clear();
    manifest = {};
    progress = 0;
    checkRelease();
}

void UpdateManager::checkRelease() {
    if (browsing || settings.value("updates/beta", false).toBool()) {
        fetchReleaseList();
        return;
    }
    status("Checking GitHub for a stable release…");
    fetch(QUrl("https://api.github.com/repos/Chuffnugget/what-windows/releases/latest"), 2 * 1024 * 1024,
          [this](QByteArray data) {
              release = QJsonDocument::fromJson(data).object();
              auto tag = release["tag_name"].toString();
              if (!tag.startsWith('v') || !validVersion(tag.mid(1)) || release["draft"].toBool() ||
                  release["prerelease"].toBool() || tag.contains("-beta.")) {
                  busy = false;
                  status("GitHub returned an invalid stable release.", true);
                  return;
              }
              const auto version = tag.mid(1);
              rememberLatest(release);
              if (!newer(version, QCoreApplication::applicationVersion())) {
                  busy = false;
                  status("You are up to date (" + QCoreApplication::applicationVersion() + ").");
                  return;
              }
              prepareRelease(release);
          });
}

void UpdateManager::fetchReleaseList(int page) {
    status("Loading releases from GitHub…");
    fetch(QUrl(QString("https://api.github.com/repos/Chuffnugget/what-windows/releases?per_page=100&page=%1")
                   .arg(page)),
          8 * 1024 * 1024, [this, page](QByteArray data) {
              const auto document = QJsonDocument::fromJson(data);
              if (!document.isArray()) {
                  busy = false;
                  status("GitHub returned an invalid release list.", true);
                  return;
              }
              const auto entries = document.array();
              for (const auto &entry : entries) {
                  const auto item = entry.toObject();
                  const auto tag = item["tag_name"].toString();
                  const bool beta = tag.contains("-beta.");
                  if (!tag.startsWith('v') || !validVersion(tag.mid(1)) || item["draft"].toBool() ||
                      item["prerelease"].toBool() != beta ||
                      (beta && !settings.value("updates/beta", false).toBool()))
                      continue;
                  releases.append(item);
              }
              if (entries.size() == 100 && page < 10) {
                  fetchReleaseList(page + 1);
                  return;
              }
              if (entries.size() == 100) {
                  busy = false;
                  releases = {};
                  status("Release history exceeds the browser limit. Open Releases instead.", true);
                  return;
              }
              QList<QJsonObject> sorted;
              for (const auto &entry : releases)
                  sorted.append(entry.toObject());
              std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
                  return newer(a["tag_name"].toString().mid(1), b["tag_name"].toString().mid(1));
              });
              releases = {};
              for (const auto &item : sorted)
                  releases.append(item);
              if (!sorted.isEmpty())
                  rememberLatest(sorted.first());
              else {
                  latestCheckedVersion.clear();
                  latestCheckedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
              }
              if (!sorted.isEmpty() && newer(sorted.first()["tag_name"].toString().mid(1),
                                             QCoreApplication::applicationVersion())) {
                  prepareRelease(sorted.first());
                  return;
              }
              busy = false;
              status(browsing ? "Select a release to view its notes and prepare installation."
                              : "You are up to date for the selected update channel.");
          });
}

void UpdateManager::prepareRelease(const QJsonObject &candidateRelease) {
    release = candidateRelease;
    const auto tag = release["tag_name"].toString();
    const auto version = tag.mid(1);
    available = version;
    QUrl manifestUrl;
    for (const auto &v : release["assets"].toArray())
        if (v.toObject()["name"] == Architecture::updateManifest())
            manifestUrl = assetUrl(v.toObject(), tag);
    if (manifestUrl.isEmpty()) {
        busy = false;
        status("Version " + version +
                   " is available. Open Releases to install it manually; no supported update manifest was "
                   "published.",
               true);
        return;
    }
    fetch(
        manifestUrl, 65536,
        [this, version](QByteArray bytes) {
            const auto candidate = QJsonDocument::fromJson(bytes).object();
            const auto error = validateManifest(candidate, version);
            busy = false;
            if (!error.isEmpty()) {
                status(error, true);
                return;
            }
            bool found = false;
            for (const auto &v : release["assets"].toArray()) {
                auto asset = v.toObject();
                if (asset["name"] == candidate["installer"] && !assetUrl(asset, "v" + version).isEmpty() &&
                    asset["size"].toDouble() > 0 && asset["size"].toDouble() <= 256 * 1024 * 1024) {
                    found = true;
                    installerUrl = assetUrl(asset, "v" + version);
                    installerSize = qint64(asset["size"].toDouble());
                }
            }
            if (!found) {
                status("The release is missing its update package.", true);
                return;
            }
            manifest = candidate;
            status("Version " + version + " is available." +
                   (family.isEmpty() ? " Portable copy: download the new portable ZIP from Releases."
                                     : " Ready to download and install."));
            if (!browsing && newer(version, QCoreApplication::applicationVersion()) &&
                announcedVersion != version) {
                announcedVersion = version;
                emit updateAvailable(version);
            }
        },
        true);
}

void UpdateManager::install(const QString &expectedVersion) {
    if (!expectedVersion.isEmpty() && expectedVersion != available) {
        status("The selected release changed. Review it before installing.", true);
        return;
    }
    if (!info()["canInstall"].toBool())
        return;
    signingFailure = {};
    busy = true;
    downloading = true;
    progress = 0;
    downloadBytes = 0;
    downloadTotal = installerSize;
    status("Downloading signed update…");
    const auto folder = profile + "/updates/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto hash = manifest["sha256"].toString();
    const auto url = installerUrl;
    const auto noticePath = profile + "/updates/pending.json";
    const QJsonObject notice{{"attempt", QDir(folder).dirName()},
                             {"family", family},
                             {"previous", QCoreApplication::applicationVersion()},
                             {"current", available},
                             {"notes", release["body"].toString().left(12000)}};

    struct TransferProgress {
        std::atomic<qint64> bytes{0}, total{0};
    };

    auto transfer = std::make_shared<TransferProgress>();
    auto *progressTimer = new QTimer(this);
    connect(progressTimer, &QTimer::timeout, this, [this, transfer, progressTimer] {
        if (!downloading || !busy) {
            progressTimer->stop();
            progressTimer->deleteLater();
            return;
        }
        downloadBytes = transfer->bytes.load();
        const auto total = transfer->total.load();
        if (total > 0)
            downloadTotal = total;
        progress = downloadTotal > 0 ? qBound(0.0, double(downloadBytes) / downloadTotal, 1.0) : 0;
        emit changed();
    });
    progressTimer->start(100);
    downloadWorker.submit(
        [folder, hash, url, transfer, noticePath, notice] {
            QDir().mkpath(folder);
            QNetworkAccessManager manager;
            auto result = downloadUpdate(manager, url, folder + "/update.msix", hash,
                                         [transfer](qint64 bytes, qint64 total) {
                                             transfer->total.store(total);
                                             transfer->bytes.store(bytes);
                                         });
            if (result.error.isEmpty() && !saveUpdateNotice(noticePath, notice))
                qWarning("Could not save the update confirmation details");
            return result;
        },
        [this, folder, hash](DownloadResult result) {
            downloading = false;
            if (result.error.isEmpty())
                progress = 1;
            if (!result.error.isEmpty()) {
                busy = false;
                status(result.error, true);
                return;
            }
            QFile helper(":/scripts/Apply-Update.ps1");
            QFile copy(folder + "/Apply-Update.ps1");
            if (!helper.open(QIODevice::ReadOnly) || !copy.open(QIODevice::WriteOnly)) {
                busy = false;
                status("Could not prepare update helper.", true);
                return;
            }
            const auto script = helper.readAll();
            if (copy.write(script) != script.size() || !copy.flush()) {
                busy = false;
                status("Could not write update helper.", true);
                return;
            }
            copy.close();
            auto *process = new QProcess(this);
            // The helper validates trust and identity before it signals readiness.
            process->setCreateProcessArgumentsModifier(
                [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
            process->setProgram(qEnvironmentVariable("SystemRoot", "C:/Windows") +
                                "/System32/WindowsPowerShell/v1.0/powershell.exe");
            process->setArguments({"-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                   "-File", folder + "/Apply-Update.ps1", "-PackagePath",
                                   folder + "/update.msix", "-ExpectedHash", hash, "-ExpectedVersion",
                                   packageVersion(available), "-Family", family, "-ParentId",
                                   QString::number(QCoreApplication::applicationPid())});
            // Windows revisions order betas above stable .0. Consent also covers beta-to-stable installation.
            if (QVersionNumber::compare(
                    QVersionNumber::fromString(packageVersion(available)),
                    QVersionNumber::fromString(packageVersion(QCoreApplication::applicationVersion()))) < 0) {
                auto arguments = process->arguments();
                arguments.append("-AllowDowngrade");
                process->setArguments(arguments);
            }
            // A detached process survives app shutdown; readiness is communicated by a file.
            qint64 pid = 0;
            if (!process->startDetached(&pid)) {
                process->deleteLater();
                busy = false;
                status("Could not launch the update helper.", true);
                return;
            }
            process->deleteLater();
            status("Verifying Windows signature and installed publisher…");
            auto *timer = new QTimer(this);
            timer->setProperty("ticks", 0);
            connect(timer, &QTimer::timeout, this, [this, timer, folder] {
                QFile failure(folder + "/error.txt");
                const int ticks = timer->property("ticks").toInt() + 1;
                timer->setProperty("ticks", ticks);
                if (failure.open(QIODevice::ReadOnly)) {
                    timer->stop();
                    busy = false;
                    QFile code(folder + "/error-code.txt");
                    const auto reason = code.open(QIODevice::ReadOnly) && code.size() <= 128
                        ? QString::fromUtf8(code.readAll()).trimmed() : QString();
                    reportInstallFailure(reason, QString::fromUtf8(failure.read(4000)).trimmed(), available);
                    timer->deleteLater();
                } else if (QFile::exists(folder + "/ready")) {
                    QFile ready(folder + "/ready");
                    if (!ready.open(QIODevice::ReadOnly)) {
                        timer->stop();
                        busy = false;
                        status("Could not read update handoff; app left running.", true);
                        timer->deleteLater();
                        return;
                    }
                    bool valid = false;
                    const auto issued = QString::fromUtf8(ready.readAll()).trimmed().toLongLong(&valid);
                    const auto now = QDateTime::currentSecsSinceEpoch();
                    if (!valid || issued > now + 5 || now - issued > 20) {
                        timer->stop();
                        busy = false;
                        status("Update handoff expired or invalid; app left running.", true);
                        timer->deleteLater();
                        return;
                    }
                    QFile ack(folder + "/ack");
                    if (!ack.open(QIODevice::WriteOnly) || ack.write("ack") != 3 || !ack.flush()) {
                        timer->stop();
                        busy = false;
                        status("Could not acknowledge update handoff.", true);
                        timer->deleteLater();
                        return;
                    }
                    ack.close();
                    timer->stop();
                    status("Installing update; WHAT will restart.");
                    QCoreApplication::quit();
                    return;
                } else if (ticks >= 120) {
                    timer->stop();
                    busy = false;
                    status("Update verification timed out. The app was left running; see " + folder, true);
                    timer->deleteLater();
                }
            });
            timer->start(1000);
        });
}
