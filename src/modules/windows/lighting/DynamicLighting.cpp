#include "DynamicLighting.h"
#include <windows.h>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QFileInfo>
#include <QUuid>
#include <cmath>

DynamicLighting::DynamicLighting(QObject *parent) : QObject(parent) {
    poll.setInterval(5000);
    deadline.setSingleShot(true);
    deadline.setInterval(15000);
    connect(&poll, &QTimer::timeout, this, &DynamicLighting::refresh);
    connect(&deadline, &QTimer::timeout, this, [this] {
        const auto pid = process ? process->property("diagnosticPid").toLongLong() : 0;
        stop();
        data = {{"devices", QJsonArray{}},
                {"status",
                 QString("Windows lighting timed out (PID %1). Disable and re-enable the submodule to retry.")
                     .arg(pid)}};
        emit changed();
    });
}

DynamicLighting::~DynamicLighting() {
    stop(true);
}

void DynamicLighting::stop(bool shuttingDown) {
    poll.stop();
    deadline.stop();
    if (process) {
        auto *old = process;
        process = nullptr;
        old->disconnect(this);
        old->closeWriteChannel();
        if (shuttingDown) {
            // The application event loop has stopped, so a timer cannot finish cleanup.
            // EOF lets the helper release its lighting handles before the bounded kill.
            if (old->state() != QProcess::NotRunning && !old->waitForFinished(500)) {
                old->kill();
                old->waitForFinished(500);
            }
            delete old;
        } else {
            // Disabling the module while WHAT is open must not block the GUI.
            old->setParent(QCoreApplication::instance());
            connect(old, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), old,
                    &QObject::deleteLater);
            QTimer::singleShot(1000, old, [old] {
                if (old->state() != QProcess::NotRunning)
                    old->kill();
                else
                    old->deleteLater();
            });
        }
    }
    finish(false, "Lighting request cancelled.");
    buffer.clear();
}

void DynamicLighting::setEnabled(bool value, bool testing) {
    enabled = value;
    stop();
    data = {{"devices", QJsonArray{}}, {"status", value ? "Starting Windows Dynamic Lighting…" : "Disabled"}};
    emit changed();
    if (!value || testing)
        return;
    const auto helper = QCoreApplication::applicationDirPath() + "/helpers/WHAT.WindowsHelper.exe";
    if (!QFileInfo::exists(helper)) {
        data["status"] = "Windows helper missing. Rebuild or reinstall WHAT.";
        emit changed();
        return;
    }
    process = new QProcess(this);
    process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    connect(process, &QProcess::started, this, [this] {
        process->setProperty("diagnosticPid", process->processId());
        poll.start();
        refresh();
    });
    connect(process, &QProcess::readyReadStandardError, this, [this] { process->readAllStandardError(); });
    connect(process, &QProcess::readyReadStandardOutput, this, [this] {
        buffer += process->readAllStandardOutput();
        if (buffer.size() > 262144) {
            stop();
            data["devices"] = QJsonArray{};
            data["status"] = "Lighting helper response exceeded its limit.";
            emit changed();
            return;
        }
        while (buffer.contains('\n')) {
            const int end = buffer.indexOf('\n');
            const auto reply = QJsonDocument::fromJson(buffer.left(end)).object();
            buffer.remove(0, end + 1);
            if (pending.isEmpty() || reply["request"].toString() != pending)
                continue;
            const auto previous = data;
            if (reply["ok"].toBool() && reply["devices"].isArray())
                data["devices"] = reply["devices"];
            else if (!completion)
                data["devices"] = QJsonArray{};
            data["status"] = reply["status"].toString().left(512);
            finish(reply["ok"].toBool(), data["status"].toString());
            if (data != previous)
                emit changed();
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        const auto error = QString("%1 (PID %2)")
                               .arg(process->errorString())
                               .arg(process->property("diagnosticPid").toLongLong());
        stop();
        data = {{"devices", QJsonArray{}}, {"status", "Lighting helper failed: " + error}};
        emit changed();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                const auto pid = process->property("diagnosticPid").toLongLong();
                stop();
                data = {{"devices", QJsonArray{}},
                        {"status",
                         QString("Lighting helper stopped (PID %1, exit %2). Disable and re-enable to retry.")
                             .arg(pid)
                             .arg(code)}};
                emit changed();
            });
    process->start(helper, {"--lighting-server"});
}

void DynamicLighting::finish(bool ok, const QString &message) {
    deadline.stop();
    pending.clear();
    auto callback = std::move(completion);
    completion = {};
    if (callback)
        callback(ok, message);
}

void DynamicLighting::send(QJsonObject request, std::function<void(bool, QString)> done) {
    if (!enabled || !process || process->state() != QProcess::Running || !pending.isEmpty()) {
        if (done)
            done(false, "Lighting is unavailable or busy. Try again.");
        return;
    }
    pending = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request["request"] = pending;
    completion = std::move(done);
    const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (process->write(bytes) != bytes.size()) {
        finish(false, "Could not send lighting command.");
        return;
    }
    deadline.start();
}

void DynamicLighting::refresh() {
    send({{"operation", "scan"}});
}

QString DynamicLighting::validate(const QJsonObject &command) {
    const auto id = command["device"].toString();
    if (id.isEmpty() || id.size() > 2048 || id.contains(QChar::Null))
        return "Invalid lighting device.";
    if (command["operation"] != "set")
        return "Unsupported lighting operation.";
    if (!command["brightness"].isDouble() || !std::isfinite(command["brightness"].toDouble()) ||
        command["brightness"].toDouble() < 0 || command["brightness"].toDouble() > 100)
        return "Brightness must be between 0 and 100.";
    const auto rgb = command["rgb"].toArray();
    if (rgb.size() != 3)
        return "Choose three RGB channels.";
    for (const auto &channel : rgb) {
        const auto value = channel.toDouble(-1);
        if (!channel.isDouble() || !std::isfinite(value) || value < 0 || value > 255 ||
            std::floor(value) != value)
            return "RGB channels must be whole numbers from 0 to 255.";
    }
    return {};
}

void DynamicLighting::apply(QJsonObject command, std::function<void(bool, QString)> done) {
    const auto error = validate(command);
    if (!error.isEmpty()) {
        done(false, error);
        return;
    }
    send(command, std::move(done));
}
