#include "app/Controller.h"
#include "app/Architecture.h"
#include <QFileDialog>
#include <QRegularExpression>

QVariantMap Controller::beacnState() const {
    auto s = beacnData;
    s["enabled"] = moduleEnabled("beacn");
    s["control_enabled"] = settings.value("modules/beacn/control", false).toBool();
    s["cloud"] = settings.value("modules/beacn/cloud", false).toBool();
    s["busy"] = beacnModule.busy();
    s["exe_override"] = settings.value("modules/beacn/exe").toString();
    s["dll_override"] = settings.value("modules/beacn/dll").toString();
    return s;
}

QJsonObject Controller::beacnShared() const {
    auto s = beacnState();
    if (!moduleEnabled("beacn") || !s.value("control_enabled").toBool()) {
        s.remove("controls");
        s.remove("command_status");
        s.remove("last_command");
    }
    for (auto key : {"exe_path", "dll_path", "exe_override", "dll_override", "log_path", "recording_status",
                     "installation", "channels", "global"})
        s.remove(key);
    for (auto key : {"status", "command_status", "last_command"}) {
        auto text = s.value(key).toString();
        for (auto pathKey : {"exe_path", "dll_path", "log_path"}) {
            const auto path = beacnData.value(pathKey).toString();
            if (!path.isEmpty())
                text.replace(path, "[local path]", Qt::CaseInsensitive);
        }
        if (s.contains(key))
            s[key] = text;
    }
    return QJsonObject::fromVariantMap(s);
}

void Controller::initBeacn() {
    connect(&beacnModule, &BeacnModule::snapshot, this, &Controller::acceptBeacn);
    configureBeacn();
}

void Controller::configureBeacn() {
    beacnData = {{"connected", false},
                 {"ready", false},
                 {"discovering", moduleEnabled("beacn")},
                 {"status", moduleEnabled("beacn") ? "Discovering BEACN…" : "Disabled"}};
    mergeReadings("beacn", {});
    emit sensorsChanged();
    beacnModule.configure(moduleEnabled("beacn") && !testMode, settings.value("modules/beacn/exe").toString(),
                          settings.value("modules/beacn/dll").toString());
}

void Controller::saveBeacn(bool enabled, bool control, bool cloud) {
    if (!Architecture::moduleAvailable("beacn"))
        return;
    const bool wasEnabled = moduleEnabled("beacn");
    ++*commandEpoch;
    settings.setValue("modules/beacn/enabled", enabled);
    settings.setValue("modules/beacn/control", control);
    settings.setValue("modules/beacn/cloud", cloud);
    scheduleSettingsFlush();
    if (wasEnabled != enabled)
        configureBeacn();
    publishPolicy();
    emit changed();
    emit sensorsChanged();
}

void Controller::browseBeacn(bool executable) {
    const auto path = QFileDialog::getOpenFileName(
        nullptr, executable ? "Select installed BEACN app" : "Select installed BEACN driver", {},
        executable ? "BEACN (BEACN.exe)" : "BEACN driver (BEACNUsbAudioapi_x64.dll)");
    if (path.isEmpty())
        return;
    settings.setValue(executable ? "modules/beacn/exe" : "modules/beacn/dll", path);
    scheduleSettingsFlush();
    reconnectBeacn();
}

void Controller::reconnectBeacn() {
    ++*commandEpoch;
    configureBeacn();
    publishPolicy();
    emit changed();
}

void Controller::beacnCommand(const QVariantMap &args) {
    handleBeacnCommand({}, {{"args", QJsonObject::fromVariantMap(args)}});
}

void Controller::handleBeacnCommand(const QString &session, const QJsonObject &command) {
    const bool local = session.isEmpty();
    const auto id = command["id"].toString();
    const auto requestedControl = command["args"].toObject()["control"].toString();
    // Only fixed protocol identifiers belong in the shareable summary, never values or labels.
    static const QRegularExpression safeControl(
        "^(?:(?:volume|mute|audience)_[0-9]{1,3}|refresh_choices|submix|previous_page|next_page|active_device|personal_device|audience_device|theme|screen_brightness|button_brightness|dim_timeout)$");
    const auto control =
        safeControl.match(requestedControl).hasMatch() ? requestedControl : QString("unknown control");
    auto reply = [this, session, id, local, control](bool ok, QString message) {
        beacnData["last_command"] = message;
        if (!ok)
            log(QString("ERROR BEACN [%1] %2: failed; see BEACN diagnostics for details")
                    .arg(local ? "WHAT" : "Home Assistant", control));
        log("BEACN: " + message);
        if (!local)
            server.replyCommand(session, id, ok, message);
        emit changed();
        publish();
    };
    if (!moduleEnabled("beacn") || (!local && !settings.value("modules/beacn/control", false).toBool())) {
        reply(false, "Enable BEACN and its Home Assistant control permission first");
        return;
    }
    if (testMode) {
        reply(false, "BEACN commands are disabled in test mode");
        return;
    }
    auto lease = local ? std::shared_ptr<std::atomic_bool>{} : server.commandLease(session);
    if (!local && (!lease || !lease->load())) {
        reply(false, "Session expired");
        return;
    }
    beacnModule.command(command["args"].toObject(), lease, commandEpoch, commandEpoch->load(), reply);
    emit changed();
}

void Controller::saveBeacnPaths(const QString &exe, const QString &dll) {
    settings.setValue("modules/beacn/exe", exe.trimmed());
    settings.setValue("modules/beacn/dll", dll.trimmed());
    scheduleSettingsFlush();
    reconnectBeacn();
}

void Controller::acceptBeacn(const QVariantMap &state) {
    if (state.value("status") != beacnData.value("status"))
        log("BEACN: " + state.value("status").toString());
    const auto installation = state.value("installation").toMap();
    if (!installation.isEmpty() && installation != beacnData.value("installation").toMap()) {
        log(QString(
                "BEACN verification: app %1; driver %2; native compatible=%3; driver supported=%4; bridge present=%5; known app build=%6")
                .arg(installation.value("appVersion").toString(),
                     installation.value("driverVersion").toString())
                .arg(installation.value("appValid").toBool())
                .arg(installation.value("driverValid").toBool())
                .arg(installation.value("helperPresent").toBool())
                .arg(installation.value("appVerified").toBool()));
    }
    if (!moduleEnabled("beacn"))
        return;
    const auto last = beacnData.value("last_command");
    beacnData = state;
    if (last.isValid())
        beacnData["last_command"] = last;
    const bool ready = state.value("ready").toBool();
    QJsonArray rows;
    auto add = [&](QString id, QString group, QString name, QVariant value, QString kind, QString unit,
                   bool valid) {
        if (kind == "text")
            value = value.toString().left(255);
        rows.append(QJsonObject{{"id", "beacn_" + id},
                                {"source", "beacn"},
                                {"group", group},
                                {"name", name},
                                {"kind", kind},
                                {"unit", unit},
                                {"type", 0},
                                {"state_class", QString()},
                                {"recommended", false},
                                {"available", valid && value.isValid()},
                                {"value", valid && value.isValid() ? QJsonValue::fromVariant(value)
                                                                   : QJsonValue(QJsonValue::Null)}});
    };
    add("ready", "Connection", "BEACN connected", ready, "binary", "", true);
    add("sample_rate", "Connection", "Sample rate", state.value("sample_rate"), "number", "Hz",
        state.value("connected").toBool());
    for (auto v : state.value("channels").toList()) {
        const auto c = v.toMap();
        const auto id = QString::number(c.value("id").toInt());
        const auto group = "Channels / " + c.value("name").toString();
        const bool valid = ready && c.value("valid").toBool();
        for (auto mix : {"personal", "audience"}) {
            const QString key = mix;
            const auto title = key == "personal" ? "Personal" : "Audience";
            add(id + "_" + key + "_volume", group, QString(title) + " volume", c.value(key + "Percent"),
                "number", "%", valid);
            add(id + "_" + key + "_muted", group, QString(title) + " muted", c.value(key + "Muted"), "binary",
                "", valid);
        }
        add(id + "_linked", group, "Personal / Audience linked", c.value("linked"), "binary", "", valid);
    }
    const auto g = state.value("global").toMap();
    add("page", "App state", "Current knob page", g.value("page"), "number", "", ready);
    add("pages", "App state", "Knob pages", g.value("pages"), "number", "", ready);
    add("dimmed", "App state", "Screen dimmed", g.value("dimmed"), "binary", "",
        ready && g.value("lightingValid").toBool());
    const auto defaults = g.value("windowsDefaults").toStringList();
    const QStringList labels{"Recording default", "Recording communications", "Playback default",
                             "Playback communications"};
    for (int i = 0; i < 4; ++i)
        add("observed_default_" + QString::number(i), "Observed Windows defaults", labels[i],
            defaults.value(i), "text", "", ready && i < defaults.size() && !defaults[i].isEmpty());
    mergeReadings("beacn", rows);
    publish();
    emit changed();
    emit sensorsChanged();
}
