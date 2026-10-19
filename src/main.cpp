#include "app/Controller.h"
#include "app/AppRuntime.h"
#include "app/LocalDiagnostics.h"
#include "app/PackageIdentity.h"
#include "app/TrayUi.h"
#include <QRegularExpression>
#include "app/StartupNotice.h"
#include "app/TaskbarIcon.h"
#include "app/WindowFrame.h"
#include "updater/UpdateManager.h"
#include "modules/haweb/HomeAssistantWeb.h"
#include <windows.h>
#include <cstdio>
#include <memory>
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <openssl/crypto.h>

int main(int argc, char **argv) {
    const auto stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
    if ((!stderrHandle || stderrHandle == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *stream;
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
    // Portable app: do not load external OpenSSL configuration from build-machine paths.
    OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr);
    // Use Qt's supported platform renderer. QT_QUICK_BACKEND=software remains
    // available for machines with broken graphics drivers; do not force CPU rendering.
    QApplication app(argc, argv);
    const QIcon appIcon(":/assets/app-icon.ico");
    app.setWindowIcon(appIcon);
    app.setApplicationName(packageFamily().startsWith("WHAT.Development_") ? "WHAT Development" : "WHAT");
    app.setOrganizationName("WHAT");
    app.setApplicationVersion(WHAT_VERSION);
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser args;
    args.addHelpOption();
    args.addVersionOption();
    args.addOption({"data-dir", "Use an isolated profile directory", "path"});
    args.addOption({"test-mode", "Bind loopback only; disable discovery and startup changes"});
    args.addOption({"quit-after", "Exit after N seconds (test mode only)", "seconds"});
    args.addOption({"render-preview", "Save the GUI preview and exit (test mode only)", "path"});
    args.addOption({"preview-page", "Page to render: 0-3 first four pages, 4 setup, 5 startup notice, 6 Specs, 7 Modules, 8 About (test mode only)", "index", "0"});
    args.addOption({"restart-after", "Wait for the previous WHAT process to exit", "pid"});
    args.addOption({"toast-activated", "Started by Windows notification activation"});
    args.process(app);
    if (args.isSet("restart-after") && !AppRestart::waitForPrevious(args.value("restart-after").toLongLong()))
        return 1;
    QString directory = args.isSet("data-dir")
                            ? QDir(args.value("data-dir")).absolutePath()
                            : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const auto startupFailure = [&](const QString &message) {
        qCritical().noquote() << message;
        if (!args.isSet("test-mode"))
            QMessageBox::critical(nullptr, "WHAT could not start", message);
        return 1;
    };
    if (!QDir().mkpath(directory))
        return startupFailure("Cannot create the WHAT profile folder:\n" + QDir::toNativeSeparators(directory)
                              + "\nCheck that your Windows account can write to this location.");
    QLockFile lock(directory + "/app.lock");
    if (!lock.tryLock(0)) {
        qint64 owner = 0;
        QString host, name;
        if (lock.getLockInfo(&owner, &host, &name) && owner > 0) {
            // Give an instance still loading QML a bounded chance to expose its window.
            int attempts = 0;
            QTimer retry;
            QObject::connect(&retry, &QTimer::timeout, &app, [&] {
                if (notifyExistingWhatWindow(owner) || ++attempts >= 20)
                    app.quit();
            });
            retry.start(100);
            return app.exec();
        }
        return startupFailure("Cannot lock the WHAT profile:\n" + QDir::toNativeSeparators(directory)
                              + "\nCheck folder permissions and whether another WHAT process is still running.");
    }
    try {
        const auto diagnosticPath = QDir::toNativeSeparators(directory + "/diagnostics");
        SetEnvironmentVariableW(L"WHAT_DIAGNOSTICS_DIR", reinterpret_cast<LPCWSTR>(diagnosticPath.utf16()));
        qputenv("WHAT_DIAGNOSTICS_VERSION", app.applicationVersion().toUtf8());
        LocalDiagnostics localDiagnostics(directory + "/diagnostics");
        Controller controller(directory, args.isSet("test-mode"));
        if (!packageFamily().isEmpty())
            controller.richNotifications->prepareActivation();
        QObject::connect(&controller, &Controller::activityRecorded, &app,
                         [&](QString module, QString message) { localDiagnostics.event(module, message); });
        QObject diagnosticContext;
        auto syncDiagnostics = [&] {
            if (!localDiagnostics.setEnabled(controller.uiInfo()["diagnosticLogging"].toBool())) {
                controller.setDiagnosticLogging(false);
            }
            if (!localDiagnostics.setDumpsEnabled(controller.uiInfo()["crashDumps"].toBool()))
                controller.setCrashDumpsEnabled(false);
        };
        QObject::connect(&controller, &Controller::uiChanged, &diagnosticContext, syncDiagnostics);
        syncDiagnostics();
        QTimer diagnosticTimer;
        QObject::connect(&diagnosticTimer, &QTimer::timeout, &app, [&] {
            QJsonObject modules;
            for (const auto *name : {"windows", "hwinfo", "coretemp", "icue", "nvidia", "beacn", "wallpaper"})
                modules[name] = controller.moduleEnabled(name);
            const auto state = controller.uiInfo();
            QJsonObject versions;
            const auto beacn = controller.beacnState();
            static const QRegularExpression versionPattern("^[0-9]+(?:\\.[0-9]+){1,3}$");
            for (const auto *key : {"api_version", "mixer_version"}) {
                const auto value = beacn.value(key).toString();
                if (versionPattern.match(value).hasMatch())
                    versions[key] = value;
            }
            localDiagnostics.snapshot({{"modules", modules},
                                       {"service_running", state["running"].toBool()},
                                       {"ha_clients", state["clients"].toInt()},
                                       {"beacn_versions", versions}});
        });
        diagnosticTimer.start(30000);
        UpdateManager updater(directory,
                              args.isSet("test-mode") || packageFamily().startsWith("WHAT.Development_"));
        QObject::connect(&updater, &UpdateManager::diagnostic, &controller, &Controller::log);
        auto restartArguments = app.arguments().mid(1);
        const auto executable = app.applicationFilePath(), workingDirectory = QDir::currentPath();
        controller.restartLauncher = [executable, restartArguments, workingDirectory] {
            return AppRestart::launch(executable, restartArguments, workingDirectory);
        };
        controller.restartBlocked = [&updater] { return updater.isBusy(); };
        QObject::connect(&updater, &UpdateManager::changed, &controller,
                         [&] { controller.setAppUpdate(updater.sharedStatus()); });
        controller.setAppUpdate(updater.sharedStatus());
        QQuickStyle::setStyle("WHATStyle");
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{"controller", QVariant::fromValue(&controller)}, {"updater", QVariant::fromValue(&updater)}});
        QStringList interfaceErrors;
        const auto interfaceWarnings = QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&interfaceErrors](const QList<QQmlError> &errors) {
            for (const auto &error : errors) {
                if (interfaceErrors.size() == 20)
                    interfaceErrors.removeFirst();
                interfaceErrors.append(error.toString());
            }
        });
        engine.load(QUrl("qrc:/qml/Main.qml"));
        QObject::disconnect(interfaceWarnings);
        if (engine.rootObjects().isEmpty())
            return startupFailure("WHAT could not load its interface. Reinstall the complete package.\n\n"
                                  + interfaceErrors.join('\n'));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QObject::connect(window, SIGNAL(windowQuitReady()), &app, SLOT(quit()));
        WindowFrame frame(reinterpret_cast<HWND>(window->winId()));
        frame.transitionRequested = [window](const QString &action) {
            QMetaObject::invokeMethod(window, "performWindowAction", Q_ARG(QVariant, action));
        };
        auto syncWindowAnimation = [&] {
            const auto info = controller.uiInfo();
            frame.setAnimationsEnabled(info["animationsEnabled"].toBool() &&
                                       info["windowAnimationsEnabled"].toBool());
        };
        syncWindowAnimation();
        QObject::connect(&controller, &Controller::uiChanged, window, syncWindowAnimation);
        controller.enableNativeEvents(window->winId());
        window->setIcon(appIcon);
        if (!setNativeTaskbarIcon(reinterpret_cast<HWND>(window->winId())))
            controller.log("Could not load the native taskbar icon; using Qt icon fallback");
        controller.trayManager()->taskbarIcons()->attachMainWindow(window);
        QSystemTrayIcon tray(appIcon, &app);
        controller.trayManager()->attachMainTray(&tray);
        QMenu menu;
        styleWhatTrayMenu(&menu, appIcon);
        auto *show = menu.addAction("Open WHAT");
        auto *manageSensors = menu.addAction("Taskbar icons…");
        auto open = [window, &controller] {
            restoreWhatWindow(window, controller.windowMode());
        };
        QObject::connect(&updater, &UpdateManager::updateCompleted, window, open);
        QObject::connect(controller.richNotifications, &RichNotifications::openRequested, window,
                         [window, open](bool expired) {
            open();
            if (expired)
                QMetaObject::invokeMethod(window, "notify",
                    Q_ARG(QVariant, "This notification is no longer active. Request a new one from Home Assistant."),
                    Q_ARG(QVariant, 15000));
        });
        frame.openRequested = open;
        frame.alreadyRunningRequested = [window] {
            QTimer::singleShot(0, window,
                               [window] { QMetaObject::invokeMethod(window, "showAlreadyRunning"); });
        };
        auto openTrayManager = [window, open] {
            window->setProperty("previewPage", 7);
            open();
        };
        QObject::connect(manageSensors, &QAction::triggered, &app, openTrayManager);
        QObject::connect(controller.trayManager(), &SensorTrayManager::openRequested, &app, openTrayManager);
        auto *web = controller.homeAssistantWeb;
        auto *webMenu = menu.addMenu("Home Assistant Web");
        auto *webPage = webMenu->addAction("Inside WHAT");
        auto *webPip = webMenu->addAction("Show Picture-in-Picture");
        auto *webMinimise = webMenu->addAction("Minimise PiP to Title Bar");
        auto *webFullscreen = webMenu->addAction("Show Fullscreen");
        auto *webDesktop = webMenu->addAction("Show on Desktop");
        auto *webHide = webMenu->addAction("Hide Overlay");
        webMenu->addSeparator();
        auto *webSettings = webMenu->addAction("Web Settings");
        QObject::connect(webPage, &QAction::triggered, &app, [web] { web->openView("windowed"); });
        QObject::connect(webPip, &QAction::triggered, web, &HomeAssistantWeb::togglePictureInPicture);
        QObject::connect(webMinimise, &QAction::triggered, web, &HomeAssistantWeb::togglePipMinimised);
        QObject::connect(webFullscreen, &QAction::triggered, &app, [web] { web->openView("fullscreen"); });
        QObject::connect(webDesktop, &QAction::triggered, &app, [web] { web->openView("desktop"); });
        QObject::connect(webHide, &QAction::triggered, web, &HomeAssistantWeb::hideOverlay);
        QObject::connect(webSettings, &QAction::triggered, &app, [window, web, open] {
            web->hideOverlay();
            open();
            QMetaObject::invokeMethod(window, "openModule", Q_ARG(QVariant, "haweb"));
        });
        auto updateWebActions = [=] {
            webMenu->menuAction()->setVisible(web->enabled());
            const bool configured = web->enabled() && !web->url().isEmpty();
            for (auto *action : {webPage, webPip, webFullscreen, webDesktop})
                action->setEnabled(configured);
            const bool pip = web->overlayVisible() && web->overlayMode() == "pip";
            webPip->setText(pip ? "Hide Picture-in-Picture" : "Show Picture-in-Picture");
            webMinimise->setEnabled(pip);
            webMinimise->setText(web->pipMinimised() ? "Restore PiP" : "Minimise PiP to Title Bar");
            webFullscreen->setText(web->overlayVisible() && !pip ? "Hide Fullscreen" : "Show Fullscreen");
            webHide->setEnabled(web->overlayVisible());
        };
        QObject::connect(web, &HomeAssistantWeb::configurationChanged, &app, updateWebActions);
        QObject::connect(web, &HomeAssistantWeb::overlayChanged, &app, updateWebActions);
        updateWebActions();
        auto *updates = menu.addAction("Check for updates…");
        QObject::connect(updates, &QAction::triggered, &app, [&] {
            open();
            QMetaObject::invokeMethod(window, "openUpdateSettings");
            updater.check();
        });
        menu.addSeparator();
        auto *startMonitoring = menu.addAction(QString());
        auto *stopMonitoring = menu.addAction(QString());
        auto *restartMonitoring = menu.addAction(QString());
        auto updateTrayActions = [&] {
            const auto state = controller.trayConnectionInfo();
            const bool active = state["active"].toBool();
            startMonitoring->setText(state["start"].toString());
            stopMonitoring->setText(state["stop"].toString());
            restartMonitoring->setText(state["restart"].toString());
            startMonitoring->setEnabled(!active);
            stopMonitoring->setEnabled(active);
            restartMonitoring->setEnabled(active);
        };
        QObject::connect(startMonitoring, &QAction::triggered, &controller, &Controller::startTrayConnection);
        QObject::connect(stopMonitoring, &QAction::triggered, &controller, &Controller::stopTrayConnection);
        QObject::connect(restartMonitoring, &QAction::triggered, &controller,
                         &Controller::restartTrayConnection);
        QObject::connect(&controller, &Controller::uiChanged, &app, updateTrayActions);
        QObject::connect(&menu, &QMenu::aboutToShow, &app, updateTrayActions);
        updateTrayActions();
        menu.addSeparator();
        auto *quit = menu.addAction("Quit WHAT");
        QObject::connect(show, &QAction::triggered, &app, open);
        QObject::connect(quit, &QAction::triggered, window, [window, open] {
            open();
            // Restore the page before the shared glass overlay captures it.
            QTimer::singleShot(0, window, [window] {
                QMetaObject::invokeMethod(window, "openQuitConfirmation");
            });
        });
        QObject::connect(&tray, &QSystemTrayIcon::activated, &app, [window, web, open, &menu](auto reason) {
            if (window->property("quitConfirmationVisible").toBool()) {
                open();
                return;
            }
            if (reason == QSystemTrayIcon::Context)
                popupWhatTrayMenu(&menu);
            // Windows sends Trigger for the first click before DoubleClick.
            // Handling both would immediately hide a Web overlay we just opened.
            if (reason == QSystemTrayIcon::Trigger) {
                const auto action = web->options()["trayAction"].toString();
                if (action == "what" || !web->enabled() || web->url().isEmpty())
                    open();
                else
                    web->openView(action);
            }
        });
        QObject::connect(&controller, &Controller::uiChanged, &app, [&] {
            controller.trayManager()->setMainToolTip("WHAT\n" + controller.uiInfo()["status"].toString());
        });
        QObject::connect(&updater, &UpdateManager::updateAvailable, &app, [&](const QString &version) {
            if (args.isSet("test-mode"))
                return;
            open();
            tray.showMessage("WHAT update available", "Version " + version + " is available.",
                             QSystemTrayIcon::Information, 8000);
        });
        controller.notificationSender = [&tray](const QString &title, const QString &message,
                                                const QString &level) {
            if (!QSystemTrayIcon::isSystemTrayAvailable() || !QSystemTrayIcon::supportsMessages() ||
                !tray.isVisible())
                return QString("Windows system-tray notifications are unavailable");
            tray.showMessage(title, message,
                             level == "warning" ? QSystemTrayIcon::Warning
                             : level == "error" ? QSystemTrayIcon::Critical
                                                : QSystemTrayIcon::Information,
                             10000);
            return QString();
        };
        controller.trayManager()->setContextMenu(&menu);
        tray.show();
        QObject::connect(&tray, &QSystemTrayIcon::messageClicked, &app, open);
        std::unique_ptr<QWidget> startupNotice(createStartupNotice(open));
        auto syncNoticeLogo = [&] {
            auto *logo =
                static_cast<AnimatedBrandLogo *>(startupNotice->findChild<QWidget *>("animatedPromptLogo"));
            if (logo)
                logo->configure(controller.uiInfo()["animationsEnabled"].toBool(), controller.uiTimings());
        };
        syncNoticeLogo();
        QObject::connect(&controller, &Controller::uiChanged, startupNotice.get(), syncNoticeLogo);
        if (!args.isSet("test-mode")) {
            updater.checkCompletedUpdate();
            if (args.isSet("toast-activated") || !controller.uiInfo()["startMinimised"].toBool() ||
                !QSystemTrayIcon::isSystemTrayAvailable())
                open();
            else if (controller.uiInfo()["startupNotice"].toBool())
                QTimer::singleShot(1200, &app, [window, &startupNotice] {
                    if (window->isVisible())
                        return;
                    startupNotice->show();
                    startupNotice->raise();
                    QTimer::singleShot(10000, startupNotice.get(), &QWidget::hide);
                });
        }
        if (args.isSet("test-mode") && args.isSet("render-preview")) {
            window->setProperty("previewPage", qBound(0, args.value("preview-page").toInt(), 3));
            if (args.value("preview-page") == "8")
                window->setProperty("previewPage", 6);
            if (args.value("preview-page") == "7")
                window->setProperty("previewPage", 5);
            if (args.value("preview-page") == "4") {
                window->setProperty("previewPage", 0);
                QMetaObject::invokeMethod(window, "showSetup");
            }
            if (args.value("preview-page") == "6") {
                window->setProperty("previewPage", 4);
                controller.refreshSpecs();
            }
            if (args.value("preview-page") == "5")
                startupNotice->show();
            else
                open();
            QTimer::singleShot(args.value("preview-page") == "6" ? 12000 : 1500, &app,
                               [window, &app, &args, &startupNotice] {
                                   if (args.value("preview-page") == "5")
                                       startupNotice->grab().save(args.value("render-preview"));
                                   else
                                       window->grabWindow().save(args.value("render-preview"));
                                   app.quit();
                               });
        }
        if (args.isSet("test-mode") && args.isSet("quit-after"))
            QTimer::singleShot(qMax(1, args.value("quit-after").toInt()) * 1000, &app, &QApplication::quit);
        return app.exec();
    } catch (const std::exception &error) {
        if (args.isSet("test-mode")) {
            fprintf(stderr, "%s\n", error.what());
            return 1;
        }
        QMessageBox::critical(nullptr, "WHAT could not start", QString::fromUtf8(error.what()));
        return 1;
    }
}
