#pragma once
#include <windows.h>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>

inline UINT whatOpenWindowMessage() {
    static const UINT message = RegisterWindowMessageW(L"WHAT.OpenMainWindow.1");
    return message;
}

// Setup sends this only after the user approves closing WHAT for installation.
// Unlike WM_QUIT, it does not ask auxiliary icon windows to accept a close event.
inline UINT whatInstallerQuitMessage() {
    static const UINT message = RegisterWindowMessageW(L"WHAT.QuitForInstaller.1");
    return message;
}

inline bool notifyExistingWhatWindow(qint64 processId) {
    struct Search {
        DWORD pid;
        HWND window = nullptr;
    } search{DWORD(processId)};

    EnumWindows(
        [](HWND window, LPARAM context) -> BOOL {
            auto &search = *reinterpret_cast<Search *>(context);
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == search.pid && GetPropW(window, L"WHAT.MainWindow")) {
                search.window = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.window)
        return false;
    AllowSetForegroundWindow(search.pid);
    return PostMessageW(search.window, whatOpenWindowMessage(), 1, 0) != FALSE;
}

inline void restoreWhatWindow(QQuickWindow *window, const QString &savedMode = {}) {
    // Defer until the native tray/menu activation has finished unwinding.
    QTimer::singleShot(0, window, [window, savedMode] {
        const auto handle = reinterpret_cast<HWND>(window->winId());
        const bool fullscreen = savedMode == "fullscreen" ||
                                (savedMode.isEmpty() && window->windowState() == Qt::WindowFullScreen);
        const bool maximised =
            savedMode == "maximised" ||
            (savedMode.isEmpty() && (window->windowState() == Qt::WindowMaximized || IsZoomed(handle)));
        // Clear the native minimised placement before Qt applies fullscreen geometry.
        if (fullscreen && IsIconic(handle))
            window->showNormal();
        if (fullscreen)
            window->showFullScreen();
        else if (maximised)
            window->showMaximized();
        else
            window->showNormal();
        if (!fullscreen)
            ShowWindow(handle, maximised ? SW_SHOWMAXIMIZED : SW_RESTORE);
        // A detached display or a stale minimized position must not strand the title bar.
        bool reachable = false;
        const QRect title(window->x(), window->y(), window->width(), 44);
        for (auto *screen : QGuiApplication::screens()) {
            const QRect visible = screen->availableGeometry().intersected(title);
            if (visible.width() >= qMin(160, title.width()) && visible.height() >= 32) {
                reachable = true;
                break;
            }
        }
        if (!fullscreen && !reachable && QGuiApplication::primaryScreen()) {
            const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
            window->setPosition(area.x() + qMax(0, (area.width() - window->width()) / 2),
                                area.y() + qMax(0, (area.height() - window->height()) / 2));
        }
        window->raise();
        window->requestActivate();
        SetForegroundWindow(handle);
        // Let Qt Quick schedule the repaint through its scene graph.
        window->update();
    });
}
