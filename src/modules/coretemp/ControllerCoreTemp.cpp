#include "app/Controller.h"

bool Controller::coretempHealthy() const {
    return moduleEnabled("coretemp") && coretempData["connected"].toBool() && coretempClock.isValid() &&
           coretempClock.elapsed() < qMax(15000, coretempTimer.interval() * 3);
}

void Controller::pollCoreTemp() {
    if (testMode || !moduleEnabled("coretemp") || coretempJob.isRunning())
        return;
    const auto generation = coretempGeneration;
    coretempJob.submit([] { return CoreTemp::read(); },
                       [this, generation](QJsonObject data) {
                           if (generation == coretempGeneration && moduleEnabled("coretemp"))
                               acceptCoreTemp(data);
                       });
}

void Controller::acceptCoreTemp(QJsonObject data) {
    if (!moduleEnabled("coretemp"))
        data = {{"connected", false}, {"status", "Disabled"}};
    if (coretempData["status"] != data["status"])
        log("Core Temp: " + data["status"].toString());
    coretempData = data;
    coretempClock.restart();
    mergeReadings("coretemp", data["readings"].toArray());
    publish();
    emit changed();
    emit sensorsChanged();
}
