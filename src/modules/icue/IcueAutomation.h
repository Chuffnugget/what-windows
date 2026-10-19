#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QMap>
#include <functional>

// Only used on the iCUE QThread. No vendor code or DLL dependency.
class IcueAutomation {
public:
    explicit IcueAutomation(QString endpoint = "corsair_automation_sdk_v2") : endpoint(std::move(endpoint)) {}

    QJsonObject poll(bool enabled, bool force = false, bool readTelemetry = true);
    QString command(const QJsonObject &args, const std::function<void()> &clearRgb,
                    const std::function<bool()> &cancelled);
    static QJsonArray lightingReadings(const QJsonObject &active, const QString &error = {});
    QString lightingCommand(const QJsonObject &args, const std::function<void()> &clearRgb,
                            const std::function<bool()> &cancelled);
    static QString validateLighting(const QJsonObject &args, const QJsonObject &state);
    static QString validate(const QJsonObject &args, const QJsonObject &state);

private:
    QString endpoint;
    QJsonObject cache;
    QMap<QString, QJsonObject> routes;
    QElapsedTimer inventoryAge, lightingInventoryAge;
    QJsonArray lightingEffects;
};
