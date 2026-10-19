#include "../../../app/CrashDiagnostics.h"
// Small C++/WinRT companion, built with MSVC + Windows SDK. LGPL-3.0-or-later.
// Fixed media commands only; network authentication/permissions are enforced by the app.
#include <windows.h>
#include <appmodel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <algorithm>
#include <cmath>
#include <string>

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Media::Control;
using namespace Windows::UI::Notifications;
using namespace Windows::UI::Notifications::Management;

JsonValue text(hstring value) {
    auto s = std::wstring(value);
    if (s.size() > 255)
        s.resize(255);
    return JsonValue::CreateStringValue(s);
}

void add(JsonArray &rows, const wchar_t *id, const wchar_t *group, const wchar_t *name, IJsonValue value,
         const wchar_t *kind = L"text") {
    JsonObject row;
    row.SetNamedValue(L"id", text(id));
    row.SetNamedValue(L"source", text(L"windows"));
    row.SetNamedValue(L"group", text(group));
    row.SetNamedValue(L"name", text(name));
    row.SetNamedValue(L"value", value);
    row.SetNamedValue(L"kind", text(kind));
    row.SetNamedValue(L"type", JsonValue::CreateNumberValue(0));
    row.SetNamedValue(L"unit", text(L""));
    row.SetNamedValue(L"available", JsonValue::CreateBooleanValue(value.ValueType() != JsonValueType::Null));
    row.SetNamedValue(L"state_class", text(kind == std::wstring(L"number") ? L"measurement" : L""));
    rows.Append(row);
}

bool packaged() {
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) == ERROR_INSUFFICIENT_BUFFER;
}

hstring timestamp(Windows::Foundation::DateTime time) {
    ULARGE_INTEGER ticks;
    ticks.QuadPart = time.time_since_epoch().count();
    FILETIME file{ticks.LowPart, ticks.HighPart};
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&file, &utc))
        return L"";
    wchar_t value[40];
    swprintf_s(value, L"%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
               utc.wMinute, utc.wSecond);
    return value;
}

fire_and_forget requestAccess(DWORD thread) {
    try {
        auto access = co_await UserNotificationListener::Current().RequestAccessAsync();
        MessageBoxW(
            nullptr,
            access == UserNotificationListenerAccessStatus::Allowed
                ? L"Notification access allowed. Return to WHAT and enable notification collection in Settings."
                : L"Notification access was not allowed. You can review this in Windows Settings > Privacy & security > Notifications.",
            L"WHAT notification access", MB_OK | MB_ICONINFORMATION);
    } catch (hresult_error const &) {
        MessageBoxW(
            nullptr,
            L"Windows could not grant access. Launch the installed WHAT package from the Start menu, then try again. Review Privacy & security > Notifications in Windows Settings.",
            L"WHAT notification access", MB_OK | MB_ICONINFORMATION);
    }
    PostThreadMessageW(thread, WM_QUIT, 0, 0);
}

#include "../lighting/LightingBridge.h"

int bridgeMain(int argc, wchar_t **argv) {
    if (argc == 1) {
        MessageBoxW(
            nullptr,
            L"This helper is started automatically by WHAT and should not be opened manually.\n\nPlease run WHAT.exe instead (or open WHAT from the Start menu).",
            L"WHAT Windows Helper", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }
    auto flag = [&](const wchar_t *name) {
        for (int i = 1; i < argc; ++i)
            if (std::wstring(argv[i]) == name)
                return true;
        return false;
    };
    if (flag(L"--request-notifications")) {
        init_apartment(apartment_type::single_threaded);
        if (!packaged()) {
            MessageBoxW(
                nullptr,
                L"Windows requires package identity for notification access. Use the optional package installer included with WHAT, launch it from the Start menu, then request access again. All other Windows readings work in the portable app.",
                L"WHAT notification access", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        MSG message;
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        requestAccess(GetCurrentThreadId());
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return 0;
    }
    init_apartment(apartment_type::multi_threaded);
    if (flag(L"--lighting-server"))
        return lightingServer();
    JsonArray rows;
    if (argc >= 3 && std::wstring(argv[1]) == L"--media-command") {
        try {
            auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = manager.GetCurrentSession();
            if (!session)
                return 2;
            const std::wstring action = argv[2];
            bool ok = false;
            if (action == L"play")
                ok = session.TryPlayAsync().get();
            else if (action == L"pause")
                ok = session.TryPauseAsync().get();
            else if (action == L"stop")
                ok = session.TryStopAsync().get();
            else if (action == L"next")
                ok = session.TrySkipNextAsync().get();
            else if (action == L"previous")
                ok = session.TrySkipPreviousAsync().get();
            else if (argc == 4 && action == L"seek") {
                const auto seconds = std::stod(argv[3]);
                if (!std::isfinite(seconds) || seconds < 0 || seconds > 604800)
                    return 2;
                ok = session.TryChangePlaybackPositionAsync(int64_t(seconds * 10000000)).get();
            } else if (argc == 4 && action == L"shuffle") {
                if (std::wstring(argv[3]) != L"true" && std::wstring(argv[3]) != L"false")
                    return 2;
                ok = session.TryChangeShuffleActiveAsync(std::wstring(argv[3]) == L"true").get();
            } else if (argc == 4 && action == L"repeat") {
                const std::wstring mode = argv[3];
                using Windows::Media::MediaPlaybackAutoRepeatMode;
                if (mode != L"off" && mode != L"one" && mode != L"all")
                    return 2;
                ok = session
                         .TryChangeAutoRepeatModeAsync(mode == L"one"   ? MediaPlaybackAutoRepeatMode::Track
                                                       : mode == L"all" ? MediaPlaybackAutoRepeatMode::List
                                                                        : MediaPlaybackAutoRepeatMode::None)
                         .get();
            }
            return ok ? 0 : 2;
        } catch (...) {
            return 2;
        }
    }
    if (flag(L"--notifications")) {
        try {
            if (!packaged())
                add(rows, L"win_notifications_status", L"Notifications", L"Collection status",
                    text(L"Requires optional packaged installation and Windows permission"));
            else {
                auto listener = UserNotificationListener::Current();
                if (listener.GetAccessStatus() != UserNotificationListenerAccessStatus::Allowed)
                    add(rows, L"win_notifications_status", L"Notifications", L"Collection status",
                        text(L"Access not granted; use Request notification access in Settings"));
                else {
                    auto notifications = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
                    add(rows, L"win_notifications_status", L"Notifications", L"Collection status",
                        text(L"Ready"));
                    add(rows, L"win_notifications_count", L"Notifications", L"Current toast notifications",
                        JsonValue::CreateNumberValue(notifications.Size()), L"number");
                    UserNotification latest{nullptr};
                    for (auto const &notification : notifications)
                        if (!latest || notification.CreationTime() > latest.CreationTime())
                            latest = notification;
                    add(rows, L"win_notifications_app", L"Notifications", L"Latest notification application",
                        latest ? text(latest.AppInfo().DisplayInfo().DisplayName())
                               : JsonValue::CreateNullValue());
                    add(rows, L"win_notifications_time", L"Notifications", L"Latest notification time",
                        latest ? text(timestamp(latest.CreationTime())) : JsonValue::CreateNullValue(),
                        L"timestamp");
                    if (flag(L"--notification-contents")) {
                        std::wstring title, body;
                        if (latest) {
                            auto binding = latest.Notification().Visual().GetBinding(
                                KnownNotificationBindings::ToastGeneric());
                            if (binding) {
                                bool first = true;
                                for (const auto &element : binding.GetTextElements()) {
                                    if (first) {
                                        title = element.Text();
                                        first = false;
                                    } else {
                                        if (!body.empty())
                                            body += L" ";
                                        body += element.Text();
                                    }
                                }
                            }
                        }
                        add(rows, L"win_notifications_title", L"Notifications", L"Latest title",
                            latest ? text(title.c_str()) : JsonValue::CreateNullValue());
                        add(rows, L"win_notifications_body", L"Notifications",
                            L"Latest body (first 255 characters)",
                            latest ? text(body.c_str()) : JsonValue::CreateNullValue());
                    }
                }
            }
        } catch (hresult_error const &) {
            add(rows, L"win_notifications_status", L"Notifications", L"Collection status",
                text(L"Notification API unavailable or permission revoked"));
        }
    }
    const auto output = to_string(rows.Stringify());
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(), DWORD(output.size()), &written, nullptr);
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    wchar_t directory[2048]{};
    const DWORD length = GetEnvironmentVariableW(L"WHAT_DIAGNOSTICS_DIR", directory, 2048);
    char version[64]{};
    GetEnvironmentVariableA("WHAT_DIAGNOSTICS_VERSION", version, 64);
    const char *module = "Windows";
    if (argc > 1) {
        if (std::wstring(argv[1]) == L"--lighting-server")
            module = "Dynamic Lighting";
        else if (std::wstring(argv[1]) == L"--media-command")
            module = "Media commands";
        else
            module = "Notifications";
    }
    if (length && length < 2048) {
        CrashDiagnostics::initialize(directory, "WindowsHelper", version, module);
        CrashDiagnostics::appendAt(directory, module, "INFO", "Process started");
    }
    const int result = bridgeMain(argc, argv);
    if (length && length < 2048) {
        char message[64];
        snprintf(message, sizeof(message), "Process exited: code=%d", result);
        CrashDiagnostics::appendAt(directory, module, result ? "ERROR" : "INFO", message);
    }
    return result;
}
