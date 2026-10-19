#include "BeacnModule.h"
#include "BeacnBackend.h"
#include "InstalledFiles.h"
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QFileInfo>
#include <QCoreApplication>
#include <cmath>
#include <utility>

#include <QMutex>
#include <QMutexLocker>

struct BeacnDelivery {
    QMutex mutex;
    BeacnModule *owner = nullptr;
};

// Everything a native call can touch remains owned by this worker, even if the
// GUI module has already been destroyed. No worker closure captures that module.
class BeacnWorker : public QObject {
public:
    BeacnBackend *backend = nullptr;
    QVariantMap installation;
    QString choiceIdentity;
    qint64 lastChoiceAttempt = 0;
    std::shared_ptr<std::atomic_uint64_t> revision;
    std::shared_ptr<BeacnDelivery> delivery;
    QVariantMap readState() const;
    void sendSnapshot(quint64 generation);
    void configure(quint64 generation, bool enabled, const QString &exe, const QString &dll);
    void command(const QJsonObject &args, std::shared_ptr<std::atomic_bool> lease,
                 std::shared_ptr<std::atomic_uint64_t> epoch, quint64 generation, quint64 config,
                 QElapsedTimer age, std::function<void(bool, QString)> done);

    template <class Fn> void deliver(Fn callback) {
        QMutexLocker lock(&delivery->mutex);
        if (auto *owner = delivery->owner)
            QMetaObject::invokeMethod(owner, [owner, callback] { callback(*owner); }, Qt::QueuedConnection);
    }
};

BeacnModule::BeacnModule(QObject *parent)
    : QObject(parent), thread(new QThread), worker(new BeacnWorker),
      delivery(std::make_shared<BeacnDelivery>()) {
    auto watchdog = new QTimer(this);
    watchdog->setInterval(1000);
    connect(watchdog, &QTimer::timeout, this, [this] {
        if (enabled && !stale && lastSnapshot.isValid() && lastSnapshot.elapsed() > 15000) {
            stale = true;
            emit snapshot({{"connected", false},
                           {"ready", false},
                           {"status", "No recent BEACN readback; controls unavailable"}});
        }
    });
    watchdog->start();
    delivery->owner = this;
    worker->delivery = delivery;
    worker->revision = revision;
    worker->moveToThread(thread);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->setObjectName("BEACN serialized worker");
    thread->start();
}

BeacnModule::~BeacnModule() {
    ++*revision;
    {
        QMutexLocker lock(&delivery->mutex);
        delivery->owner = nullptr;
    }
    thread->requestInterruption();
    thread->quit();
    // A stuck vendor call cannot hold the GUI's shutdown hostage. The worker and
    // its backend remain alive until the call returns and the thread finishes.
    thread->wait(250);
}

void BeacnModule::configure(bool enabled, const QString &exe, const QString &dll) {
    this->enabled = enabled;
    stale = false;
    lastSnapshot.start();
    const auto generation = ++*revision;
    QMetaObject::invokeMethod(
        worker,
        [state = worker, generation, enabled, exe, dll] { state->configure(generation, enabled, exe, dll); },
        Qt::QueuedConnection);
}

void BeacnWorker::configure(quint64 generation, bool enabled, const QString &exe, const QString &dll) {
    if (revision->load() != generation)
        return;
    delete backend;
    backend = nullptr;
    choiceIdentity.clear();
    lastChoiceAttempt = 0;
    if (!enabled)
        return;
    backend = new BeacnBackend(this);
    auto files = discoverInstalledFiles(exe, dll, true);
    // Explicit choices must be verified, never silently replaced with another install.
    if (!exe.isEmpty() || !dll.isEmpty())
        files = discoverInstalledFiles(exe.isEmpty() ? files.value("appPath").toString() : exe,
                                       dll.isEmpty() ? files.value("driverPath").toString() : dll, false);
    installation = files;
    installation["helperPresent"] =
        QFileInfo::exists(QCoreApplication::applicationDirPath() + "/beacn-volume-hook.dll");
    if (revision->load() != generation)
        return;
    backend->setExePath(files.value("appPath").toString());
    backend->setDllPath(files.value("driverPath").toString());
    // Driver readbacks do not depend on the app's private UI layout. App controls
    // keep their separate executable and live-object checks in the backend.
    if (files.value("driverValid").toBool())
        backend->connectDriver();
    auto timer = new QTimer(backend);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, [this, generation] { sendSnapshot(generation); });
    timer->start();
    sendSnapshot(generation);
}

QVariantMap BeacnWorker::readState() const {
    if (!backend)
        return {};
    const auto g = backend->globalState();
    const bool live = backend->connected() && g.value("valid").toBool() && !backend->commandUncertain() &&
                      installation.value("helperPresent").toBool();
    QString status = backend->status();
    if (backend->commandUncertain())
        status = "Command outcome unknown; check BEACN before reconnecting";
    else if (!installation.value("appValid").toBool() || !installation.value("driverValid").toBool()) {
        status = installation.value("status").toString();
        if (backend->connected())
            status += "\nDriver connected; app controls are unavailable.";
        else if (installation.value("driverValid").toBool())
            status += "\nDriver: " + backend->status();
    } else if (!installation.value("helperPresent").toBool())
        status = "WHAT's BEACN bridge DLL is missing. Reinstall WHAT or rebuild beacn-volume-hook.";
    else if (backend->connected() && !g.value("valid").toBool())
        status = "Driver connected. " + backend->globalStatus();
    QString compatibilityWarning;
    if (live && !installation.value("appVerified").toBool()) {
        compatibilityWarning = "Working on an unsupported BEACN build " +
                               installation.value("appVersion").toString() +
                               ". Native compatibility and live mixer checks passed.";
    }
    QStringList unavailableFeatures;
    if (live) {
        for (const auto &[key, label] : {
                 std::pair{"profilesValid", "profiles"},
                 std::pair{"assignmentsValid", "default-device assignments"},
                 std::pair{"autoResetValid", "automatic default-device reset"},
                 std::pair{"lightingValid", "screen and lighting settings"}}) {
            if (!g.value(key).toBool())
                unavailableFeatures.append(label);
        }
        if (!unavailableFeatures.isEmpty()) {
            if (!compatibilityWarning.isEmpty())
                compatibilityWarning += '\n';
            compatibilityWarning += "Live interface validation failed for: " +
                                    unavailableFeatures.join(", ") + ". These controls are unavailable.";
        }
    }
    if (!compatibilityWarning.isEmpty())
        status += "\n" + compatibilityWarning;
    QVariantList controls, channels;
    QStringList unsupported;
    for (int source : {5, 6})
        if (BeacnBackend::nativeSourceType(source) < 0)
            for (const auto *kind : {"volume_", "mute_", "audience_"})
                unsupported.append(QString::fromLatin1(kind) + QString::number(source));
    auto add = [&](QString id, QString name, QString kind, QVariant value, bool available, double low = 0,
                   double high = 100, QStringList options = QStringList{}) {
        controls.append(QVariantMap{{"id", id},
                                    {"name", name},
                                    {"kind", kind},
                                    {"value", value},
                                    {"available", live && available},
                                    {"min", low},
                                    {"max", high},
                                    {"step", 1},
                                    {"options", options}});
    };
    const auto states = backend->nativeStates();
    const auto names = backend->sourceNames();
    auto order = g.value("sourceOrder").toList();
    if (order.isEmpty())
        for (int i = 0; i < states.size(); ++i)
            order.append(i);
    for (auto index : order) {
        const int i = index.toInt();
        if (i < 0 || i >= states.size() || BeacnBackend::nativeSourceType(i) < 0)
            continue;
        auto s = states[i].toMap();
        s["id"] = i;
        s["name"] = names.value(i);
        channels.append(s);
        const auto suffix = QString::number(i);
        const bool valid = s.value("valid").toBool();
        add("volume_" + suffix, names.value(i) + " linked volume", "number", s.value("personalPercent"),
            valid && s.value("linked").toBool());
        add("mute_" + suffix, names.value(i) + " knob mute", "switch", s.value("knobMuted"), valid);
        add("audience_" + suffix, names.value(i) + " audience button", "switch", s.value("buttonMuted"),
            valid);
    }
    add("submix", "Submix", "switch", g.value("submix"), true);
    add("previous_page", "Previous knob page", "button", {}, g.value("page").toInt() > 1);
    add("next_page", "Next knob page", "button", {}, g.value("page").toInt() < g.value("pages").toInt());
    add("active_device", "Personal listening device", "select", g.value("activeA").toBool() ? "A" : "B", true,
        0, 100, {"A", "B"});
    const auto choices = backend->deviceChoices();
    const QStringList ids{"device_a",
                          "device_b",
                          "audience_device",
                          "recording_default",
                          "recording_communications",
                          "playback_default",
                          "playback_communications"};
    const QStringList labels{"Personal device A",
                             "Personal device B",
                             "Audience output",
                             "Recording default assignment",
                             "Recording communications assignment",
                             "Playback default assignment",
                             "Playback communications assignment"};
    const QStringList keys{"deviceA", "deviceB", "audienceDevice"};
    for (int i = 0; i < 7; ++i)
        add(ids[i], labels[i], "select",
            i < 3 ? g.value(keys[i]) : QVariant(g.value("assignments").toStringList().value(i - 3)),
            i < 3 || g.value("assignmentsValid").toBool(), 0, 100,
            i < choices.size() ? choices[i].toStringList() : QStringList{});
    add("profile", "Profile", "select", g.value("profile"), g.value("profilesValid").toBool(), 0, 100,
        g.value("profiles").toStringList());
    add("auto_reset", "Automatically reset default devices", "switch", g.value("autoResetDefaults"),
        g.value("autoResetValid").toBool());
    const bool lighting = g.value("lightingValid").toBool();
    const auto themes = g.value("themeNames").toStringList();
    add("theme", "Screen theme", "select", themes.value(g.value("themeId").toInt() - 1), lighting, 0, 100,
        themes);
    add("screen_brightness", "Screen brightness", "number", g.value("screenBrightness"), lighting, 2, 100);
    add("button_brightness", "Button brightness", "number", g.value("buttonBrightness"), lighting, 4, 40);
    add("dim_timeout", "Dim timeout (seconds)", "number", g.value("dimmerSeconds"), lighting, 1, 86400);
    add("refresh_choices", "Refresh device choices", "button", {}, true);
    return {{"installation", installation},
            {"connected", backend->connected()},
            {"ready", live},
            {"status", status},
            {"compatibility_warning", compatibilityWarning},
            {"command_status", backend->globalStatus()},
            {"reconnect_pending", backend->reconnectPending()},
            {"controls", controls},
            {"unsupported_controls", unsupported},
            {"channels", channels},
            {"global", g},
            {"exe_path", backend->exePath()},
            {"dll_path", backend->dllPath()},
            {"log_path", backend->logPath()},
            {"recording_status", backend->recordingStatus()},
            {"api_version", backend->apiVersion()},
            {"mixer_version", backend->mixerVersion()},
            {"sample_rate", backend->sampleRate()}};
}

void BeacnWorker::sendSnapshot(quint64 generation) {
    if (revision->load() != generation)
        return;
    if (backend) {
        const auto g = backend->globalState();
        const auto identity = g.value("valid").toBool() ? QString::number(backend->nativeProcessId()) + ":" +
                                                              g.value("profile").toString()
                                                        : QString{};
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (identity.isEmpty())
            choiceIdentity.clear();
        else if (identity != choiceIdentity && !backend->commandUncertain() &&
                 now - lastChoiceAttempt >= 3000) {
            lastChoiceAttempt = now;
            if (backend->globalControl(0))
                choiceIdentity = identity;
        }
    }
    const auto state = readState();
    deliver([generation, state](BeacnModule &owner) {
        if (owner.revision->load() == generation) {
            owner.lastSnapshot.restart();
            owner.stale = false;
            emit owner.snapshot(state);
        }
    });
}

QString BeacnModule::validate(const QJsonObject &args, const QVariantList &controls) {
    for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key() != "control" && it.key() != "value")
            return "Unknown BEACN command field";
    if (!args["control"].isString())
        return "Choose a BEACN control";
    for (auto value : controls) {
        const auto c = value.toMap();
        if (c["id"].toString() != args["control"].toString())
            continue;
        if (!c["available"].toBool())
            return "BEACN control is currently unavailable";
        const auto v = args["value"];
        const auto kind = c["kind"].toString();
        if (kind == "button")
            return args.contains("value") ? "Buttons do not accept a value" : QString{};
        if (kind == "switch")
            return v.isBool() ? QString{} : "Expected a boolean";
        if (kind == "select")
            return v.isString() && c["options"].toStringList().contains(v.toString())
                       ? QString{}
                       : "Selection is no longer available";
        if (kind == "number")
            return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= c["min"].toDouble() &&
                           v.toDouble() <= c["max"].toDouble() &&
                           (c["id"].toString().startsWith("volume_") ||
                            std::floor(v.toDouble()) == v.toDouble())
                       ? QString{}
                       : "Number is outside the supported range";
        return "Unsupported control";
    }
    return "Unknown BEACN control";
}

void BeacnModule::command(const QJsonObject &args, std::shared_ptr<std::atomic_bool> lease,
                          std::shared_ptr<std::atomic_uint64_t> epoch, quint64 generation,
                          std::function<void(bool, QString)> done) {
    if (pending) {
        done(false, "BEACN is busy; command was not queued");
        return;
    }
    pending = true;
    const auto config = revision->load();
    QElapsedTimer age;
    age.start();
    QMetaObject::invokeMethod(
        worker,
        [state = worker, args, lease, epoch, generation, config, age, done] {
            state->command(args, lease, epoch, generation, config, age, done);
        },
        Qt::QueuedConnection);
}

void BeacnWorker::command(const QJsonObject &args, std::shared_ptr<std::atomic_bool> lease,
                          std::shared_ptr<std::atomic_uint64_t> epoch, quint64 generation, quint64 config,
                          QElapsedTimer age, std::function<void(bool, QString)> done) {
    bool ok = false;
    QString error;
    auto cancelled = [&] {
        return revision->load() != config || epoch->load() != generation || (lease && !lease->load()) ||
               age.elapsed() > 5000;
    };
    if (cancelled() || !backend)
        error = "BEACN command cancelled before dispatch";
    else {
        backend->refreshNativeStates();
        error = BeacnModule::validate(args, readState().value("controls").toList());
        if (error.isEmpty() && cancelled())
            error = "BEACN command cancelled before dispatch";
        if (error.isEmpty()) {
            // Recheck at the final native dispatch too, after potentially slow
            // process discovery, so revoked permissions cannot launch queued work.
            auto revisionToken = revision;
            backend->setCommandGuard([revisionToken, config, lease, epoch, generation, age] {
                return revisionToken->load() == config && epoch->load() == generation &&
                       (!lease || lease->load()) && age.elapsed() <= 5000;
            });
            const auto id = args["control"].toString();
            const auto v = args["value"];
            if (id.startsWith("volume_"))
                ok = backend->setNativeSourcePercent(id.mid(7).toInt(), v.toDouble());
            else if (id.startsWith("mute_"))
                ok = backend->setNativeControl(id.mid(5).toInt(), v.toBool(), false);
            else if (id.startsWith("audience_") && id != "audience_device")
                ok = backend->setNativeControl(id.mid(9).toInt(), v.toBool(), true);
            else {
                const QMap<QString, int> ops{{"refresh_choices", 0},
                                             {"submix", 1},
                                             {"previous_page", 2},
                                             {"next_page", 2},
                                             {"active_device", 3},
                                             {"device_a", 4},
                                             {"device_b", 5},
                                             {"audience_device", 6},
                                             {"profile", 7},
                                             {"recording_default", 10},
                                             {"recording_communications", 11},
                                             {"playback_default", 12},
                                             {"playback_communications", 13},
                                             {"auto_reset", 14},
                                             {"theme", 20},
                                             {"screen_brightness", 30},
                                             {"button_brightness", 31},
                                             {"dim_timeout", 32}};
                int n = v.isBool() ? int(v.toBool()) : v.toInt();
                if (id == "previous_page")
                    n = -1;
                if (id == "next_page")
                    n = 1;
                if (id == "active_device")
                    n = v.toString() == "A" ? 1 : 0;
                if (id == "theme")
                    n = backend->globalState().value("themeNames").toStringList().indexOf(v.toString()) + 1;
                ok = backend->globalControl(ops.value(id), n, v.toString());
            }
            backend->setCommandGuard({});
            if (!ok)
                error = "BEACN refused or did not confirm the command. Check its app before retrying. " +
                        backend->globalStatus() + " " + backend->winUsbStatus();
        }
    }
    sendSnapshot(config);
    deliver([ok, error, done](BeacnModule &owner) {
        owner.pending = false;
        done(ok, ok ? "Sent once through BEACN; live state confirms the result" : error);
    });
}
