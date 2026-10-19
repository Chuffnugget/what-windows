#include "app/Controller.h"

QVariantMap Controller::dynamicLightingState() const {
    auto result = lighting ? lighting->state() : QJsonObject{};
    if (!result["devices"].isArray())
        result["devices"] = QJsonArray{};
    const bool enabled =
        moduleEnabled("windows") && settings.value("modules/windows/lighting", false).toBool();
    result["enabled"] = enabled;
    result["feedback"] = lightingFeedback;
    result["control_enabled"] = enabled && settings.value("controls/dynamic_lighting", false).toBool();
    result["cloud"] = settings.value("modules/windows/lighting_cloud", false).toBool();
    if (!enabled) {
        result["devices"] = QJsonArray{};
        result["status"] = "Disabled";
    }
    return result.toVariantMap();
}

QJsonObject Controller::lightingShared() const {
    auto result = QJsonObject::fromVariantMap(dynamicLightingState());
    if (!result["control_enabled"].toBool())
        result["devices"] = QJsonArray{};
    return result;
}

void Controller::setDynamicLightingOption(const QString &option, bool enabled) {
    if (option != "enabled" && option != "control" && option != "cloud")
        return;
    ++*commandEpoch;
    const auto key = option == "control" ? QString("controls/dynamic_lighting")
                     : option == "cloud" ? QString("modules/windows/lighting_cloud")
                                         : QString("modules/windows/lighting");
    lightingFeedback.clear();
    settings.setValue(key, enabled);
    scheduleSettingsFlush();
    // Revoking remote access also cancels in-flight work and returns control to Windows.
    if (lighting && (option == "enabled" || !enabled))
        lighting->setEnabled(
            moduleEnabled("windows") && settings.value("modules/windows/lighting", false).toBool(), testMode);
    emit dynamicLightingChanged();
    publishPolicy();
}

void Controller::refreshDynamicLighting() {
    if (lighting)
        lighting->refresh();
}

void Controller::applyDynamicLighting(const QVariantMap &command) {
    if (!lighting || !dynamicLightingState()["enabled"].toBool())
        return;
    lighting->apply(QJsonObject::fromVariantMap(command), [this](bool ok, QString message) {
        lightingFeedback = ok ? "Colour applied" : message;
        log(QString("Dynamic Lighting: ") + lightingFeedback);
        emit dynamicLightingChanged();
    });
}

void Controller::handleDynamicLightingCommand(const QString &session, const QJsonObject &command) {
    const auto id = command["id"].toString();
    auto lease = server.commandLease(session);
    if (!lease || !lease->load())
        return;
    if (!lighting || !dynamicLightingState()["control_enabled"].toBool()) {
        server.replyCommand(session, id, false,
                            "Enable Dynamic Lighting and its Home Assistant control permission first.");
        return;
    }
    lighting->apply(command["args"].toObject(), [this, session, id](bool ok, QString message) {
        server.replyCommand(session, id, ok, message);
    });
}
