#pragma once
#include "HomeAssistantWeb.h"
#include "DesktopPlacement.h"
#include "WebOptions.h"
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <QPointer>
#include <QTimer>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

inline QString takeWebString(LPWSTR value) {
    if (!value)
        return {};
    const auto text = QString::fromWCharArray(value);
    CoTaskMemFree(value);
    return text;
}

struct HomeAssistantWeb::State {
    QSettings &settings;
    QString profile, address, viewMode, shortcut, shortcutError, message;
    int zoom = 100;
    bool active = false, testing = false, presented = false, creating = false;
    bool loading = false, loaded = false, clearing = false, clearLogin = false;
    bool failed = false;
    bool registered = false, comInitialized = false, overlayVisible = false;
    bool suspendRequested = false;
    bool replacingSurface = false;
    quint64 generation = 0;
    UINT64 navigation = 0;
    UINT32 browserPid = 0;
    HWND previousForeground = nullptr;
    HWND parkingWindow = nullptr;
    HWND browserParent = nullptr;
    QPointer<QWindow> overlay;
    QPointer<QWindow> desktop;
    QString overlayMode = "fullscreen";
    bool desktopActive = false;
    QString presentationMode;
    bool pipRegistered = false;
    QString pipShortcutError;
    bool pipMinimised = false;
    QRect pipRestoreBounds;
    QPoint pipDockedPosition;
    int pipRestoreMaximumHeight = 0;
    QVariantMap options;
    QString source, target, lastError, runtimeVersion;
    bool usingCloud = false, suspended = false, back = false, forward = false;
    bool signingIn = false;
    bool permissionBusy = false;
    QString permissionStatus = "Open the dashboard, then refresh to inspect site permissions.";
    QVariantList permissions, permissionQueue;
    int retries = 0;
    QTimer recovery, stable, navigationTimeout;
    QTimer saveGeometry;
    DesktopPlacement desktopPlacement{nullptr};
    std::unique_ptr<QWindow> window = std::make_unique<QWindow>();
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;

    explicit State(QSettings &settings) : settings(settings) {}
};
