#pragma once
#include <QJsonObject>
#include <atomic>
#include <memory>

class IcueSdk {
public:
    explicit IcueSdk(QString libraryPath = {});
    ~IcueSdk();
    QJsonObject poll(bool enabled);
    QString command(const QString &action, const QJsonObject &args);
    static QString validate(const QString &action, const QJsonObject &args);

private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
