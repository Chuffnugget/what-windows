#include "modules/windows/commands/WindowsCommands.h"
#include "modules/windows/commands/ShellCommand.h"
#include "modules/windows/input/WindowsInput.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QProcess>
#include <QUrl>
#include <QSet>
#include <QStringList>
#include <cmath>
#include <windows.h>
#include <powrprof.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <shellapi.h>

namespace {
QString failure(const char *operation, DWORD code = GetLastError()) {
    return QString("%1 failed (Windows error %2)").arg(operation).arg(code);
}

bool number(const QJsonValue &value, double low, double high) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= low &&
           value.toDouble() <= high;
}

QString guidText(const GUID &id) {
    wchar_t out[40];
    StringFromGUID2(id, out, 40);
    return QString::fromWCharArray(out);
}

QString modeGuid(const QString &mode) {
    if (mode == "efficiency" || mode == "Best power efficiency")
        return "{961cc777-2547-4f9d-8174-7d86181b8a7a}";
    if (mode == "performance" || mode == "Best performance")
        return "{ded574b5-45a0-4f42-8737-46345c09c238}";
    return "{00000000-0000-0000-0000-000000000000}";
}
} // namespace

QString WindowsCommands::permission(const QString &action) {
    if (action == "keyboard" || action == "mouse")
        return "input";
    if (action == "run_command")
        return "shell";
    if (QStringList{"volume", "volume_up", "volume_down", "mute", "play", "pause", "stop", "next", "previous",
                    "seek", "shuffle", "repeat"}
            .contains(action))
        return "media";
    if (QStringList{"sleep", "hibernate", "reboot", "shutdown", "lock"}.contains(action))
        return "power";
    if (action == "power_plan" || action == "power_mode_ac" || action == "power_mode_dc")
        return "power_plan";
    if (action == "open_url")
        return "urls";
    if (action == "monitor_sleep")
        return "monitor";
    return {};
}

QString WindowsCommands::validate(const QString &action, const QJsonObject &args) {
    if (action == "keyboard" || action == "mouse")
        return WindowsInput::validate(action, args);
    if (action == "run_command")
        return ShellCommand::validate(args);
    if (permission(action).isEmpty())
        return "Unsupported command";
    const QSet<QString> valued{"volume",     "mute",          "seek",          "shuffle", "repeat",
                               "power_plan", "power_mode_ac", "power_mode_dc", "open_url"};
    if (args.size() != (valued.contains(action) ? 1 : 0) ||
        (valued.contains(action) && !args.contains("value")))
        return "Unexpected command arguments";
    const auto value = args["value"];
    if (action.startsWith("power_mode_") &&
        !QStringList{"efficiency", "balanced", "performance", "Best power efficiency", "Balanced", "Best performance"}.contains(value.toString()))
        return "Invalid power mode";
    if (action == "volume" && !number(value, 0, 1))
        return "Volume must be between 0 and 1";
    if (action == "seek" && !number(value, 0, 604800))
        return "Seek position must be between 0 and 604800 seconds";
    if ((action == "mute" || action == "shuffle") && !value.isBool())
        return "Expected a boolean";
    if (action == "repeat" && !QStringList{"off", "one", "all"}.contains(value.toString()))
        return "Invalid repeat mode";
    if (action == "power_plan") {
        GUID id;
        const auto s = value.toString();
        if (s.size() != 38 || FAILED(CLSIDFromString(reinterpret_cast<LPCOLESTR>(s.utf16()), &id)))
            return "Invalid power-plan identifier";
    }
    if (action == "open_url") {
        const auto s = value.toString();
        const QUrl url(s, QUrl::StrictMode);
        if (s.size() > 2048 || !url.isValid() || !QStringList{"https", "http"}.contains(url.scheme()) ||
            url.host().isEmpty() || !url.userInfo().isEmpty())
            return "Only HTTP/HTTPS URLs without embedded credentials are allowed";
        for (auto c : s)
            if (c.unicode() < 32 || c == QChar(127))
                return "Invalid URL characters";
    }
    return {};
}

QJsonObject WindowsCommands::powerState() {
    QJsonArray plans;
    GUID *active = nullptr;
    QString current;
    if (PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS) {
        current = guidText(*active);
        LocalFree(active);
    }
    for (DWORD i = 0; i < 64; ++i) {
        GUID id;
        DWORD length = sizeof(id);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i, reinterpret_cast<UCHAR *>(&id),
                           &length) != ERROR_SUCCESS)
            break;
        DWORD size = 0;
        PowerReadFriendlyName(nullptr, &id, nullptr, nullptr, nullptr, &size);
        QString name = guidText(id);
        if (size > 0 && size < 65536) {
            QByteArray bytes(size, 0);
            if (PowerReadFriendlyName(nullptr, &id, nullptr, nullptr, reinterpret_cast<UCHAR *>(bytes.data()),
                                      &size) == ERROR_SUCCESS)
                name = QString::fromWCharArray(reinterpret_cast<const wchar_t *>(bytes.constData()));
        }
        plans.append(QJsonObject{{"id", guidText(id)}, {"name", name.left(256)}});
    }
    QJsonObject modes, modeKeys;
    auto library = GetModuleHandleW(L"powrprof.dll");
    for (const auto &supply : {"ac", "dc"}) {
        const bool ac = QString(supply) == "ac";
        auto get = reinterpret_cast<DWORD(WINAPI *)(GUID *)>(GetProcAddress(
            library, ac ? "PowerGetUserConfiguredACPowerMode" : "PowerGetUserConfiguredDCPowerMode"));
        auto set = GetProcAddress(library, ac ? "PowerSetUserConfiguredACPowerMode"
                                              : "PowerSetUserConfiguredDCPowerMode");
        GUID value;
        if (get && set && get(&value) == ERROR_SUCCESS)
            for (const auto &mode : {"Best power efficiency", "Balanced", "Best performance"})
                if (guidText(value).compare(modeGuid(mode), Qt::CaseInsensitive) == 0) {
                    modes[supply] = mode;
                    modeKeys[supply] = QString(mode) == "Best power efficiency" ? "efficiency"
                                      : QString(mode) == "Best performance" ? "performance" : "balanced";
                }
    }
    return {{"plans", plans}, {"active", current}, {"modes", modes}, {"mode_keys", modeKeys}};
}

QString WindowsCommands::execute(const QString &action, const QJsonObject &args) {
    const auto invalid = validate(action, args);
    if (!invalid.isEmpty())
        return invalid;
    if (action == "keyboard" || action == "mouse")
        return WindowsInput::execute(action, args);
    if (action.startsWith("power_mode_")) {
        auto set = reinterpret_cast<DWORD(WINAPI *)(const GUID *)>(GetProcAddress(
            GetModuleHandleW(L"powrprof.dll"), action.endsWith("ac") ? "PowerSetUserConfiguredACPowerMode"
                                                                     : "PowerSetUserConfiguredDCPowerMode"));
        if (!set)
            return "This Windows installation does not expose power-mode control";
        const auto text = modeGuid(args["value"].toString());
        GUID value;
        CLSIDFromString(reinterpret_cast<LPCOLESTR>(text.utf16()), &value);
        const DWORD result = set(&value);
        return result == ERROR_SUCCESS ? QString() : failure("Power mode", result);
    }
    if (action == "volume" || action == "volume_up" || action == "volume_down" || action == "mute") {
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator *enumerator = nullptr;
        IMMDevice *device = nullptr;
        IAudioEndpointVolume *volume = nullptr;
        HRESULT result =
            CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(result))
            result = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        if (SUCCEEDED(result))
            result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void **>(&volume));
        if (SUCCEEDED(result)) {
            if (action == "volume_up" || action == "volume_down") {
                float current;
                result = volume->GetMasterVolumeLevelScalar(&current);
                if (SUCCEEDED(result))
                    result = volume->SetMasterVolumeLevelScalar(
                        qBound(0.0f, current + (action == "volume_up" ? .05f : -.05f), 1.0f), nullptr);
            } else
                result = action == "volume"
                             ? volume->SetMasterVolumeLevelScalar(float(args["value"].toDouble()), nullptr)
                             : volume->SetMute(args["value"].toBool(), nullptr);
        }
        if (volume)
            volume->Release();
        if (device)
            device->Release();
        if (enumerator)
            enumerator->Release();
        if (SUCCEEDED(initialized))
            CoUninitialize();
        return SUCCEEDED(result) ? QString() : failure("Audio control", DWORD(result));
    }
    if (action == "power_plan") {
        const auto value = args["value"].toString();
        bool found = false;
        for (auto p : powerState()["plans"].toArray())
            if (p.toObject()["id"].toString().compare(value, Qt::CaseInsensitive) == 0)
                found = true;
        if (!found)
            return "Power plan is no longer installed";
        GUID id;
        CLSIDFromString(reinterpret_cast<LPCOLESTR>(value.utf16()), &id);
        const auto result = PowerSetActiveScheme(nullptr, &id);
        return result == ERROR_SUCCESS ? QString() : failure("Power plan", result);
    }
    if (action == "open_url") {
        const auto url = QUrl(args["value"].toString()).toString(QUrl::FullyEncoded);
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            nullptr, L"open", reinterpret_cast<LPCWSTR>(url.utf16()), nullptr, nullptr, SW_SHOWNORMAL));
        return result > 32 ? QString() : failure("Open URL", DWORD(result));
    }
    if (action == "lock")
        return LockWorkStation() ? QString() : failure("Lock workstation");
    if (action == "monitor_sleep") {
        // A synchronous broadcast can wait once per unresponsive window.
        // Queue the request; success means dispatched, not confirmed display sleep.
        return PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2)
                   ? QString()
                   : failure("Request monitor sleep");
    }
    if (action == "sleep" || action == "hibernate") {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
            return failure("Open suspend privilege");
        TOKEN_PRIVILEGES requested{}, previous{};
        DWORD bytes = sizeof(previous);
        requested.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &requested.Privileges[0].Luid)) {
            CloseHandle(token);
            return failure("Find suspend privilege");
        }
        requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        const bool adjusted =
            AdjustTokenPrivileges(token, FALSE, &requested, sizeof(previous), &previous, &bytes);
        const DWORD error = GetLastError();
        if (!adjusted || error != ERROR_SUCCESS) {
            CloseHandle(token);
            return failure("Enable suspend privilege", error);
        }
        const bool ok = SetSuspendState(action == "hibernate", FALSE, FALSE);
        const DWORD result = GetLastError();
        AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
        CloseHandle(token);
        return ok ? QString() : failure("Suspend", result);
    }
    if (action == "reboot" || action == "shutdown") {
        QProcess process;
        process.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
        process.start(qEnvironmentVariable("SystemRoot", "C:/Windows") + "/System32/shutdown.exe",
                      {action == "reboot" ? "/r" : "/s", "/t", "0"});
        if (!process.waitForStarted(2000))
            return "Could not start Windows shutdown";
        if (!process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished(1000);
            return "Windows shutdown command timed out";
        }
        return process.exitCode() == 0
                   ? QString()
                   : QString("Windows rejected shutdown (exit %1)").arg(process.exitCode());
    }
    QProcess process;
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
    QStringList arguments{"--media-command", action};
    if (args.contains("value"))
        arguments << (args["value"].isBool()     ? (args["value"].toBool() ? "true" : "false")
                      : args["value"].isString() ? args["value"].toString()
                                                 : QString::number(args["value"].toDouble(), 'g', 16));
    process.start(QCoreApplication::applicationDirPath() + "/helpers/WHAT.WindowsHelper.exe", arguments);
    if (!process.waitForStarted(1500))
        return "Media helper is missing or could not start";
    if (!process.waitForFinished(6000)) {
        process.kill();
        process.waitForFinished(1000);
        return "Media command timed out";
    }
    return process.exitCode() == 0 ? QString()
                                   : "The current media app rejected or does not support this command";
}
