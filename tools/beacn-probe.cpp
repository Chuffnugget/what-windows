// Read-only by default. --roundtrip opts into small volume/brightness changes
// followed by restoration. --inspect checks a file without opening the driver.
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
    QTimer heartbeat;
    heartbeat.setInterval(20);
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++ticks; });
    heartbeat.start();
    const bool roundtrip = app.arguments().contains("--roundtrip");
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
        module.command(command, {}, epoch, 0, [&, command](bool ok, const QString &message) {
            // Wait for the vendor's queued UI/model updates before readback.
            QTimer::singleShot(1400, &app, [&, command, ok, message] {
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
                                           {"message", message}});
                // The following step restores the original value even if the
                // readback failed. The module still blocks uncertain dispatches.
                next();
            });
        });
    };
    QObject::connect(&module, &BeacnModule::snapshot, &app, [&](QVariantMap state) {
        state["main_thread_heartbeats"] = ticks;
        latest = state;
        if (started)
            return;
        started = true;
        if (!roundtrip || !state.value("ready").toBool()) {
            save(state);
            app.exit(state.value("ready").toBool() ? 0 : 2);
            return;
        }
        for (const auto &item : state["controls"].toList()) {
            const auto control = item.toMap();
            const auto id = control["id"].toString();
            if (!control["available"].toBool())
                continue;
            if (id == "volume_2" || id == "screen_brightness" || id == "button_brightness") {
                const double original = control["value"].toDouble();
                const double target = id == "volume_2"                       ? qMax(0.0, original - 0.25)
                                      : original < control["max"].toDouble() ? original + 1
                                                                             : original - 1;
                commands.append(QJsonObject{{"control", id}, {"value", target}});
                commands.append(QJsonObject{{"control", id}, {"value", original}});
            }
        }
        success = commands.size() == 6;
        QTimer::singleShot(0, &app, next);
    });
    QTimer::singleShot(60000, &app, [&] {
        save(latest);
        app.exit(3);
    });
    module.configure(true, {}, {});
    return app.exec();
}
