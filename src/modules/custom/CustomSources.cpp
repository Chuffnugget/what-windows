#include "CustomSources.h"
#include "modules/windows/commands/ShellCommand.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <windows.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cmath>

namespace {
bool text(const QJsonValue &value, int maximum, bool empty = false) {
    return value.isString() && (empty || !value.toString().trimmed().isEmpty()) &&
           value.toString().size() <= maximum && !value.toString().contains(QChar::Null);
}

bool identifier(const QJsonValue &value) {
    static const QRegularExpression pattern("^[a-f0-9]{32}$");
    return value.isString() && pattern.match(value.toString()).hasMatch();
}

QString quotePowerShell(QString value) {
    return "'" + value.replace("'", "''") + "'";
}

QJsonValue field(QJsonValue value, const QString &path) {
    if (path.isEmpty())
        return value;
    for (const auto &part : path.split('.')) {
        if (value.isObject())
            value = value.toObject().value(part);
        else if (value.isArray()) {
            bool ok = false;
            const auto index = part.toInt(&ok);
            value =
                ok && index >= 0 && index < value.toArray().size() ? value.toArray().at(index) : QJsonValue();
        } else
            return {};
    }
    return value;
}

QJsonObject failure(const QString &message) {
    return {{"ok", false}, {"error", message}, {"value", QJsonValue()}};
}

QJsonValue collect(const QJsonObject &d, const std::function<bool()> &cancelled, QString &error,
                   QJsonObject &execution) {
    const auto source = d["source"].toString(), target = d["target"].toString();
    if (source == "process") {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            error = "Windows could not enumerate processes";
            return {};
        }
        PROCESSENTRY32W process{};
        process.dwSize = sizeof(process);
        bool found = false;
        BOOL next = Process32FirstW(snapshot, &process);
        for (; next; next = Process32NextW(snapshot, &process)) {
            if (cancelled())
                break;
            if (QString::fromWCharArray(process.szExeFile)
                    .compare(QFileInfo(target).fileName(), Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        }
        if (!next && GetLastError() != ERROR_NO_MORE_FILES)
            error = "Windows could not enumerate processes";
        CloseHandle(snapshot);
        return found;
    }
    if (source == "service") {
        const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        const auto service =
            manager ? OpenServiceW(manager, reinterpret_cast<LPCWSTR>(target.utf16()), SERVICE_QUERY_STATUS)
                    : nullptr;
        SERVICE_STATUS_PROCESS state{};
        DWORD bytes = 0;
        const bool ok =
            service && QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&state),
                                            sizeof(state), &bytes);
        if (service)
            CloseServiceHandle(service);
        if (manager)
            CloseServiceHandle(manager);
        if (!ok) {
            error = "Service not found or access denied";
            return {};
        }
        switch (state.dwCurrentState) {
        case SERVICE_RUNNING:
            return "Running";
        case SERVICE_STOPPED:
            return "Stopped";
        case SERVICE_PAUSED:
            return "Paused";
        case SERVICE_START_PENDING:
            return "Starting";
        case SERVICE_STOP_PENDING:
            return "Stopping";
        default:
            return "Changing";
        }
    }
    if (source == "file") {
        const QFileInfo info(target);
        const auto property = d["property"].toString("exists");
        if (property == "exists")
            return info.exists();
        if (!info.exists()) {
            error = "File is unavailable";
            return {};
        }
        return property == "size" ? QJsonValue(double(info.size()))
                                  : QJsonValue(info.lastModified().toUTC().toString(Qt::ISODate));
    }
    if (source == "counter") {
        PDH_HQUERY query = nullptr;
        PDH_HCOUNTER counter = nullptr;
        auto status = PdhOpenQueryW(nullptr, 0, &query);
        if (status == ERROR_SUCCESS)
            status = PdhAddEnglishCounterW(query, reinterpret_cast<LPCWSTR>(target.utf16()), 0, &counter);
        if (status == ERROR_SUCCESS)
            status = PdhCollectQueryData(query);
        // Rate counters need two samples. Wait on the collector thread, never QML.
        for (int i = 0; status == ERROR_SUCCESS && i < 20 && !cancelled(); ++i)
            QThread::msleep(50);
        if (status == ERROR_SUCCESS && !cancelled())
            status = PdhCollectQueryData(query);
        PDH_FMT_COUNTERVALUE value{};
        if (status == ERROR_SUCCESS)
            status = PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value);
        if (query)
            PdhCloseQuery(query);
        if (status != ERROR_SUCCESS ||
            (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) ||
            !std::isfinite(value.doubleValue)) {
            error = "Counter is unavailable; check its path and instance";
            return {};
        }
        return value.doubleValue;
    }

    QString command = d["command"].toString();
    if (source == "wmi")
        command = "$ErrorActionPreference='Stop'; Get-CimInstance -Namespace " +
                  quotePowerShell(d["namespace"].toString("root/cimv2")) + " -Query " +
                  quotePowerShell(d["query"].toString()) + " | Select-Object -ExpandProperty " +
                  quotePowerShell(d["property"].toString()) + " | ConvertTo-Json -Compress -Depth 6";
    const auto result =
        ShellCommand::run({{"shell", source == "wmi" ? "powershell" : d["shell"].toString("powershell")},
                           {"command", command},
                           {"timeout", d["timeout"].toInt(10)},
                           {"leave_running", d["mode"] == "action" && d["leave_running"].toBool()}},
                          cancelled);
    // Kept in the local preview/status only, never in published sensor metadata.
    execution = result;
    if (result.contains("error") || result["timed_out"].toBool() || result["cancelled"].toBool() ||
        result["truncated"].toBool() || result["exit_code"].toInt(-1) != 0) {
        error = result["error"].toString();
        if (error.isEmpty())
            error = result["timed_out"].toBool()   ? "Execution timed out"
                    : result["cancelled"].toBool() ? "Execution cancelled"
                    : result["truncated"].toBool()
                        ? "Output exceeded 64 KiB"
                        : "Command failed: " + result["stderr"].toString().trimmed().left(512);
        return {};
    }
    if (d["mode"] == "action")
        return result["stdout"].toString().left(4096);
    const auto output = result["stdout"].toString().trimmed();
    if (d["format"] == "text" && source != "wmi")
        return output;
    // Wrapping permits JSON scalar values as well as objects, with no string guessing.
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(("[" + output + "]").toUtf8(), &parse);
    if (parse.error == QJsonParseError::NoError && json.isArray() && json.array().size() == 1)
        return json.array().first();
    error = "Expected one JSON value on standard output";
    return {};
}
} // namespace

QString CustomSources::validate(const QJsonObject &d) {
    if ((d.contains("description") && !text(d["description"], 500, true)) ||
        (d.contains("ha_enabled") && !d["ha_enabled"].isBool()) ||
        (d.contains("cloud_enabled") && !d["cloud_enabled"].isBool()))
        return "Check the description and Home Assistant permissions";
    if (!identifier(d["id"]) || !text(d["name"], 100) || !d["enabled"].isBool() ||
        !QStringList{"sensor", "action"}.contains(d["mode"].toString()))
        return "Enter a name and choose Sensor or Action";
    const auto source = d["source"].toString();
    if (!QStringList{"process", "service", "file", "counter", "wmi", "script"}.contains(source))
        return "Choose a supported source";
    if (d["mode"] == "action" && source != "script")
        return "Saved actions use a shell command";
    if (d["interval"].toInt() < 1 || d["interval"].toInt() > 86400 || d["timeout"].toInt() < 1 ||
        d["timeout"].toInt() > 60)
        return "Interval must be 1-86400 seconds and timeout 1-60 seconds";
    if (d.contains("leave_running") && (!d["leave_running"].isBool() ||
        (d["leave_running"].toBool() && d["mode"] != "action")))
        return "Only saved commands may leave applications running";
    if (source == "script") {
        const auto error = ShellCommand::validate(
            {{"shell", d["shell"]}, {"command", d["command"]}, {"timeout", d["timeout"]},
             {"leave_running", d["leave_running"].toBool()}});
        if (!error.isEmpty())
            return error;
    } else if (source == "wmi") {
        if (!text(d["namespace"], 128) || !text(d["query"], 1024) || !text(d["property"], 128) ||
            !d["query"].toString().trimmed().startsWith("SELECT ", Qt::CaseInsensitive))
            return "Enter a WMI namespace, SELECT query and result property";
    } else if (!text(d["target"], 1024))
        return "Choose a process, service, file or counter path";
    if (source == "file" && !QStringList{"exists", "size", "modified"}.contains(d["property"].toString()))
        return "Choose a file property";
    if (!QStringList{"text", "json"}.contains(d["format"].toString()))
        return "Choose Text or JSON output";
    if (d["mode"] == "action")
        return {};
    const auto outputs = d["outputs"].toArray();
    if (outputs.isEmpty() || outputs.size() > 32)
        return "Add between 1 and 32 output readings";
    QSet<QString> ids;
    for (auto value : outputs) {
        const auto output = value.toObject();
        if (!identifier(output["id"]) || ids.contains(output["id"].toString()) ||
            !text(output["name"], 100) || !text(output["path"], 256, true) ||
            !text(output["unit"], 32, true) ||
            !QStringList{"number", "binary", "text", "timestamp"}.contains(output["kind"].toString()) ||
            !QStringList{"", "measurement", "total", "total_increasing"}.contains(
                output["state_class"].toString()))
            return "Check output names, types, units and unique IDs";
        if (output["kind"] != "number" &&
            (!output["unit"].toString().isEmpty() || !output["state_class"].toString().isEmpty()))
            return "Only numeric outputs can have units or statistics";
        ids.insert(output["id"].toString());
    }
    return {};
}

QJsonObject CustomSources::run(const QJsonObject &d, const std::function<bool()> &cancelled) try {
    const auto invalid = validate(d);
    if (!invalid.isEmpty())
        return failure(invalid);
    QElapsedTimer clock;
    clock.start();
    QString error;
    QJsonObject execution;
    auto value = cancelled() ? QJsonValue() : collect(d, cancelled, error, execution);
    if (cancelled())
        error = "Execution cancelled";
    QJsonObject result{{"ok", error.isEmpty()},
                       {"error", error},
                       {"value", value},
                       {"duration_ms", double(clock.elapsed())},
                       {"attempt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (!execution.isEmpty())
        result["execution"] = execution;
    if (error.isEmpty() && d["mode"] == "sensor") {
        for (auto row : readings(d, result))
            if (!row.toObject()["available"].toBool()) {
                result["ok"] = false;
                result["error"] = "Output '" + row.toObject()["name"].toString() +
                                  "' is missing or does not match its configured type";
                break;
            }
    }
    return result;
} catch (...) {

    return failure("Source execution failed unexpectedly");
}

QJsonArray CustomSources::readings(const QJsonObject &d, const QJsonObject &result) {
    QJsonArray rows;
    if (d["mode"] != "sensor")
        return rows;
    for (auto entry : d["outputs"].toArray()) {
        const auto output = entry.toObject();
        const auto kind = output["kind"].toString();
        auto value = field(result["value"], output["path"].toString());
        bool valid = false;
        if (kind == "number") {
            bool ok = value.isDouble();
            const auto number = value.isString() ? value.toString().toDouble(&ok) : value.toDouble();
            valid = ok && std::isfinite(number);
            value = number;
        } else if (kind == "binary") {
            if (value.isString() && QStringList{"true", "false"}.contains(value.toString().toLower()))
                value = value.toString().compare("true", Qt::CaseInsensitive) == 0;
            valid = value.isBool();
        } else if (kind == "timestamp") {
            const auto date = QDateTime::fromString(value.toString(), Qt::ISODate);
            valid = date.isValid() && date.timeSpec() != Qt::LocalTime;
            value = date.toUTC().toString(Qt::ISODate);
        } else
            valid = value.isString() && value.toString().size() <= 255;
        const bool available = d["enabled"].toBool() && result["ok"].toBool() && valid;
        rows.append(QJsonObject{{"id", "custom_" + output["id"].toString()},
                                {"source", "custom"},
                                {"group", d["name"]},
                                {"name", output["name"]},
                                {"kind", kind},
                                {"type", 0},
                                {"unit", output["unit"]},
                                {"state_class", output["state_class"]},
                                {"value", available ? value : QJsonValue()},
                                {"available", available}});
    }
    return rows;
}
