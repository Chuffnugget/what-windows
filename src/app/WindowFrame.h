#pragma once
#include <windows.h>
#include <dwmapi.h>
#include "WindowActivation.h"
#include <functional>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>

// Retain Windows sizing/snap styles while WHAT paints the entire client frame.
// No ToolWindow flag: the ordinary taskbar button and Alt+Tab entry are retained.
class WindowFrame final : public QAbstractNativeEventFilter {
    HWND handle;

public:
    std::function<void()> openRequested;
    std::function<void()> alreadyRunningRequested;
    std::function<void(const QString &)> transitionRequested;

    explicit WindowFrame(HWND window) : handle(window) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        SetPropW(handle, L"WHAT.MainWindow", reinterpret_cast<HANDLE>(1));
        SetPropW(handle, L"WHAT.InstallerQuit", reinterpret_cast<HANDLE>(1));
        SetWindowLongPtrW(handle, GWL_STYLE,
                          GetWindowLongPtrW(handle, GWL_STYLE) | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU |
                              WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    void setAnimationsEnabled(bool enabled) {
        const BOOL disabled = !enabled;
        DwmSetWindowAttribute(handle, DWMWA_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled));
    }

    ~WindowFrame() override {
        RemovePropW(handle, L"WHAT.InstallerQuit");
        RemovePropW(handle, L"WHAT.MainWindow");
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }

    bool nativeEventFilter(const QByteArray &, void *message, qintptr *result) override {
        auto *msg = static_cast<MSG *>(message);
        if (msg->hwnd != handle)
            return false;
        if (msg->message == whatInstallerQuitMessage()) {
            // Finish this native callback before exiting the event loop. Normal
            // stack destruction still saves settings and disposes the workers.
            QTimer::singleShot(0, QCoreApplication::instance(), [] { QCoreApplication::exit(0); });
            if (result)
                *result = 0;
            return true;
        }
        if (msg->message == whatOpenWindowMessage() && openRequested) {
            openRequested();
            if (msg->wParam == 1 && alreadyRunningRequested)
                alreadyRunningRequested();
            if (result)
                *result = 0;
            return true;
        }
        if (msg->message == WM_SYSCOMMAND && transitionRequested) {
            const auto command = msg->wParam & 0xfff0;
            if (command == SC_MINIMIZE || command == SC_MAXIMIZE || command == SC_RESTORE) {
                if (command == SC_RESTORE && IsIconic(handle) && openRequested)
                    openRequested();
                else
                    transitionRequested(command == SC_MINIMIZE   ? "minimise"
                                        : command == SC_MAXIMIZE ? "maximise"
                                                                 : "restore");
                if (result)
                    *result = 0;
                return true;
            }
        }
        if (msg->message == WM_NCCALCSIZE) {
            if (IsZoomed(handle)) {
                MONITORINFO monitor{sizeof(MONITORINFO)};
                if (GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor))
                    *reinterpret_cast<RECT *>(msg->lParam) = monitor.rcWork;
            }
            *result = 0;
            return true;
        }
        // The QML edge handles and startSystemMove own hit testing. Do not let
        // the invisible Windows caption intercept WHAT's custom window buttons.
        if (msg->message == WM_NCHITTEST) {
            *result = HTCLIENT;
            return true;
        }
        return false;
    }
};
