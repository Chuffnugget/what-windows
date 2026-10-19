#include "HomeAssistantWebState.h"
#include "BrowserScript.h"
#include "SidebarScript.h"
#include <QDesktopServices>
#include <QUrl>

void HomeAssistantWeb::fail(const QString &message) {
    d->navigationTimeout.stop();
    d->creating = d->loading = d->clearing = false;
    d->message = message;
    d->lastError = message;
    d->failed = true;
    d->stable.stop();
    emit browserChanged();
    emit diagnostic("Home Assistant Web: " + message);
}

QString HomeAssistantWeb::dashboardAddress() const {
    const auto view = d->overlayVisible ? d->overlayMode : d->presentationMode;
    const auto preset = d->options.value(view + "Dashboard").toString();
    // Absolute per-view addresses are intentional destinations. Relative paths
    // follow the selected local/cloud base, without copying credentials or query strings.
    if (d->usingCloud && !preset.isEmpty() && WebOptions::origin(preset) == WebOptions::origin(d->address)) {
        QUrl target(preset), cloud(d->options["cloudUrl"].toString());
        target.setScheme(cloud.scheme());
        target.setHost(cloud.host());
        target.setPort(cloud.port());
        return target.toString();
    }
    return WebOptions::dashboard(d->usingCloud ? d->options["cloudUrl"].toString() : d->address, preset);
}

bool HomeAssistantWeb::internalAddress(const QString &address) const {
    const auto origin = WebOptions::origin(address);
    if (origin.isEmpty())
        return false;
    if (origin == WebOptions::origin(d->address) ||
        origin == WebOptions::origin(d->options["cloudUrl"].toString()))
        return true;
    for (const auto *key : {"desktopDashboard", "fullscreenDashboard", "pipDashboard"})
        if (origin == WebOptions::origin(d->options[key].toString()))
            return true;
    return false;
}

void HomeAssistantWeb::navigateDashboard(bool force) {
    const auto address = dashboardAddress();
    if (!d->web || d->clearing || address.isEmpty() || (!force && d->target == address))
        return;
    d->target = address;
    if (FAILED(d->web->Navigate(address.toStdWString().c_str())))
        fail("The dashboard navigation could not start. Select Reload to retry.");
}

void HomeAssistantWeb::resetRecovery() {
    d->recovery.stop();
    d->stable.stop();
    d->retries = 0;
}

void HomeAssistantWeb::scheduleRecovery() {
    static constexpr int delays[] = {2000, 5000, 15000};
    if (!d->active || d->clearing || !d->options["autoRecover"].toBool() || d->recovery.isActive())
        return;
    if (d->retries >= int(std::size(delays))) {
        d->message += " Automatic retries stopped. Select Reload to try again.";
    } else {
        const int delay = delays[d->retries++];
        d->message += QString(" Retrying in %1 seconds (%2 of 3).").arg(delay / 1000).arg(d->retries);
        d->recovery.start(delay);
    }
    emit browserChanged();
}

void HomeAssistantWeb::navigationFailed(int error, int httpStatus) {
    using E = COREWEBVIEW2_WEB_ERROR_STATUS;
    d->recovery.stop();
    if ((error >= E::COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT &&
         error <= E::COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID)) {
        fail(
            QString(
                "The dashboard certificate was rejected (browser error %1). Check its name, expiry and trust. WHAT will not bypass it or switch addresses.")
                .arg(error));
        return;
    }
    if (error == E::COREWEBVIEW2_WEB_ERROR_STATUS_VALID_AUTHENTICATION_CREDENTIALS_REQUIRED ||
        error == E::COREWEBVIEW2_WEB_ERROR_STATUS_VALID_PROXY_AUTHENTICATION_REQUIRED || httpStatus == 401 ||
        httpStatus == 403) {
        fail(
            "The dashboard or proxy rejected authentication. Check your sign-in and access settings, then select Reload.");
        return;
    }
    const bool transient = error == E::COREWEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE ||
                           error == E::COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT ||
                           error == E::COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED ||
                           error == E::COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET ||
                           error == E::COREWEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED ||
                           error == E::COREWEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT ||
                           error == E::COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED ||
                           httpStatus >= 500;
    fail(
        transient
            ? QString("The dashboard server could not be reached or is unavailable (browser %1, HTTP %2).")
                  .arg(error)
                  .arg(httpStatus)
            : QString(
                  "The dashboard could not load (browser %1, HTTP %2). Check the address, then select Reload.")
                  .arg(error)
                  .arg(httpStatus));
    if (!transient)
        return;
    // Only a failed primary dashboard can trigger fallback, never an external
    // sign-in provider or an explicitly different per-view destination.
    if (!d->usingCloud && d->options["fallback"].toBool() && !d->options["cloudUrl"].toString().isEmpty() &&
        WebOptions::origin(d->source) == WebOptions::origin(d->address) &&
        WebOptions::origin(dashboardAddress()) == WebOptions::origin(d->address)) {
        d->usingCloud = true;
        emit diagnostic(
            "Home Assistant Web: primary address unavailable; trying the configured cloud address.");
        d->message = "Primary address unavailable. Trying Home Assistant Cloud…";
        QTimer::singleShot(0, this, [this] { navigateDashboard(true); });
        emit browserChanged();
    } else {
        scheduleRecovery();
    }
}

void HomeAssistantWeb::startBrowser() {
    if (!d->active || d->creating || d->web || (d->address.isEmpty() && !d->clearing))
        return;
    if (!d->comInitialized) {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
            fail("Could not initialise the browser host.");
            return;
        }
        d->comInitialized = true;
    }
    d->creating = true;
    d->message.clear();
    emit browserChanged();
    if (!d->parkingWindow) {
        d->parkingWindow = CreateWindowExW(0, L"STATIC", L"WHAT Browser Host", WS_POPUP, 0, 0, 1, 1, nullptr,
                                           nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!d->parkingWindow) {
            fail("Could not create the Home Assistant browser host.");
            return;
        }
    }
    const auto generation = ++d->generation;
    const QPointer<HomeAssistantWeb> owner(this);
    const auto profile = d->profile.toStdWString();
    // No waits or event-loop pumping: both callbacks return on this STA.
    const auto result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, profile.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([owner, generation](
                                                                                 HRESULT error,
                                                                                 ICoreWebView2Environment *
                                                                                     environment) -> HRESULT {
            if (!owner || generation != owner->d->generation)
                return S_OK;
            if (FAILED(error) || !environment) {
                owner->fail(
                    "Could not start Microsoft Edge WebView2. Install or repair the WebView2 Runtime, then retry.");
                return S_OK;
            }
            owner->d->environment = environment;
            LPWSTR version = nullptr;
            environment->get_BrowserVersionString(&version);
            owner->d->runtimeVersion = takeWebString(version);
            // The page can change host windows while creation is pending. Use
            // the stable parking HWND, then attach to the current view on completion.
            const auto result = environment->CreateCoreWebView2Controller(
                owner->d->parkingWindow,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [owner, generation](HRESULT error, ICoreWebView2Controller *controller) -> HRESULT {
                        if (!owner || generation != owner->d->generation) {
                            if (controller)
                                controller->Close();
                            return S_OK;
                        }
                        if (FAILED(error) || !controller) {
                            owner->fail("Could not create the Home Assistant browser. Try Reload.");
                            return S_OK;
                        }
                        owner->d->controller = controller;
                        controller->get_CoreWebView2(&owner->d->web);
                        owner->attachBrowser();
                        return S_OK;
                    })
                    .Get());
            if (FAILED(result))
                owner->fail("Could not create the Home Assistant browser. Try Reload.");
            return S_OK;
        }).Get());
    if (FAILED(result))
        fail("Microsoft Edge WebView2 Runtime is unavailable. Install it, then retry.");
}

void HomeAssistantWeb::stopBrowser() {
    d->navigationTimeout.stop();
    d->recovery.stop();
    d->stable.stop();
    ++d->generation; // Ignore callbacks belonging to a closed or replaced session.
    if (d->controller)
        d->controller->Close();
    d->web.Reset();
    d->controller.Reset();
    d->environment.Reset();
    d->browserParent = nullptr;
    d->browserPid = 0;
    d->creating = d->loading = d->loaded = d->clearing = false;
    d->failed = false;
    d->suspendRequested = false;
    d->suspended = d->back = d->forward = false;
    d->signingIn = false;
    d->permissionBusy = false;
    d->permissions.clear();
    d->permissionQueue.clear();
    d->permissionStatus = "Open the dashboard, then refresh to inspect site permissions.";
    d->target.clear();
    d->source.clear();
    d->message.clear();
    emit permissionsChanged();
}

void HomeAssistantWeb::attachBrowser() {
    if (!d->web) {
        fail("WebView2 did not provide a browser instance.");
        return;
    }
    d->creating = false;
    d->web->get_BrowserProcessId(&d->browserPid);
    const QPointer<HomeAssistantWeb> owner(this);
    const auto generation = d->generation;
    auto current = [owner, generation] { return owner && owner->d->generation == generation; };
    EventRegistrationToken token{};
    ComPtr<ICoreWebView2Settings> settings;
    d->web->get_Settings(&settings);
    if (settings) {
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
    }
    d->controller->put_ZoomFactor(d->zoom / 100.0);
    d->web->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [owner, current](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                if (!current())
                    return S_OK;
                LPWSTR value = nullptr;
                args->get_Uri(&value);
                const auto url = takeWebString(value);
                // The dashboard can redirect for login, but cannot open local files
                // or invoke Windows applications through a custom URI scheme.
                if (!HomeAssistantWeb::validUrl(url) || url.isEmpty()) {
                    args->put_Cancel(TRUE);
                    emit owner->notice("This browser only opens HTTP and HTTPS pages.");
                    return S_OK;
                }
                BOOL userInitiated = FALSE, redirected = FALSE;
                args->get_IsUserInitiated(&userInitiated);
                args->get_IsRedirected(&redirected);
                // Keep server-driven sign-in redirects and the login provider's
                // own forms in this session. User links away from it open externally.
                if (!redirected && !owner->d->signingIn && !owner->internalAddress(url) &&
                    WebOptions::origin(url) != WebOptions::origin(owner->d->source) &&
                    url != owner->d->target) {
                    args->put_Cancel(TRUE);
                    if (userInitiated)
                        QTimer::singleShot(0, owner, [url] { QDesktopServices::openUrl(QUrl(url)); });
                    else
                        emit owner->notice("Blocked a script from navigating away from the configured dashboard.");
                    return S_OK;
                }
                if (owner->internalAddress(url))
                    owner->d->signingIn = QUrl(url).path().startsWith("/auth/");
                owner->d->recovery.stop();
                owner->d->source = url;
                owner->d->suspended = false;
                owner->d->loading = true;
                owner->d->loaded = false;
                args->get_NavigationId(&owner->d->navigation);
                owner->d->navigationTimeout.start();
                owner->d->message.clear();
                emit owner->browserChanged();
                return S_OK;
            })
            .Get(),
        &token);
    d->web->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [owner, current](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                if (!current())
                    return S_OK;
                UINT64 navigation = 0;
                args->get_NavigationId(&navigation);
                if (navigation != owner->d->navigation || owner->d->clearing)
                    return S_OK;
                owner->d->navigationTimeout.stop();
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                int httpStatus = 0;
                ComPtr<ICoreWebView2NavigationCompletedEventArgs2> response;
                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&response))))
                    response->get_HttpStatusCode(&httpStatus);
                owner->d->loading = false;
                owner->d->loaded = success;
                if (!success || httpStatus >= 400) {
                    owner->d->loaded = false;
                    COREWEBVIEW2_WEB_ERROR_STATUS error{};
                    args->get_WebErrorStatus(&error);
                    // Stop() and replacement navigations cancel the old request.
                    // Their completion must not replace the new request's status.
                    if (error == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED || owner->d->clearing)
                        return S_OK;
                    owner->navigationFailed(int(error), httpStatus);
                } else {
                    owner->d->recovery.stop();
                    owner->d->failed = false;
                    owner->d->stable.start();
                    emit owner->browserChanged();
                    owner->syncBrowser();
                }
                return S_OK;
            })
            .Get(),
        &token);
    d->controller->add_MoveFocusRequested(
        Callback<ICoreWebView2MoveFocusRequestedEventHandler>(
            [owner, current](ICoreWebView2Controller *,
                             ICoreWebView2MoveFocusRequestedEventArgs *args) -> HRESULT {
                if (!current())
                    return S_OK;
                args->put_Handled(TRUE);
                QTimer::singleShot(0, owner, [owner, current] {
                    if (current())
                        emit owner->focusToolbarRequested();
                });
                return S_OK;
            })
            .Get(),
        &token);
    d->web->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [owner, current](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                args->put_Handled(TRUE);
                if (!current())
                    return S_OK;
                LPWSTR value = nullptr;
                args->get_Uri(&value);
                const auto url = takeWebString(value);
                BOOL userInitiated = FALSE;
                args->get_IsUserInitiated(&userInitiated);
                if (userInitiated && !url.isEmpty() && HomeAssistantWeb::validUrl(url)) {
                    QTimer::singleShot(0, owner, [owner, current, url] {
                        if (!current())
                            return;
                        if (owner->internalAddress(url) || owner->d->signingIn)
                            owner->d->web->Navigate(url.toStdWString().c_str());
                        else
                            QDesktopServices::openUrl(QUrl(url));
                    });
                }
                return S_OK;
            })
            .Get(),
        &token);
    d->web->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>([owner, current](ICoreWebView2 *,
                                                                          ICoreWebView2ProcessFailedEventArgs
                                                                              *args) -> HRESULT {
            if (!current())
                return S_OK;
            COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
            args->get_ProcessFailedKind(&kind);
            emit owner->diagnostic(QString("Home Assistant Web: browser process failure %1").arg(int(kind)));
            // WebView2 restarts auxiliary processes such as the GPU process.
            // Only loss of the browser or main renderer requires replacing it.
            if (kind != COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED &&
                kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED &&
                kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE)
                return S_OK;
            if (current())
                QTimer::singleShot(0, owner, [owner, current] {
                    if (!current())
                        return;
                    owner->stopBrowser();
                    owner->fail("The browser process stopped.");
                    owner->scheduleRecovery();
                });
            return S_OK;
        }).Get(),
        &token);
    d->controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [owner, current](ICoreWebView2Controller *,
                             ICoreWebView2AcceleratorKeyPressedEventArgs *args) -> HRESULT {
                if (!current())
                    return S_OK;
                UINT key = 0;
                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                args->get_VirtualKey(&key);
                args->get_KeyEventKind(&kind);
                if (key == VK_F6 && kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN) {
                    args->put_Handled(TRUE);
                    // Accelerator callbacks are synchronous with the browser. Defer
                    // window changes until it has received our handled response.
                    QTimer::singleShot(0, owner, [owner, current] {
                        if (current())
                            emit owner->focusToolbarRequested();
                    });
                }
                return S_OK;
            })
            .Get(),
        &token);
    d->web->add_HistoryChanged(
        Callback<ICoreWebView2HistoryChangedEventHandler>([owner, current](ICoreWebView2 *web,
                                                                           IUnknown *) -> HRESULT {
            if (!current())
                return S_OK;
            BOOL back = FALSE, forward = FALSE;
            web->get_CanGoBack(&back);
            web->get_CanGoForward(&forward);
            owner->d->back = back;
            owner->d->forward = forward;
            emit owner->browserChanged();
            return S_OK;
        }).Get(),
        &token);
    d->web->add_SourceChanged(
        Callback<ICoreWebView2SourceChangedEventHandler>(
            [owner, current](ICoreWebView2 *web, ICoreWebView2SourceChangedEventArgs *) -> HRESULT {
                if (!current())
                    return S_OK;
                LPWSTR raw = nullptr;
                web->get_Source(&raw);
                const auto source = takeWebString(raw);
                if (!source.isEmpty() && HomeAssistantWeb::validUrl(source)) {
                    owner->d->source = source;
                    if (owner->internalAddress(source))
                        owner->d->signingIn = QUrl(source).path().startsWith("/auth/");
                }
                emit owner->browserChanged();
                return S_OK;
            })
            .Get(),
        &token);
    d->web->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [owner, current](ICoreWebView2 *web, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                if (!current())
                    return S_OK;
                LPWSTR raw = nullptr;
                args->get_Source(&raw);
                const auto origin = WebOptions::origin(takeWebString(raw));
                if (origin.isEmpty() || origin != WebOptions::origin(owner->d->source))
                    return S_OK;
                raw = nullptr;
                if (FAILED(args->TryGetWebMessageAsString(&raw)))
                    return S_OK;
                const auto message = takeWebString(raw);
                BOOL fullscreen = FALSE;
                web->get_ContainsFullScreenElement(&fullscreen);
                if (message == "what:escape" && !fullscreen) {
                    QTimer::singleShot(0, owner, [owner, current] {
                        if (current())
                            owner->hideOverlay();
                    });
                } else if (message == "what:toolbar") {
                    emit owner->revealToolbarRequested();
                } else if (owner->internalAddress(owner->d->source)) {
                    if (message == "what:sidebar-ready") {
                        owner->syncSidebar();
                    } else if (message.startsWith("what:sidebar-width:")) {
                        bool ok = false;
                        const int width = message.mid(19).toInt(&ok);
                        if (ok && width >= 180 && width <= 420 &&
                            width != owner->d->options["sidebarWidth"].toInt()) {
                            owner->d->options["sidebarWidth"] = width;
                            owner->d->settings.setValue("modules/haweb/options", owner->d->options);
                            emit owner->configurationChanged();
                        }
                    }
                }
                return S_OK;
            })
            .Get(),
        &token);
    syncBrowser();
    // Register before the first navigation. Its completion is asynchronous too.
    const auto script = QString::fromWCharArray(BrowserScript::source) +
                        QString::fromWCharArray(SidebarScript::source);
    const auto scriptResult = d->web->AddScriptToExecuteOnDocumentCreated(
        reinterpret_cast<LPCWSTR>(script.utf16()),
        Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([owner, current](
                                                                                       HRESULT error,
                                                                                       LPCWSTR) -> HRESULT {
            if (!current())
                return S_OK;
            if (FAILED(error))
                emit owner->diagnostic(
                    "Home Assistant Web: page keyboard integration unavailable; use the overlay hotkey to close it.");
            if (owner->d->clearing)
                owner->clearBrowsingData();
            else
                owner->navigateDashboard(true);
            return S_OK;
        }).Get());
    if (FAILED(scriptResult)) {
        if (d->clearing)
            clearBrowsingData();
        else
            navigateDashboard(true);
    }
    emit browserChanged();
}

void HomeAssistantWeb::syncSidebar() {
    if (!d->web)
        return;
    const auto message = QString("{\"type\":\"what-sidebar\",\"width\":%1}")
                             .arg(d->options["sidebarWidth"].toInt());
    d->web->PostWebMessageAsJson(reinterpret_cast<LPCWSTR>(message.utf16()));
}

void HomeAssistantWeb::syncBrowser() {
    if (!d->controller || d->replacingSurface)
        return;
    const double zoom = (d->overlayVisible && d->overlayMode == "pip" ? pipScale() : d->zoom) / 100.0;
    double currentZoom = 1;
    if (SUCCEEDED(d->controller->get_ZoomFactor(&currentZoom)) && currentZoom != zoom)
        d->controller->put_ZoomFactor(zoom);
    const auto parent = reinterpret_cast<HWND>(d->window->winId());
    if (parent != d->browserParent) {
        d->controller->put_ParentWindow(parent);
        d->browserParent = parent;
    }
    RECT bounds{};
    GetClientRect(parent, &bounds);
    d->controller->put_Bounds(bounds);
    d->controller->NotifyParentWindowPositionChanged();
    d->controller->put_IsVisible(d->presented);
    const bool keepRunning = d->presented || d->options["keepAudio"].toBool();
    ComPtr<ICoreWebView2_8> audio;
    if (SUCCEEDED(d->web.As(&audio)))
        audio->put_IsMuted(d->options["muted"].toBool() || !keepRunning);
    ComPtr<ICoreWebView2_3> suspend;
    if (SUCCEEDED(d->web.As(&suspend))) {
        if (keepRunning) {
            if (d->suspendRequested)
                suspend->Resume();
            d->suspendRequested = false;
            d->suspended = false;
        } else if (!d->suspendRequested && d->loaded && !d->clearing) {
            d->suspendRequested = true;
            const QPointer<HomeAssistantWeb> owner(this);
            const auto generation = d->generation;
            // A page can be shown again before suspension completes. Resume
            // that same browser, without touching a replacement session.
            const auto result = suspend->TrySuspend(
                Callback<ICoreWebView2TrySuspendCompletedHandler>([owner,
                                                                   generation](HRESULT error,
                                                                               BOOL suspended) -> HRESULT {
                    if (!owner || owner->d->generation != generation)
                        return S_OK;
                    owner->d->suspended = SUCCEEDED(error) && suspended;
                    if (owner->d->presented || owner->d->options["keepAudio"].toBool()) {
                        ComPtr<ICoreWebView2_3> current;
                        if (SUCCEEDED(owner->d->web.As(&current)))
                            current->Resume();
                        owner->d->suspended = false;
                    }
                    emit owner->browserChanged();
                    return S_OK;
                }).Get());
            if (FAILED(result))
                d->suspendRequested = false;
        }
    }
}

void HomeAssistantWeb::setPresented(bool presented) {
    d->presented = presented && d->active;
    if (d->presented)
        startBrowser();
    syncBrowser();
    emit browserChanged();
}

void HomeAssistantWeb::reload() {
    if (!d->active || d->clearing)
        return;
    d->message.clear();
    resetRecovery();
    if (d->usingCloud) {
        d->usingCloud = false;
        navigateDashboard(true);
        if (!d->web)
            startBrowser();
        emit browserChanged();
        return;
    }
    if (d->web)
        d->web->Reload();
    else
        startBrowser();
    emit browserChanged();
}

void HomeAssistantWeb::goHome() {
    if (!d->active || d->clearing)
        return;
    resetRecovery();
    d->usingCloud = false;
    navigateDashboard(true);
}

void HomeAssistantWeb::goBack() {
    if (d->web && !d->clearing)
        d->web->GoBack();
}

void HomeAssistantWeb::goForward() {
    if (d->web && !d->clearing)
        d->web->GoForward();
}

void HomeAssistantWeb::openInBrowser() {
    const auto address = d->source.isEmpty() ? dashboardAddress() : d->source;
    if (!address.isEmpty() && validUrl(address))
        QDesktopServices::openUrl(QUrl(address));
}

void HomeAssistantWeb::toggleMuted() {
    d->options["muted"] = !d->options["muted"].toBool();
    d->settings.setValue("modules/haweb/options", d->options);
    syncBrowser();
    emit configurationChanged();
    emit browserChanged();
}

void HomeAssistantWeb::clearCache(bool includeLogin) {
    if (!d->active || d->clearing)
        return;
    if (d->permissionBusy) {
        emit notice("Wait for the site permission changes to finish, then clear browser data.");
        return;
    }
    d->clearing = true;
    d->navigationTimeout.stop();
    resetRecovery();
    d->clearLogin = includeLogin;
    d->message.clear();
    if (d->web)
        clearBrowsingData();
    else
        startBrowser();
    emit browserChanged();
}

void HomeAssistantWeb::clearBrowsingData() {
    ComPtr<ICoreWebView2_13> web;
    ComPtr<ICoreWebView2Profile> profile;
    ComPtr<ICoreWebView2Profile2> cleaner;
    if (FAILED(d->web.As(&web)) || FAILED(web->get_Profile(&profile)) || FAILED(profile.As(&cleaner))) {
        fail("Update the Microsoft Edge WebView2 Runtime to clear browsing data.");
        return;
    }
    ComPtr<ICoreWebView2_3> suspended;
    if (SUCCEEDED(d->web.As(&suspended)))
        suspended->Resume();
    d->suspendRequested = false;
    d->suspended = false;
    d->web->Stop();
    const auto kinds =
        d->clearLogin ? COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_PROFILE
                      : COREWEBVIEW2_BROWSING_DATA_KINDS(COREWEBVIEW2_BROWSING_DATA_KINDS_DISK_CACHE |
                                                         COREWEBVIEW2_BROWSING_DATA_KINDS_CACHE_STORAGE |
                                                         COREWEBVIEW2_BROWSING_DATA_KINDS_SERVICE_WORKERS);
    const QPointer<HomeAssistantWeb> owner(this);
    const auto generation = d->generation;
    const auto result = cleaner->ClearBrowsingData(
        kinds, Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>([owner, generation](
                                                                            HRESULT error) -> HRESULT {
                   if (!owner || owner->d->generation != generation)
                       return S_OK;
                   owner->d->clearing = false;
                   if (FAILED(error))
                       owner->fail("Browser data could not be cleared. Try again after reloading.");
                   else {
                       emit owner->notice(owner->d->clearLogin
                                              ? "Browser login and site data cleared."
                                              : "Home Assistant Web cache cleared. Your login is kept.");
                       owner->goHome();
                       owner->refreshPermissions();
                       emit owner->browserChanged();
                   }
                   return S_OK;
               }).Get());
    if (FAILED(result))
        fail("Browser data could not be cleared.");
}
