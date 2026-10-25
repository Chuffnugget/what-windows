// Read-only by default. --roundtrip opts into small volume/brightness changes
// followed by restoration; --volume-roundtrip limits this to the Game fader.
// --volume-check sends its unchanged value; --samples repeats these timing checks.
// --inspect checks a file without opening the driver.
#include "modules/beacn/BeacnModule.h"
#include "modules/beacn/InstalledFiles.h"
#include <QCoreApplication>
#include <QTimer>
#include <QJsonDocument>
#include <QFile>
#include <QJsonArray>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("WHAT");
    app.setApplicationName("WHAT BEACN probe");
    const auto save = [&](const QVariantMap &state) {
        if (app.arguments().size() > 1) {
            QFile file(app.arguments()[1]);
            if (file.open(QIODevice::WriteOnly))
                file.write(QJsonDocument::fromVariant(state).toJson());
        }
    };
    const int inspect = app.arguments().indexOf("--inspect");
    if (inspect >= 0) {
        if (inspect + 1 >= app.arguments().size())
            return 1;
        const auto state = discoverInstalledFiles(app.arguments()[inspect + 1], {}, false);
        save(state);
        return state.value("appValid").toBool() ? 0 : 2;
    }
    BeacnModule module;
    int ticks = 0;
    qint64 maxMainThreadGap = 0;
    QElapsedTimer heartbeatClock;
    heartbeatClock.start();
    QTimer heartbeat;
    heartbeat.setInterval(20);
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] {
        ++ticks;
        maxMainThreadGap = qMax(maxMainThreadGap, heartbeatClock.restart());
    });
    heartbeat.start();
    const bool volumeCheck = app.arguments().contains("--volume-check");
    const bool volumeRoundtrip = app.arguments().contains("--volume-roundtrip");
    const bool volumeOnly = volumeCheck || volumeRoundtrip;
    const bool roundtrip = volumeOnly || app.arguments().contains("--roundtrip");
    const int samplesAt = app.arguments().indexOf("--samples");
    const int samples = samplesAt >= 0 && samplesAt + 1 < app.arguments().size()
                            ? qBound(1, app.arguments()[samplesAt + 1].toInt(), 20) : 1;
    int discoverySnapshots = 0;
    QVariantMap latest;
    QList<QJsonObject> commands;
    QJsonArray results;
    auto epoch = std::make_shared<std::atomic_uint64_t>(0);
    bool started = false;
    bool success = true;
    std::function<void()> next;
    next = [&] {
        if (commands.isEmpty()) {
            latest["roundtrip"] = results.toVariantList();
            latest["roundtrip_passed"] = success;
            save(latest);
            app.exit(success ? 0 : 4);
            return;
        }
        const auto command = commands.takeFirst();
        QElapsedTimer latency;
        latency.start();
        module.command(command, {}, epoch, 0, [&, command, latency](bool ok, const QString &message) {
            const auto commandMs = latency.elapsed();
            // Wait for the vendor's queued UI/model updates before readback.
            QTimer::singleShot(1400, &app, [&, command, ok, message, commandMs] {
                QJsonValue observed;
                for (const auto &item : latest["controls"].toList()) {
                    const auto control = item.toMap();
                    if (control["id"].toString() == command["control"].toString())
                        observed = QJsonValue::fromVariant(control["value"]);
                }
                const auto expected = command["value"];
                const bool matched = expected.isDouble() && observed.isDouble()
                                         ? qAbs(expected.toDouble() - observed.toDouble()) < 0.01
                                         : expected == observed;
                success &= ok && matched;
                results.append(QJsonObject{{"command", command},
                                           {"accepted", ok},
                                           {"matched", matched},
                                           {"observed", observed},
                                           {"message", message}, {"command_ms", commandMs},
                                           {"main_thread_max_gap_ms", maxMainThreadGap}});
                // The following step restores the original value even if the
                // readback failed. The module still blocks uncertain dispatches.
                next();
            });
        });
    };
    QObject::connect(&module, &BeacnModule::snapshot, &app, [&](QVariantMap state) {
        if (state.value("discovering").toBool())
            ++discoverySnapshots;
        state["discovery_snapshots"] = discoverySnapshots;
        state["main_thread_heartbeats"] = ticks;
        latest = state;
        if (started || !state.value("ready").toBool())
            return; // Initial native discovery runs independently of the command worker.
        started = true;
        if (!roundtrip || !state.value("ready").toBool()) {
            save(state);
            app.exit(state.value("ready").toBool() ? 0 : 2);
            return;
        }
        for (const auto &item : state["controls"].toList()) {
            const auto control = item.toMap();
            const auto id = control["id"].toString();
            if (!control["available"].toBool() || (volumeOnly && id != "volume_2"))
                continue;
            if (id == "volume_2" || id == "screen_brightness" || id == "button_brightness") {
                const double original = control["value"].toDouble();
                const double target = volumeCheck ? original
                    : id == "volume_2" ? qMax(0.0, original - 0.25)
                    : original < control["max"].toDouble() ? original + 1 : original - 1;
                for (int n = 0; n < (volumeOnly ? samples : 1); ++n) {
                    commands.append(QJsonObject{{"control", id}, {"value", target}});
                    if (!volumeCheck)
                        commands.append(QJsonObject{{"control", id}, {"value", original}});
                }
            }
        }
        success = commands.size() == (volumeOnly ? samples * (volumeCheck ? 1 : 2) : 6);
        QTimer::singleShot(0, &app, next);
    });
    QTimer::singleShot(60000, &app, [&] {
        save(latest);
        app.exit(3);
    });
    module.configure(true, {}, {});
    return app.exec();
}
