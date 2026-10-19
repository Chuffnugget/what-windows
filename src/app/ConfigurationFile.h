#pragma once
#include <QJsonObject>
#include <QSettings>

namespace ConfigurationFile {
constexpr qint64 maximumBytes = 4 * 1024 * 1024;

struct Result {
    QJsonObject values;
    QString error;
    QString preview;
};

bool permission(const QString &key);
QJsonObject snapshot(QSettings &settings);
QByteArray encode(const QJsonObject &values);
Result decode(const QByteArray &data);
Result read(const QString &path);
QString write(const QString &path, const QByteArray &data);
QString applyPending(QSettings &settings);
} // namespace ConfigurationFile
