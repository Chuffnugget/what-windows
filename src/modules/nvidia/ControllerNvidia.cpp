#include "app/Controller.h"

bool Controller::nvidiaHealthy() const {
    return moduleEnabled("nvidia") && nvidiaData["connected"].toBool() && nvidiaClock.isValid() &&
           nvidiaClock.elapsed() < qMax(15000, nvidiaTimer.interval() * 3);
}

void Controller::pollNvidia() {
    if (testMode || !moduleEnabled("nvidia") || nvidiaJob.isRunning())
        return;
    const auto generation = nvidiaGeneration;
    nvidiaJob.submit([] { return Nvidia::read(); },
                     [this, generation](QJsonObject data) {
                         if (generation != nvidiaGeneration || !moduleEnabled("nvidia"))
                             return;
                         acceptNvidia(data);
                     });
}

void Controller::acceptNvidia(QJsonObject data) {
    if (!moduleEnabled("nvidia"))
        data = {{"connected", false}, {"status", "Disabled"}};
    if (nvidiaData["status"] != data["status"] || nvidiaData["errors"] != data["errors"])
        log("NVIDIA: " + data["status"].toString());
    nvidiaData = data;
    nvidiaClock.restart();
    mergeReadings("nvidia", data["readings"].toArray());
    publish();
    emit changed();
    emit sensorsChanged();
}
