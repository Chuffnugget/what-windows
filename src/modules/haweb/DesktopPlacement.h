#pragma once
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <windows.h>
#include <dwmapi.h>

// Keep a normal top-level window immediately above Explorer's desktop. This
// avoids cross-process SetParent, which can reset the app's DPI awareness.
// Only WHAT's window is changed; Explorer and wallpaper settings stay intact.
class DesktopPlacement final : public QObject, public QAbstractNativeEventFilter {
public:
    explicit DesktopPlacement(QObject *parent) : QObject(parent) {
        pending.setSingleShot(true);
        connect(&pending, &QTimer::timeout, this, [this] { place(); });
    }

    ~DesktopPlacement() override {
        detach();
    }

    void attach(QWindow *target) {
        if (window == target)
            return;
        detach();
        window = target;
        if (!window)
            return;
        // Create the HWND before installing the native filter. Calling winId()
        // from that filter can recursively create a window during WM_CREATE.
        own = reinterpret_cast<HWND>(window->winId());
        window->installEventFilter(this);
        QCoreApplication::instance()->installNativeEventFilter(this);
        const BOOL exclude = TRUE;
        DwmSetWindowAttribute(own, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof(exclude));
        for (const auto range : {qMakePair(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND),
                                 qMakePair(EVENT_OBJECT_SHOW, EVENT_OBJECT_REORDER)}) {
            const auto hook =
                SetWinEventHook(range.first, range.second, nullptr, changed, 0, 0, WINEVENT_OUTOFCONTEXT);
            if (hook)
                hooks.insert(hook, this);
        }
        place();
    }

    void detach() {
        pending.stop();
        for (auto it = hooks.begin(); it != hooks.end();) {
            if (it.value() == this) {
                UnhookWinEvent(it.key());
                it = hooks.erase(it);
            } else
                ++it;
        }
        if (window)
            window->removeEventFilter(this);
        QCoreApplication::instance()->removeNativeEventFilter(this);
        window = nullptr;
        own = nullptr;
    }

private:
    inline static QHash<HWINEVENTHOOK, DesktopPlacement *> hooks;
    QPointer<QWindow> window;
    QTimer pending;
    HWND shell = nullptr;
    HWND own = nullptr;
    bool placing = false;
    bool showingDesktop = false;

    static void CALLBACK changed(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG, DWORD,
                                 DWORD) {
        const auto owner = hooks.value(hook);
        if (!owner || !owner->window)
            return;
        // Foreground changes cover Show Desktop and Explorer restarts. Object
        // notifications only matter when Explorer changes the desktop's order.
        if (event == EVENT_SYSTEM_FOREGROUND || (hwnd == owner->shell && object == OBJID_WINDOW))
            owner->pending.start(0);
    }

    void place() {
        if (!window)
            return;
        shell = GetShellWindow();
        EnumWindows(
            [](HWND hwnd, LPARAM result) -> BOOL {
                if (!FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr))
                    return TRUE;
                *reinterpret_cast<HWND *>(result) = hwnd;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&shell));
        own = reinterpret_cast<HWND>(window->winId());
        if (!IsWindow(shell))
            return;
        const auto foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
        if (foreground == shell || foreground == GetShellWindow())
            showingDesktop = true;
        else if (foreground && foreground != own && foreground != FindWindowW(L"Shell_TrayWnd", nullptr))
            showingDesktop = false;
        placing = true;
        if (IsIconic(own) || !IsWindowVisible(own))
            ShowWindow(own, SW_SHOWNOACTIVATE);
        // Explorer raises its desktop for Win+D. Follow that desktop without
        // activating WHAT or putting it above any of the remaining app windows.
        auto above = GetWindow(shell, GW_HWNDPREV);
        // Hidden shell utility windows can retain a topmost style at the bottom
        // of the order. Inserting behind one would promote our desktop to topmost.
        while (above && (!IsWindowVisible(above) || IsIconic(above)))
            above = GetWindow(above, GW_HWNDPREV);
        if (showingDesktop || above != own) {
            if (!above || (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST))
                above = HWND_TOP;
            if (showingDesktop) {
                // HWND_TOP requires foreground activation permission. Instead
                // insert just below the topmost band, keeping the taskbar above
                // us without activating WHAT or making the desktop topmost.
                above = HWND_TOP;
                for (auto candidate = GetTopWindow(nullptr); candidate;
                     candidate = GetWindow(candidate, GW_HWNDNEXT)) {
                    if (!(GetWindowLongPtrW(candidate, GWL_EXSTYLE) & WS_EX_TOPMOST))
                        break;
                    above = candidate;
                }
            }
            SetWindowPos(own, above, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        placing = false;
    }

    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Hide || event->type() == QEvent::WindowStateChange ||
            event->type() == QEvent::PlatformSurface)
            pending.start(0);
        return false;
    }

    bool nativeEventFilter(const QByteArray &, void *raw, qintptr *result) override {
        const auto message = static_cast<MSG *>(raw);
        if (!window || !own || message->hwnd != own)
            return false;
        if (message->message == WM_WINDOWPOSCHANGING && !placing)
            reinterpret_cast<WINDOWPOS *>(message->lParam)->flags |= SWP_NOZORDER;
        if (message->message == WM_SYSCOMMAND && (message->wParam & 0xfff0) == SC_MINIMIZE) {
            if (result)
                *result = 0;
            return true;
        }
        return false;
    }
};
