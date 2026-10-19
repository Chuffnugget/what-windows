#include "Controller.h"

// Use completed module results, not log text or the sensor catalogue. This runs
// once per cached UI snapshot; neither page needs to inspect thousands of rows.
QVariantMap Controller::moduleIssues() const {
    QMap<QString, QStringList> issues;
    const auto add = [&](const QString &module, const QString &message) {
        if (moduleEnabled(module) && !message.isEmpty() && !issues[module].contains(message))
            issues[module].append(message);
    };
    const auto reader = [&](const QString &module, const QJsonObject &data,
                            const QElapsedTimer &clock, bool healthy, const QString &label = QString{}) {
        // An invalid clock means no result yet, not a failed first connection.
        if (!clock.isValid() || healthy)
            return;
        const auto message = data["connected"].toBool()
                                 ? "Readings have stopped updating. Check the source application and retry."
                                 : data["status"].toString("The source did not provide readings.");
        add(module, label + message);
    };

    if (!healthy() && !latest.status.isEmpty())
        add("hwinfo", sourceStatus());
    reader("coretemp", coretempData, coretempClock, coretempHealthy());
    reader("nvidia", nvidiaData, nvidiaClock, nvidiaHealthy());
    for (const auto &error : nvidiaData["errors"].toArray())
        add("nvidia", error.toString());
    for (const auto &value : nvidiaData["devices"].toArray()) {
        const auto device = value.toObject();
        for (const auto &error : device["errors"].toArray())
            add("nvidia", device["name"].toString() + ": " + error.toString());
    }

    if (icueDirectEnabled())
        reader("icue", icueData, icueClock,
               icueData["connected"].toBool() && icueClock.isValid() && icueClock.elapsed() < 15000,
               "RGB SDK: ");
    if (icueAutomationEnabled())
        reader("icue", automationData, automationClock,
               icueAutomationState()["connected"].toBool(), "Automation: ");
    if (!icueDirectEnabled() && !icueAutomationEnabled())
        add("icue", "Enable the RGB SDK or Automation connection in this module's settings.");

    if (beacnData.contains("ready") && !beacnData["ready"].toBool()
        && !beacnData["discovering"].toBool())
        add("beacn", beacnData.value("status").toString());
    else if (!beacnData.value("compatibility_warning").toString().isEmpty())
        add("beacn", beacnData.value("compatibility_warning").toString());
    if (wallpaperData.contains("running") && (!wallpaperData["running"].toBool()
                                             || !wallpaperData["error"].toString().isEmpty()))
        add("wallpaper", wallpaperData["status"].toString("Start Wallpaper Engine to connect."));

    const auto sources = sourceOptions();
    if (sources["wifi"].toBool())
        add("windows", wifiIssue);
    const auto worker = [&](const QString &module, const QString &name, bool enabled = true) {
        if (enabled && workerErrors.contains(name) && !issues[module].contains(workerErrors.value(name)))
            add(module, name + ": " + workerErrors.value(name));
    };
    worker("hwinfo", "HWiNFO reader");
    worker("coretemp", "Core Temp reader");
    worker("nvidia", "NVIDIA NVML");
    worker("windows", "Windows sensors");
    worker("windows", "Slow Windows sources");
    worker("windows", "Storage inventory");
    worker("windows", "Media sessions", sources["media"].toBool());
    worker("windows", "Power plans", controlPermissions()["power_plan"].toBool());
    worker("windows", "Displays", displaysEnabled());
    worker("windows", "Dynamic Lighting", settings.value("modules/windows/lighting", false).toBool());
    if (windowsClock.isValid() && windowsClock.elapsed() >= qMax(30000, sampleTimer.interval() * 3))
        add("windows", "Windows readings have stopped updating.");
    if (displaysEnabled() && displayInfo.contains("available") && !displayInfo["available"].toBool())
        add("windows", "Displays: " + displayInfo["status"].toString());
    // These are fixed API status rows, not missing optional readings such as
    // battery life on a desktop or a track title when nothing is playing.
    if (sources["notifications"].toBool() && windowsSlowClock.isValid()) {
        const auto status = catalog.value("win_notifications_status")["value"].toString();
        if (status != "Ready" && !status.isEmpty())
            add("windows", "Notifications: " + status);
    }
    if (sources["media"].toBool() && mediaClock.isValid()) {
        const auto status = catalog.value("win_media_status")["value"].toString();
        if (status != "Ready" && status != "No media session" && !status.isEmpty())
            add("windows", "Media: " + status);
    }

    if (customSources)
        issues["custom"] = customSources->issues();
    if (homeAssistantWeb)
        issues["haweb"] = homeAssistantWeb->issues();

    QVariantMap result;
    for (auto it = issues.cbegin(); it != issues.cend(); ++it)
        if (!it.value().isEmpty())
            result[it.key()] = it.value().join('\n');
    return result;
}
