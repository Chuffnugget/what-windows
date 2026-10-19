#include "app/Controller.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QColor>
#include <algorithm>

bool Controller::icueDirectEnabled() const {
    return moduleEnabled("icue") && settings.value("modules/icue/direct", true).toBool();
}

bool Controller::icueAutomationEnabled() const {
    return moduleEnabled("icue") && settings.value("modules/icue/automation", false).toBool();
}

void Controller::setIcueOption(const QString &option, bool enabled) {
    if (option != "direct" && option != "automation" && option != "automationControl")
        return;
    ++*commandEpoch;
    ++*localIcueEpoch;
    for (const auto &request : localIcueQueue)
        emit localIcueReply(request["requestId"].toString(), false, "iCUE configuration changed");
    localIcueQueue.clear();
    settings.setValue("modules/icue/" + option, enabled);
    if (enabled && option == "direct") {
        icueClock.invalidate();
        icueData = {};
    }
    if (enabled && option == "automation") {
        automationClock.invalidate();
        automationData = {};
    }
    scheduleSettingsFlush();
    if (option == "direct" && !enabled) {
        icueReset = true;
        acceptIcue({});
    }
    if (option == "automation" && !enabled)
        acceptIcueAutomation({});
    log("iCUE " + option + (enabled ? " enabled" : " disabled"));
    pollIcue();
    browserPending = true;
    if (browserVisible && !browserInteracting)
        refreshBrowserAsync();
    publishPolicy();
    emit changed();
    emit sensorsChanged();
}

QJsonObject Controller::icueAutomationState() const {
    auto data = automationData;
    data.remove("readings");
    const bool enabled = icueAutomationEnabled();
    data["enabled"] = enabled;
    data["control_enabled"] = enabled && settings.value("modules/icue/automationControl", false).toBool();
    data["connected"] = enabled && data["connected"].toBool() && automationClock.isValid() &&
                        automationClock.elapsed() < 15000;
    if (!enabled) {
        data["controls"] = QJsonArray{};
        data["status"] = "Disabled";
    }
    if (!data["controls"].isArray())
        data["controls"] = QJsonArray{};
    if (!data["status"].isString())
        data["status"] = "Connecting to iCUE Automation API";
    return data;
}

void Controller::acceptIcueAutomation(QJsonObject data) {
    if (!icueAutomationEnabled())
        data = {{"enabled", false}, {"connected", false}, {"controls", QJsonArray{}}, {"status", "Disabled"}};
    if (automationData["status"] != data["status"])
        log("iCUE Automation: " + data["status"].toString());
    const bool modified = automationData != data;
    automationData = data;
    automationClock.start();
    mergeReadings("icue", data["readings"].toArray(), 4);
    if (automationData["telemetry_status"] != lastAutomationTelemetryStatus) {
        lastAutomationTelemetryStatus = automationData["telemetry_status"].toString();
        if (!lastAutomationTelemetryStatus.isEmpty())
            log("iCUE readings: " + lastAutomationTelemetryStatus);
    }
    if (modified) {
        publish();
        emit changed();
        emit sensorsChanged();
    }
}

void Controller::setIcueControl(bool enabled) {
    ++*commandEpoch;
    settings.setValue("modules/icue/control", enabled);
    scheduleSettingsFlush();
    if (!enabled)
        icueReset = true;
    icueData["control_enabled"] = moduleEnabled("icue") && enabled;
    pollIcue();
    publishPolicy();
    log(enabled ? "iCUE remote control enabled" : "iCUE remote control disabled; returning lighting to iCUE");
}

void Controller::acceptIcue(QJsonObject data) {
    if (!icueDirectEnabled())
        data = {{"enabled", false}, {"connected", false}, {"status", "Disabled"}, {"devices", QJsonArray{}}};
    data["control_enabled"] = icueDirectEnabled() && settings.value("modules/icue/control", false).toBool();
    if (icueData["status"] != data["status"])
        log("iCUE: " + data["status"].toString());
    for (auto item : data["devices"].toArray()) {
        const auto device = item.toObject();
        QString previous;
        for (auto old : icueData["devices"].toArray())
            if (old.toObject()["id"] == device["id"])
                previous = old.toObject()["topology_status"].toString();
        if (!device["topology_status"].toString().isEmpty() &&
            previous != device["topology_status"].toString())
            log("iCUE " + device["model"].toString() + ": " + device["topology_status"].toString());
    }
    icueData = data;
    icueClock.start();
    QJsonArray readings;
    for (auto item : data["devices"].toArray()) {
        const auto device = item.toObject();
        const auto prefix = "icue_" + QString(QCryptographicHash::hash(device["id"].toString().toUtf8(),
                                                                       QCryptographicHash::Sha256)
                                                  .toHex()
                                                  .left(24));
        auto add = [&](QString suffix, QString name, QJsonValue value, QString unit = {}) {
            readings.append(QJsonObject{{"id", prefix + "_" + suffix},
                                        {"source", "icue"},
                                        {"group", device["model"].toString()},
                                        {"name", name},
                                        {"value", value},
                                        {"available", true},
                                        {"kind", value.isBool() ? "binary" : "number"},
                                        {"unit", unit},
                                        {"type", 0},
                                        {"state_class", ""},
                                        {"recommended", true}});
        };
        add("led_count", "Controllable LEDs", device["leds"].toArray().size());
        add("channels", "Lighting channels", device["channels"]);
        for (auto property : device["properties"].toArray()) {
            const auto p = property.toObject();
            add(QString::number(p["id"].toInt()), p["name"].toString(), p["value"],
                p["id"].toInt() == 9 ? "%" : "");
        }
    }
    mergeReadings("icue", readings, 5);
    publish();
    emit changed();
    emit sensorsChanged();
}

void Controller::pollIcue() {
    runLocalIcueCommand();
    const auto generation = localIcueEpoch->load();
    const bool direct = icueDirectEnabled() && !testMode;
    const bool automated = icueAutomationEnabled() && !testMode;
    if (!icueJob.isRunning() && !icueRgbHandoff && (direct || icueData["enabled"].toBool() || icueReset)) {
        auto sdk = icueSdk;
        const bool reset = icueReset;
        icueReset = false;
        icueJob.onStatus = [this](QString error) {
            if (!error.isEmpty())
                acceptIcue({{"status", error}});
        };
        icueJob.submit(
            [sdk, direct, reset] {
                if (reset)
                    sdk->poll(false);
                return sdk->poll(direct);
            },
            [this, generation](QJsonObject data) {
                if (localIcueEpoch->load() == generation)
                    acceptIcue(data);
                // A configuration change can request another release while this job
                // is active. Drain it now instead of blocking queued commands until
                // the next periodic poll (which may be stopped).
                if (icueReset)
                    pollIcue();
                else
                    runLocalIcueCommand();
            });
    }
    if (!icueAutomationJob.isRunning() && !icueAutomationPending &&
        (automated || automationData["enabled"].toBool())) {
        auto automation = icueAutomation;
        icueAutomationJob.onStatus = [this](QString error) {
            if (!error.isEmpty())
                acceptIcueAutomation({{"status", error}});
        };
        icueAutomationJob.submit([automation, automated] { return automation->poll(automated); },
                                 [this, generation](QJsonObject data) {
                                     if (localIcueEpoch->load() == generation)
                                         acceptIcueAutomation(data);
                                     runLocalIcueCommand();
                                 });
    }
}

namespace {
bool automationAction(const QString &action) {
    return action == "icue_automation" || action == "icue_lighting";
}

bool releasesRgb(const QString &action, const QJsonObject &args) {
    return action == "icue_lighting" || (action == "icue_automation" && args["control"] == "profile");
}
} // namespace

bool Controller::icueCommandBusy(const QString &action, const QJsonObject &args) const {
    if (automationAction(action))
        return icueAutomationJob.isRunning() || icueAutomationPending ||
               (releasesRgb(action, args) && (icueJob.isRunning() || icueReset));
    return icueJob.isRunning() || icueReset || icueRgbHandoff;
}

// Profile and Lighting Link changes must release direct RGB first. Keep that handoff
// asynchronous across the two workers; its timeout must also prevent late dispatch.
void Controller::submitIcueCommand(const QString &action, const QJsonObject &args,
                                   std::function<bool()> cancelled, std::function<void(QString)> done) {
    const bool automated = automationAction(action), testing = testMode;
    const bool release = automated && releasesRgb(action, args) && !testing;
    if (automated)
        icueAutomationPending = true;
    if (release)
        icueRgbHandoff = true;
    auto finished = std::make_shared<bool>(false); // GUI-thread completion guard.
    auto finish = [this, finished, automated, release, done](QString error) {
        if (*finished)
            return;
        *finished = true;
        if (automated)
            icueAutomationPending = false;
        if (release)
            icueRgbHandoff = false;
        done(error);
    };
    auto sdk = icueSdk;
    auto automation = icueAutomation;
    auto start = [this, action, args, cancelled, finish, finished, sdk, automation, automated, testing] {
        // The SDK may reply after the timeout has already reported failure to the caller.
        if (*finished)
            return;
        if (cancelled()) {
            finish("iCUE command cancelled");
            return;
        }
        auto &job = automated ? icueAutomationJob : icueJob;
        job.onStatus = [finish](QString error) {
            if (!error.isEmpty())
                finish(error);
        };
        job.submit(
            [action, args, cancelled, sdk, automation, testing] {
                if (cancelled())
                    return QString("iCUE command cancelled");
                if (testing)
                    return QString{};
                // RGB release, when required, has completed on the SDK's own thread.
                if (action == "icue_lighting")
                    return automation->lightingCommand(args, [] {}, cancelled);
                if (action == "icue_automation")
                    return automation->command(args, [] {}, cancelled);
                return sdk->command(action, args);
            },
            finish);
    };
    if (!release) {
        start();
        return;
    }
    // Do not block either the GUI or Automation worker waiting for the SDK.
    // A late SDK reply must never dispatch the abandoned Automation command.
    auto releaseResolved = std::make_shared<bool>(false);
    QTimer::singleShot(2000, this, [finished, finish, releaseResolved] {
        if (!*finished && !*releaseResolved)
            finish("RGB release timed out; Automation command was not sent. Check iCUE before retrying.");
    });
    icueJob.onStatus = [finish](QString error) {
        if (!error.isEmpty())
            finish(error);
    };
    icueJob.submit(
        [sdk, cancelled] {
            if (cancelled())
                return false;
            sdk->poll(false);
            return true;
        },
        [start, finish, releaseResolved](bool released) {
            *releaseResolved = true;
            if (released)
                start();
            else
                finish("iCUE command cancelled");
        });
}

QVariantList Controller::icueBrowserRows(QVariantList rows) const {
    return makeIcueBrowserRows(rows, icueData, icueDirectEnabled(),
                               icueData["connected"].toBool() && icueClock.isValid() &&
                                   icueClock.elapsed() < 15000,
                               icueAutomationState(), favourites);
}

QVariantList Controller::makeIcueBrowserRows(QVariantList rows, QJsonObject icueData, bool directEnabled,
                                             bool available, QJsonObject state, QSet<QString> favourites) {
    if (state["enabled"].toBool()) {
        const auto lighting = state["lighting"].toObject();
        const auto active = lighting["effect"].toString();
        const auto params = lighting["parameters"].toObject();
        QJsonArray supported;
        QVariantList effectOptions;
        for (auto v : lighting["effects"].toArray()) {
            const auto effect = v.toObject();
            effectOptions.append(
                QVariantMap{{"id", effect["id"].toString()}, {"name", effect["name"].toString()}});
            if (effect["id"] == active)
                supported = effect["parameters"].toArray();
        }
        auto addLighting = [&](QString parameter, QString name, QVariant value, QVariantList options = {},
                               bool rgb = false) {
            const QString id = "icue_lighting_control_" + parameter;
            const QColor colour(value.toString());
            QVariantMap row{
                {"id", id},
                {"source", "icue"},
                {"icueAutomation", true},
                {"controlOnly", true},
                {"control", rgb ? "rgb" : "lightingChoice"},
                {"lightingParameter", parameter},
                {"lightingEffect", active},
                {"categoryKey", "icue_automation_lighting"},
                {"categoryName", "Lighting Link"},
                {"group", "Lighting Link"},
                {"name", name},
                {"value", value},
                {"options", options},
                {"available", state["connected"].toBool() && lighting["available"].toBool()},
                {"enabled", false},
                {"favourite", favourites.contains(id)},
                {"tooltip",
                 "iCUE renders this Lighting Link override across supported devices. Stop override returns control to profile lighting."}};
            if (rgb)
                row["rgb"] = QVariantList{colour.isValid() ? colour.red() : 255,
                                          colour.isValid() ? colour.green() : 255,
                                          colour.isValid() ? colour.blue() : 255};
            rows.append(row);
        };
        if (!effectOptions.isEmpty())
            addLighting("effect", "Lighting Link effect", active, effectOptions);
        for (const auto &parameter :
             {QString("Speed"), QString("Direction"), QString("Rotation"), QString("RandomColor")})
            if (supported.contains(parameter)) {
                QVariantList options;
                const int maximum = parameter == "Speed" ? 2 : parameter == "Direction" ? 3 : 1;
                for (int value = 0; value <= maximum; ++value)
                    options.append(QVariantMap{{"id", QString::number(value)},
                                               {"name", parameter == "RandomColor"
                                                            ? (value ? "Random colours" : "Chosen colours")
                                                            : parameter + " " + QString::number(value)}});
                addLighting(parameter, parameter == "RandomColor" ? "Random colours" : parameter,
                            params.contains(parameter)
                                ? QString::number(parameter == "RandomColor" ? int(params[parameter].toBool())
                                                                             : params[parameter].toInt())
                                : QString(),
                            options);
            }
        for (const auto &parameter : {QString("Color1"), QString("Color2")})
            if (supported.contains(parameter))
                addLighting(parameter, parameter == "Color1" ? "Effect colour 1" : "Effect colour 2",
                            params[parameter].toString(), {}, true);
        for (auto value : state["controls"].toArray()) {
            const auto control = value.toObject();
            const auto key = "icue_auto_" + control["id"].toString();
            rows.append(QVariantMap{
                {"id", key},
                {"name", control["name"].toString()},
                {"source", "icue"},
                {"group", "Profiles & device controls / " + control["device"].toString()},
                {"categoryKey", "icue_automation/" + control["id"].toString()},
                {"categoryName", control["device"].toString()},
                {"icueAutomation", true},
                {"controlOnly", true},
                {"control", "automation"},
                {"automationId", control["id"].toString()},
                {"options", control["options"].toArray().toVariantList()},
                {"value", control["current"].toString()},
                {"available", state["connected"].toBool()},
                {"enabled", false},
                {"favourite", favourites.contains(key)},
                {"tooltip",
                 control["confirmed"].toBool()
                     ? "Select an existing iCUE profile. WHAT's RGB overrides are cleared first."
                     : "Apply an existing iCUE preset. Current preset cannot be read back by this API."}});
        }
    }
    if (!directEnabled)
        return rows;
    QMap<QString, int> modelCounts;
    for (auto item : icueData["devices"].toArray())
        ++modelCounts[item.toObject()["model"].toString()];
    for (auto item : icueData["devices"].toArray()) {
        const auto device = item.toObject();
        const auto id = device["id"].toString();
        const auto prefix =
            "icue_" +
            QString(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
        auto deviceName = device["model"].toString();
        if (deviceName.isEmpty())
            deviceName = "Corsair device";
        if (modelCounts[device["model"].toString()] > 1)
            deviceName += " · " + prefix.right(6);
        for (auto &entry : rows) {
            auto row = entry.toMap();
            if (row.value("id").toString().startsWith(prefix + "_")) {
                row["categoryKey"] = prefix;
                row["categoryName"] = deviceName;
                row["group"] = deviceName;
                row["deviceName"] = deviceName;
                entry = row;
            }
        }
        for (auto value : device["properties"].toArray()) {
            const auto property = value.toObject();
            const int number = property["id"].toInt();
            if (!property["writable"].toBool() || number < 2 || number > 5)
                continue;
            for (auto &entry : rows) {
                auto row = entry.toMap();
                if (row.value("id") != prefix + "_" + QString::number(number))
                    continue;
                row["control"] = number == 5 ? "preset" : "toggle";
                row["icueDevice"] = id;
                row["icueProperty"] = number;
                entry = row;
                break;
            }
        }
        auto add = [&](QString suffix, QString name, QString group, QJsonArray rgb, QString control,
                       QVariant led = {}) {
            const auto key = prefix + "_" + suffix;
            QVariantMap row{{"id", key},
                            {"name", name},
                            {"group", group},
                            {"source", "icue"},
                            {"available", available},
                            {"controlOnly", true},
                            {"control", control},
                            {"icueDevice", id},
                            {"kind", "text"},
                            {"unit", ""},
                            {"value", rgb.size() == 3
                                          ? QColor(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt()).name()
                                          : QString()},
                            {"rgb", rgb.toVariantList()},
                            {"favourite", favourites.contains(key)},
                            {"enabled", false}};
            row["categoryKey"] = led.isValid() ? prefix + "/leds" : prefix;
            row["categoryName"] = led.isValid() ? "Individual LEDs" : deviceName;
            row["deviceName"] = deviceName;
            row["tooltip"] = deviceName + "\n" + name;
            if (led.isValid()) {
                row["icueLed"] = led;
                row["parentCategory"] = prefix;
                row["tooltip"] = deviceName + "\n" + name + "\nSDK LED ID: " + led.toString();
            }
            rows.append(row);
        };
        // A hub exposes one SDK device; its channel metadata identifies attached lighting devices.
        QMap<QString, QList<QJsonObject>> segments;
        for (auto value : device["leds"].toArray()) {
            const auto led = value.toObject();
            if (!led["segment"].toString().isEmpty())
                segments[led["segment"].toString()].append(led);
        }
        const auto leds = device["leds"].toArray();
        if (leds.isEmpty())
            continue;
        int r = 0, g = 0, b = 0, count = 0;
        for (auto led : leds) {
            auto rgb = led.toObject()["rgb"].toArray();
            if (rgb.size() == 3) {
                r += rgb[0].toInt();
                g += rgb[1].toInt();
                b += rgb[2].toInt();
                ++count;
            }
        }
        const auto model = deviceName;
        add("rgb", "Device RGB lighting", model,
            count ? QJsonArray{r / count, g / count, b / count} : QJsonArray{}, "rgb");
        add("release", "Clear lighting override", model, {}, "release");
        for (auto it = segments.cbegin(); it != segments.cend(); ++it) {
            QJsonArray subset;
            int sr = 0, sg = 0, sb = 0, n = 0;
            const auto title = it.value().first()["segment_name"].toString();
            for (const auto &led : it.value()) {
                subset.append(led["id"]);
                const auto rgb = led["rgb"].toArray();
                if (rgb.size() == 3) {
                    sr += rgb[0].toInt();
                    sg += rgb[1].toInt();
                    sb += rgb[2].toInt();
                    ++n;
                }
            }
            add("segment_" + it.key(), "Device RGB lighting · " + title, model + " / " + title,
                n ? QJsonArray{sr / n, sg / n, sb / n} : QJsonArray{}, "rgb");
            auto row = rows.last().toMap();
            row["categoryKey"] = prefix + "/" + it.key();
            row["categoryName"] = title;
            row["parentCategory"] = prefix;
            row["icueLeds"] = subset.toVariantList();
            rows.last() = row;
        }
        QList<QJsonObject> ordered;
        for (auto value : leds)
            ordered.append(value.toObject());
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto &a, const auto &b) { return a["id"].toDouble() < b["id"].toDouble(); });
        int ordinal = 0;
        for (const auto &led : ordered) {
            const auto number = quint32(led["id"].toDouble());
            ++ordinal;
            const auto label =
                led["name"].toString().isEmpty() ? "LED " + QString::number(ordinal) : led["name"].toString();
            add("led_" + QString::number(number), label + " · " + deviceName, model + " / Individual LEDs",
                led["rgb"].toArray(), "rgb", qulonglong(number));
            const auto segment = led["segment"].toString();
            if (!segment.isEmpty()) {
                auto row = rows.last().toMap();
                const auto title = led["segment_name"].toString();
                row["categoryKey"] = prefix + "/" + segment + "/leds";
                row["parentCategory"] = prefix + "/" + segment;
                row["group"] = model + " / " + title + " / Individual LEDs";
                row["name"] =
                    "LED " + QString::number(led["segment_led"].toInt()) + " · " + title + " · " + deviceName;
                row["tooltip"] = row["name"].toString() + "\nSDK LED ID: " + QString::number(number);
                rows.last() = row;
            } else if (device["channels"].toInt() > 0) {
                auto row = rows.last().toMap();
                row["categoryName"] = "Unassigned LEDs";
                row["group"] = model + " / Unassigned LEDs";
                row["tooltip"] =
                    row["tooltip"].toString() + "\nSDK did not provide a verified connected-device mapping.";
                rows.last() = row;
            }
        }
    }
    return rows;
}

QVariantMap Controller::icueLightingRow(const QString &id) const {
    for (const auto &value : icueBrowserRows(sensors())) {
        const auto row = value.toMap();
        if (row["id"] == id && row["control"] == "rgb")
            return row;
    }
    return {};
}

void Controller::localIcueCommand(const QString &action, const QVariantMap &arguments,
                                  const QString &requestId) {
    const auto args = QJsonObject::fromVariantMap(arguments);
    auto error = action == "icue_lighting"     ? IcueAutomation::validateLighting(args, icueAutomationState())
                 : action == "icue_automation" ? IcueAutomation::validate(args, icueAutomationState())
                                               : IcueSdk::validate(action, args);
    if (error.isEmpty() &&
        !((action == "icue_automation" || action == "icue_lighting") ? icueAutomationEnabled()
                                                                     : icueDirectEnabled()))
        error = "Enable the corresponding iCUE submodule first";
    if (action != "icue_automation" && action != "icue_lighting" && error.isEmpty() &&
        (!icueData["connected"].toBool() || !icueClock.isValid() || icueClock.elapsed() > 15000))
        error = "iCUE is not connected";
    if (!error.isEmpty()) {
        emit localIcueFinished(false, error);
        emit localIcueReply(requestId, false, error);
        return;
    }
    // Coalesce edits to the same target; preserve ordering relative to other LEDs.
    auto key = args;
    key.remove("rgb");
    key.remove("value");
    key["action"] = action;
    if (action == "icue_lighting" && args["parameters"].isObject()) {
        QJsonObject targets;
        for (auto k : args["parameters"].toObject().keys())
            targets[k] = true;
        key["parameters"] = targets;
    }
    const auto identity = QString::fromUtf8(QJsonDocument(key).toJson(QJsonDocument::Compact));
    for (int i = localIcueQueue.size() - 1; i >= 0; --i)
        if (localIcueQueue[i]["key"] == identity)
            localIcueQueue.removeAt(i);
    if (localIcueQueue.size() >= 32) {
        emit localIcueFinished(false, "Too many pending iCUE edits");
        emit localIcueReply(requestId, false, "Too many pending iCUE edits");
        return;
    }
    localIcueQueue.append(
        QJsonObject{{"action", action}, {"args", args}, {"key", identity}, {"requestId", requestId}});
    runLocalIcueCommand();
}

void Controller::runLocalIcueCommand() {
    if (!moduleEnabled("icue")) {
        localIcueQueue.clear();
        return;
    }
    bool seen[2] = {false, false};
    int next = -1;
    for (int i = 0; i < localIcueQueue.size(); ++i) {
        const auto action = localIcueQueue[i]["action"].toString();
        const int lane = automationAction(action) ? 1 : 0;
        if (seen[lane])
            continue;
        seen[lane] = true;
        if (!icueCommandBusy(action, localIcueQueue[i]["args"].toObject())) {
            next = i;
            break;
        }
    }
    if (next < 0)
        return;
    const auto request = localIcueQueue.takeAt(next);
    auto epoch = localIcueEpoch;
    const auto generation = epoch->load();
    submitIcueCommand(
        request["action"].toString(), request["args"].toObject(),
        [epoch, generation] { return epoch->load() != generation; },
        [this, request](QString error) {
            log(error.isEmpty() ? "Local iCUE edit completed" : "Local iCUE edit failed: " + error);
            emit localIcueFinished(error.isEmpty(), error.isEmpty() ? "Updated" : error);
            emit localIcueReply(request["requestId"].toString(), error.isEmpty(),
                                error.isEmpty() ? "Updated" : error);
            if (!testMode)
                pollIcue();
            else
                runLocalIcueCommand();
        });
    runLocalIcueCommand(); // The other independent lane may also be ready.
}

void Controller::handleIcueCommand(const QString &session, const QJsonObject &command) {
    const auto id = command["id"].toString(), action = command["action"].toString();
    const auto args = command["args"].toObject();
    QString error = action == "icue_lighting" ? IcueAutomation::validateLighting(args, icueAutomationState())
                    : action == "icue_automation" ? IcueAutomation::validate(args, icueAutomationState())
                                                  : IcueSdk::validate(action, args);
    if (error.isEmpty() &&
        (!((action == "icue_automation" || action == "icue_lighting") ? icueAutomationEnabled()
                                                                      : icueDirectEnabled()) ||
         !settings
              .value((action == "icue_automation" || action == "icue_lighting")
                         ? "modules/icue/automationControl"
                         : "modules/icue/control",
                     false)
              .toBool()))
        error = "Enable iCUE module and its remote-control permission on the PC";
    if (error.isEmpty() && icueCommandBusy(action, args))
        error = "iCUE is busy; try again shortly";
    if (!error.isEmpty()) {
        log("iCUE command rejected: " + error);
        server.replyCommand(session, id, false, error);
        return;
    }
    auto lease = server.commandLease(session);
    auto epoch = commandEpoch;
    const auto generation = epoch->load();
    if (!lease || !lease->load())
        return;
    submitIcueCommand(
        action, args, [lease, epoch, generation] { return !lease->load() || epoch->load() != generation; },
        [this, session, id, action](QString message) {
            server.replyCommand(session, id, message.isEmpty(), message.isEmpty() ? "Completed" : message);
            log("iCUE " + action + (message.isEmpty() ? " completed" : " failed: " + message));
            pollIcue();
        });
}
