#pragma once
#include <QString>
#include <QUrl>
#include <functional>
#include <QNetworkAccessManager>

struct DownloadResult {
    QString path, error;
};

DownloadResult downloadUpdate(QNetworkAccessManager &network, QUrl url, QString path, QString sha256,
                              std::function<void(qint64, qint64)> progress = {});
