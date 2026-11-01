#pragma once
#include "modules/haweb/HomeAssistantWeb.h"
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QPointer>
#include <QQuickWindow>

// Run on confirmed application exit, including installer-requested exit. Closing
// a window normally still hides it to the tray and keeps its graphics resources.
inline void installGuiShutdown(QObject &context, HomeAssistantWeb &web) {
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &context, [&web] {
        qInfo("Shutdown: closing embedded browser");
        // Close WebView2 before QML destroys its WindowContainer/native parents.
        // This also invalidates pending creation callbacks and prevents recovery.
        web.shutdown();
        qInfo("Shutdown: releasing Qt Quick graphics resources");
        QList<QPointer<QQuickWindow>> windows;
        for (auto *window : QGuiApplication::allWindows()) {
            if (auto *quick = qobject_cast<QQuickWindow *>(window)) {
                windows.append(quick);
                quick->setPersistentSceneGraph(false);
                quick->setPersistentGraphics(false);
            }
        }
        // Do not call closeAllWindows(): taskbar indicators and Web windows veto
        // ordinary closes. Hide first, then release on Qt's owning render thread
        // while the application and native window hierarchy still exist.
        for (const auto &window : windows)
            if (window)
                window->hide();
        for (const auto &window : windows)
            if (window)
                window->releaseResources();
        qInfo("Shutdown: graphics resources released; finishing worker cleanup");
    }, Qt::DirectConnection);
}
