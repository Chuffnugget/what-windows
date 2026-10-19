#pragma once
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QWindow>
#include <QIcon>
#include <QTimer>
#include <QUuid>
#include <QDir>
#include <QSystemTrayIcon>
#include <windows.h>
#include <shellapi.h>

// Qt's tray API does not expose a persistent identity or registration errors.
// A GUID keeps changing readings distinct in Explorer's notification cache.
class NativeSensorIcon final : public QObject, public QAbstractNativeEventFilter {
public:
    NativeSensorIcon(const QString &profile, const QString &sensor, QObject *parent) : QObject(parent) {
        const QUuid ns("{05eb090b-0920-4b7b-9d27-076af3e5d793}");
        // Separate portable/debug executables can coexist without claiming one
        // another's protected GUID. Normal updates at the same path keep it.
        identity =
            QUuid::createUuidV5(ns, (QDir::cleanPath(QCoreApplication::applicationFilePath()).toLower() +
                                     '\n' + QDir::cleanPath(profile).toLower() + '\n' + sensor)
                                        .toUtf8());
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint);
        window.create();
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        ChangeWindowMessageFilterEx(hwnd(), taskbarCreated, MSGFLT_ALLOW, nullptr);
        QCoreApplication::instance()->installNativeEventFilter(this);
        retry.setInterval(2000);
        connect(&retry, &QTimer::timeout, this, [this] {
            if (wanted)
                update();
        });
    }

    ~NativeSensorIcon() override {
        hide();
        QCoreApplication::instance()->removeNativeEventFilter(this);
        if (handle)
            DestroyIcon(handle);
    }

    std::function<void(QSystemTrayIcon::ActivationReason)> activated;
    std::function<void()> statusChanged;

    QUuid guid() const {
        return identity;
    }

    QIcon icon() const {
        return image;
    }

    QString toolTip() const {
        return tip;
    }

    QString error() const {
        return failure;
    }

    bool isVisible() const {
        return wanted && registered;
    }

    bool registeredWithShell() const {
        NOTIFYICONIDENTIFIER id{};
        id.cbSize = sizeof(id);
        id.guidItem = identity;
        RECT rect{};
        // S_FALSE means registered in the overflow, not registration failure.
        return SUCCEEDED(Shell_NotifyIconGetRect(&id, &rect));
    }

    void setIcon(const QIcon &icon) {
        image = icon;
        const int size = GetSystemMetrics(SM_CXSMICON);
        const auto pixels = icon.pixmap(size, size).toImage();
        auto next = pixels.toHICON(pixels.createAlphaMask().convertToFormat(QImage::Format_Mono));
        if (!next) {
            setError("Windows could not create this sensor icon.");
            return;
        }
        const auto old = handle;
        handle = next;
        if (wanted)
            update();
        if (old)
            DestroyIcon(old);
    }

    void setToolTip(const QString &value) {
        if (tip == value)
            return;
        tip = value;
        if (wanted)
            update();
    }

    void show() {
        wanted = true;
        update();
    }

    void hide() {
        wanted = false;
        retry.stop();
        if (registered) {
            auto data = notification();
            Shell_NotifyIconW(NIM_DELETE, &data);
        }
        registered = false;
    }

    bool nativeEventFilter(const QByteArray &, void *raw, qintptr *result) override {
        const auto *message = static_cast<MSG *>(raw);
        if (message->hwnd != hwnd())
            return false;
        if (message->message == taskbarCreated) {
            registered = false;
            if (wanted)
                update();
        } else if (message->message == callbackMessage) {
            const auto event = LOWORD(message->lParam);
            if (activated) {
                if (event == WM_CONTEXTMENU)
                    activated(QSystemTrayIcon::Context);
                else if (event == NIN_SELECT || event == NIN_KEYSELECT)
                    activated(QSystemTrayIcon::Trigger);
                else if (event == WM_LBUTTONDBLCLK)
                    activated(QSystemTrayIcon::DoubleClick);
            }
            if (result)
                *result = 0;
            return true;
        }
        return false;
    }

private:
    HWND hwnd() const {
        return reinterpret_cast<HWND>(window.winId());
    }

    NOTIFYICONDATAW notification() const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd();
        data.uFlags = NIF_GUID | NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
        data.guidItem = identity;
        data.hIcon = handle;
        data.uCallbackMessage = callbackMessage;
        const auto value = tip.left(127);
        value.toWCharArray(data.szTip);
        data.szTip[value.size()] = 0;
        return data;
    }

    void setError(QString value) {
        if (failure == value)
            return;
        failure = std::move(value);
        if (statusChanged)
            QTimer::singleShot(0, this, [this] {
                if (statusChanged)
                    statusChanged();
            });
    }

    void update() {
        if (!handle)
            return;
        auto data = notification();
        bool ok = registered && Shell_NotifyIconW(NIM_MODIFY, &data);
        if (!ok) {
            registered = false;
            ok = Shell_NotifyIconW(NIM_ADD, &data);
            if (ok) {
                data.uVersion = NOTIFYICON_VERSION_4;
                Shell_NotifyIconW(NIM_SETVERSION, &data);
            }
        }
        registered = ok;
        if (ok) {
            retry.stop();
            setError({});
        } else {
            if (!retry.isActive())
                retry.start();
            setError("Windows could not add this tray icon. Retrying automatically.");
        }
    }

    static constexpr UINT callbackMessage = WM_APP + 102;
    QWindow window;
    QTimer retry;
    QUuid identity;
    UINT taskbarCreated = 0;
    HICON handle = nullptr;
    QIcon image;
    QString tip, failure;
    bool wanted = false, registered = false;
};
