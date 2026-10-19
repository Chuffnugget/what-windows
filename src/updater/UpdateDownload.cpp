#include "updater/UpdateDownload.h"
#include "updater/UpdateManager.h"
#include <QNetworkReply>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>

DownloadResult downloadUpdate(QNetworkAccessManager &network, QUrl url, QString path, QString expected,
                              std::function<void(qint64, qint64)> progress) {
    DownloadResult result;
    QSaveFile file(path);
    if (!UpdateManager::safeUrl(url)) {
        result.error = "Rejected update URL";
        return result;
    }
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = file.errorString();
        return result;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    QElapsedTimer clock;
    clock.start();
    for (int redirects = 0; redirects <= 5; ++redirects) {
        // Each hop gets a new request so the redirect target is checked again.
        auto *reply = network.get(UpdateManager::updateRequest(url, true));
        reply->setReadBufferSize(65536);
        QEventLoop loop;
        QTimer watchdog;
        auto drain = [&] {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
                reply->readAll();
                return;
            }
            while (reply->bytesAvailable()) {
                const auto bytes = reply->read(65536);
                if (bytes.isEmpty())
                    break;
                total += bytes.size();
                if (total > 256 * 1024 * 1024) {
                    result.error = "Update exceeds 256 MB limit";
                    reply->abort();
                    break;
                }
                if (file.write(bytes) != bytes.size()) {
                    result.error = file.errorString();
                    reply->abort();
                    break;
                }
                hash.addData(bytes);
                if (progress)
                    progress(total, reply->header(QNetworkRequest::ContentLengthHeader).toLongLong());
            }
        };
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, drain);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, [&] {
            if (QThread::currentThread()->isInterruptionRequested() || clock.elapsed() > 600000) {
                result.error = "Download cancelled or timed out";
                reply->abort();
            }
        });
        watchdog.start(250);
        loop.exec();
        drain();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto next =
            reply->url().resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
        const auto error = reply->error();
        reply->deleteLater();
        if (!result.error.isEmpty())
            break;
        if (http == 301 || http == 302 || http == 303 || http == 307 || http == 308) {
            if (redirects >= 5 || !UpdateManager::safeUrl(next)) {
                result.error = "Rejected update redirect";
                break;
            }
            url = next;
            continue;
        }
        if (error != QNetworkReply::NoError || http != 200) {
            result.error =
                QString("Download failed (HTTP %1). Check GitHub access and try checking for updates again.")
                    .arg(http);
            break;
        }
        if (QString::fromLatin1(hash.result().toHex()).compare(expected, Qt::CaseInsensitive) != 0)
            result.error = "Update checksum mismatch. Nothing was installed.";
        if (result.error.isEmpty() && !file.commit())
            result.error = "Could not commit update file: " + file.errorString();
        if (result.error.isEmpty())
            result.path = path;
        break;
    }
    return result;
}
