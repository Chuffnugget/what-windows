#include "Controller.h"
#include <QFileDialog>

QString Controller::browseMqttFile(bool privateKey) {
    if (testMode)
        return {};
    return QFileDialog::getOpenFileName(nullptr,
                                        privateKey ? "Select client private key" : "Select certificate", {},
                                        privateKey ? "Private keys (*.pem *.key);;All files (*)"
                                                   : "Certificates (*.pem *.crt *.cer);;All files (*)");
}

QVariantMap Controller::trayConnectionInfo() const {
    return {{"start", "Start Home Assistant Connections"},
            {"stop", "Stop Home Assistant Connections"},
            {"restart", "Restart Home Assistant Connections"},
            {"active", server.isListening() || server.isStarting()}};
}

void Controller::startTrayConnection() { startService(); }
void Controller::stopTrayConnection() { stopService(); }
void Controller::restartTrayConnection() {
    stopService();
    startService();
}

QVariantMap Controller::connectionProgress() const {
    QVariantMap progress{{"phase", "stopped"}, {"busy", false}, {"error", false},
                         {"title", "Home Assistant Connections Stopped"},
                         {"detail", "Start connections from Settings or the tray menu."}};
    if (!settings.value("service_enabled", true).toBool())
        return progress;
    if (server.isStarting() || (!tlsReady && certificateJob.isRunning())) {
        progress["phase"] = "starting";
        progress["busy"] = true;
        progress["title"] = "Starting Home Assistant Connections";
        progress["detail"] = serverStatus;
        return progress;
    }
    if (!server.isListening()) {
        progress["phase"] = "failed";
        progress["error"] = true;
        progress["title"] = "Home Assistant Connections Could Not Start";
        progress["detail"] = serverStatus;
        return progress;
    }
    int enabled = 0, connected = 0;
    QStringList waiting;
    for (const auto &value : installations()) {
        const auto row = value.toMap();
        if (!row.value("enabled").toBool()) continue;
        ++enabled;
        if (row.value("connected").toBool()) ++connected;
        else waiting.append(row.value("name").toString());
    }
    progress["phase"] = enabled == 0 ? "disabled" : connected == enabled ? "connected" : "waiting";
    progress["busy"] = enabled > connected;
    progress["since"] = connectionWaitingSince;
    progress["title"] = enabled == 0 ? "No Home Assistant Installations Enabled"
        : QString("%1 / %2 Home Assistant Installations Connected").arg(connected).arg(enabled);
    progress["detail"] = enabled == 0 ? "Add or enable an installation in Connection Settings."
        : waiting.isEmpty() ? "Sharing with all enabled installations."
        : "Waiting for: " + waiting.join(", ");
    return progress;
}
