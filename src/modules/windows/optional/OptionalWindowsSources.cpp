#include "OptionalWindowsSources.h"
#include "core/BackgroundWorker.h"
#include <algorithm>

namespace OptionalWindows {
bool reportedInUse(quint64 start, quint64 stop) {
    return start != 0 && (stop == 0 || start > stop);
}

QStringList keys() {
    return {"audio_activity",  "audio_levels", "camera_usage", "printers", "windows_updates",
            "virtual_desktop", "location",     "bluetooth_le", "bluetooth"};
}

QString featureForId(const QString &id) {
    if (!id.startsWith("win_opt_"))
        return {};
    for (const auto &key : keys())
        if (id.startsWith("win_opt_" + key + '_'))
            return key;
    return {};
}

int scopeForId(const QString &id) {
    const int index = keys().indexOf(featureForId(id));
    return index < 0 ? 0 : 8 + index;
}

bool permitted(const QString &id, const QJsonObject &options) {
    const auto key = featureForId(id);
    return !id.startsWith("win_opt_") || (!key.isEmpty() && options[key].toBool());
}
} // namespace OptionalWindows

struct OptionalWindowsSources::Job {
    // Optional native calls may be waiting for a device. Revocation must not wait on the GUI.
    BackgroundWorker worker{"Optional Windows source", nullptr, 0};
    std::shared_ptr<OptionalWindows::Backend *> backend =
        std::make_shared<OptionalWindows::Backend *>(nullptr);
    qint64 due = 0, started = 0;
    bool timedOut = false;

    ~Job() {
        // FIFO cleanup releases COM and advertisement callbacks on their owner thread.
        worker.post([slot = backend] {
            delete *slot;
            *slot = nullptr;
        });
    }
};

OptionalWindowsSources::OptionalWindowsSources(QObject *parent) : QObject(parent) {
    clock.start();
    connect(&timer, &QTimer::timeout, this, &OptionalWindowsSources::tick);
}

OptionalWindowsSources::~OptionalWindowsSources() = default;

void OptionalWindowsSources::setIssue(const QString &feature, const QString &message) {
    if (issueValues.value(feature).toString() == message)
        return;
    if (message.isEmpty())
        issueValues.remove(feature);
    else
        issueValues[feature] = message;
    emit issuesChanged();
}

void OptionalWindowsSources::configure(const QJsonObject &input, bool enabled, bool testing, int interval) {
    QJsonObject next;
    for (const auto &key : OptionalWindows::keys())
        next[key] = enabled && input[key].toBool();
    const int bounded = std::clamp(interval, 200, 2000);
    if (options == next && testMode == testing && publishMs == bounded)
        return;
    options = next;
    testMode = testing;
    publishMs = bounded;
    ++generation; // A disabled source must never accept a result already in flight.
    lastAudioPublish = -1;
    meterValues.clear();
    emit metersChanged();
    const auto wanted = [&](const QString &key) {
        return key == "audio" ? options["audio_activity"].toBool() || options["audio_levels"].toBool()
                              : options[key].toBool();
    };
    for (auto it = jobs.begin(); it != jobs.end();) {
        if (!wanted(it->first) || (testMode && !testCollector))
            it = jobs.erase(it);
        else {
            it->second->due = 0;
            ++it;
        }
    }
    for (const auto &key : OptionalWindows::keys())
        if (!options[key].toBool()) {
            setIssue(key, {});
            emit readingsReady(key, {});
        }
    updateTimer();
    tick();
}

void OptionalWindowsSources::watchMeter(const QString &id, bool visible) {
    if (!id.startsWith("win_opt_audio_levels_"))
        return;
    if (visible)
        watched.insert(id);
    else
        watched.remove(id);
    updateTimer();
}

void OptionalWindowsSources::updateTimer() {
    bool any = false;
    for (const auto &value : options)
        any |= value.toBool();
    if (!any || (testMode && !testCollector)) {
        timer.stop();
        return;
    }
    const int interval = options["audio_levels"].toBool() && !watched.isEmpty() ? 33
                         : options["audio_levels"].toBool()                     ? publishMs
                                                                                : 1000;
    if (!timer.isActive() || timer.interval() != interval)
        timer.start(interval);
}

void OptionalWindowsSources::refreshLocation() {
    if (auto it = jobs.find("location"); it != jobs.end())
        it->second->due = 0;
    tick();
}

void OptionalWindowsSources::tick() {
    if (testMode && !testCollector)
        return;
    const qint64 now = clock.elapsed();
    QStringList enabled;
    for (const auto &key : OptionalWindows::keys())
        if (options[key].toBool() && !key.startsWith("audio_"))
            enabled.append(key);
    if (options["audio_levels"].toBool() || options["audio_activity"].toBool())
        enabled.append("audio");
    for (const auto &feature : enabled) {
        auto &job = jobs[feature];
        if (!job)
            job = std::make_unique<Job>();
        if (job->worker.isRunning()) {
            // Some spooler/driver calls cannot be cancelled. Withhold stale values
            // while the bounded queue waits; do not launch replacement threads.
            if (!job->timedOut && now - job->started > 30000) {
                job->timedOut = true;
                const QStringList affected =
                    feature == "audio" ? QStringList{"audio_activity", "audio_levels"} : QStringList{feature};
                for (const auto &key : affected)
                    if (options[key].toBool()) {
                        setIssue(
                            key,
                            "Windows has not replied for 30 seconds. Waiting for the current request; other sources continue.");
                        emit readingsReady(key, {});
                    }
                if (feature == "audio") {
                    meterValues.clear();
                    emit metersChanged();
                }
            }
            continue;
        }
        if (now < job->due)
            continue;
        const int period = feature == "windows_updates" ? 900000
                           : feature == "printers"      ? 30000
                           : feature == "location"      ? 60000
                           : feature == "bluetooth"     ? 10000
                           : feature == "audio" && options["audio_levels"].toBool()
                               ? (watched.isEmpty() ? publishMs : 33)
                               : 1000;
        job->due = now + period;
        job->started = now;
        job->timedOut = false;
        const auto epoch = generation;
        job->worker.submit(
            [slot = job->backend, feature, opts = options, fake = testCollector] {
                if (fake)
                    return fake(feature, opts);
                try {
                    if (!*slot)
                        *slot = new OptionalWindows::Backend;
                    return (*slot)->collect(feature, opts);
                } catch (...) {
                    OptionalWindows::Result failed;
                    failed.error = "Windows could not initialise this optional source.";
                    return failed;
                }
            },
            [this, epoch, feature](OptionalWindows::Result result) {
                if (epoch != generation)
                    return;
                if (feature == "audio") {
                    if (meterValues != result.meters) {
                        meterValues = result.meters;
                        emit metersChanged();
                    }
                    if (lastAudioPublish >= 0 && clock.elapsed() - lastAudioPublish < publishMs)
                        return;
                    lastAudioPublish = clock.elapsed();
                    for (const auto &key : {QString("audio_activity"), QString("audio_levels")}) {
                        if (!options[key].toBool())
                            continue;
                        QJsonArray rows;
                        for (const auto &value : result.rows)
                            if (OptionalWindows::featureForId(value.toObject()["id"].toString()) == key)
                                rows.append(value);
                        setIssue(key, result.error);
                        emit readingsReady(key, rows);
                    }
                } else {
                    setIssue(feature, result.error);
                    emit readingsReady(feature, result.rows);
                }
            });
    }
}
