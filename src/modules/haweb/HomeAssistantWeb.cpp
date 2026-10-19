#include "HomeAssistantWebState.h"
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QGuiApplication>
#include <QKeySequence>
#include <QPointer>
#include <QPlatformSurfaceEvent>
#include <QQmlEngine>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QtMath>

namespace {
constexpr int overlayHotkeyId = 0x5748;
constexpr int pipHotkeyId = 0x5749;
constexpr int pipTitleHeight = 44; // 28-pixel toolbar plus the two 8-pixel resize margins.

bool hotkeyParts(const QString &text, UINT &modifiers, UINT &key) {
    if (text.isEmpty()) {
        modifiers = key = 0;
        return true;
    }
    const QKeySequence sequence(text, QKeySequence::PortableText);
    if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown)
        return false;
    const auto combination = sequence[0];
    const auto flags = combination.keyboardModifiers();
    modifiers = MOD_NOREPEAT;
    if (flags & Qt::ControlModifier)
        modifiers |= MOD_CONTROL;
    if (flags & Qt::AltModifier)
        modifiers |= MOD_ALT;
    if (flags & Qt::ShiftModifier)
        modifiers |= MOD_SHIFT;
    if (flags & Qt::MetaModifier)
        modifiers |= MOD_WIN;
    if (!(modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN)))
        return false;
    const int qtKey = combination.key();
    if ((qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) || (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9))
        key = UINT(qtKey);
    else if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24)
        key = VK_F1 + qtKey - Qt::Key_F1;
    else if (qtKey == Qt::Key_Space)
        key = VK_SPACE;
    else
        return false;
    return true;
}

} // namespace

HomeAssistantWeb::HomeAssistantWeb(QSettings &settings, const QString &directory, bool testing,
                                   QObject *parent)
    : QObject(parent), d(std::make_unique<State>(settings)) {
    d->testing = testing;
    d->profile = QDir(directory).absoluteFilePath("home-assistant-web");
    d->active = settings.value("modules/haweb/enabled", false).toBool();
    d->address = settings.value("modules/haweb/url").toString();
    if (!validUrl(d->address))
        d->address.clear();
    d->viewMode = settings.value("modules/haweb/mode", "windowed").toString();
    if (!validMode(d->viewMode))
        d->viewMode = "windowed";
    d->presentationMode = d->viewMode;
    d->zoom = qBound(50, settings.value("modules/haweb/scale", 100).toInt(), 200);
    d->options = WebOptions::merged(settings.value("modules/haweb/options").toMap());
    d->shortcut = settings.value("modules/haweb/hotkey", "Ctrl+Alt+H").toString();
    if (!validHotkey(d->shortcut))
        d->shortcut.clear();
    QQmlEngine::setObjectOwnership(d->window.get(), QQmlEngine::CppOwnership);
    d->window->installEventFilter(this);
    QCoreApplication::instance()->installNativeEventFilter(this);
    d->saveGeometry.setSingleShot(true);
    d->saveGeometry.setInterval(400);
    connect(&d->saveGeometry, &QTimer::timeout, this, &HomeAssistantWeb::savePictureInPicture);
    d->recovery.setSingleShot(true);
    d->stable.setSingleShot(true);
    d->stable.setInterval(60000);
    d->navigationTimeout.setSingleShot(true);
    d->navigationTimeout.setInterval(30000);
    connect(&d->navigationTimeout, &QTimer::timeout, this, [this] {
        if (!d->web || !d->loading || d->clearing)
            return;
        d->navigation = 0; // Ignore completion of the request we are cancelling.
        d->web->Stop();
        navigationFailed(COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT, 0);
    });
    connect(&d->stable, &QTimer::timeout, this, [this] { d->retries = 0; });
    connect(&d->recovery, &QTimer::timeout, this, [this] {
        if (!d->active || d->clearing)
            return;
        if (d->web)
            navigateDashboard(true);
        else
            startBrowser();
    });
    auto watchScreen = [this](QScreen *screen) {
        connect(screen, &QScreen::availableGeometryChanged, this, &HomeAssistantWeb::updateWindowGeometry);
        connect(screen, &QScreen::geometryChanged, this, &HomeAssistantWeb::updateWindowGeometry);
    };
    for (auto *screen : QGuiApplication::screens())
        watchScreen(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this, watchScreen](QScreen *screen) {
        watchScreen(screen);
        updateWindowGeometry();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this,
            [this] { QTimer::singleShot(0, this, &HomeAssistantWeb::updateWindowGeometry); });
    registerHotkey();
}

HomeAssistantWeb::~HomeAssistantWeb() {
    savePictureInPicture();
    d->desktopPlacement.detach();
    QCoreApplication::instance()->removeNativeEventFilter(this);
    if (d->registered)
        UnregisterHotKey(nullptr, overlayHotkeyId);
    if (d->pipRegistered)
        UnregisterHotKey(nullptr, pipHotkeyId);
    stopBrowser();
    if (d->parkingWindow)
        DestroyWindow(d->parkingWindow);
    if (d->comInitialized)
        CoUninitialize();
}

bool HomeAssistantWeb::enabled() const {
    return d->active;
}

QString HomeAssistantWeb::url() const {
    return d->address;
}

QString HomeAssistantWeb::mode() const {
    return d->viewMode;
}

int HomeAssistantWeb::scale() const {
    return d->zoom;
}

QString HomeAssistantWeb::hotkey() const {
    return d->shortcut;
}

QString HomeAssistantWeb::hotkeyError() const {
    return d->shortcutError;
}

QString HomeAssistantWeb::pipHotkeyError() const {
    return d->pipShortcutError;
}

bool HomeAssistantWeb::pipMinimised() const {
    return d->pipMinimised;
}

QStringList HomeAssistantWeb::issues() const {
    QStringList result;
    if (!d->active)
        return result;
    if (d->address.isEmpty())
        result.append("Enter your Home Assistant dashboard address in the module settings.");
    if (!d->shortcutError.isEmpty())
        result.append(d->shortcutError);
    if (!d->pipShortcutError.isEmpty())
        result.append(d->pipShortcutError);
    // Keep a real failure visible during recovery, but an unopened browser is healthy.
    if (d->failed)
        result.append(d->message.isEmpty() ? d->lastError : d->message);
    return result;
}

bool HomeAssistantWeb::loading() const {
    return d->creating || d->loading;
}

bool HomeAssistantWeb::ready() const {
    return d->loaded;
}

bool HomeAssistantWeb::cacheBusy() const {
    return d->clearing;
}

bool HomeAssistantWeb::overlayVisible() const {
    return d->overlayVisible;
}

QString HomeAssistantWeb::overlayMode() const {
    return d->overlayMode;
}

bool HomeAssistantWeb::desktopActive() const {
    return d->desktopActive;
}

QWindow *HomeAssistantWeb::browserWindow() const {
    return d->window.get();
}

QString HomeAssistantWeb::status() const {
    if (!d->active)
        return "Disabled";
    if (!d->message.isEmpty())
        return d->message;
    if (d->clearing)
        return "Clearing browser data…";
    if (d->address.isEmpty())
        return "Enter your Home Assistant dashboard address";
    if (loading())
        return "Loading Home Assistant…";
    return d->loaded ? "Ready" : "Ready to open";
}

bool HomeAssistantWeb::validUrl(const QString &text) {
    return WebOptions::validUrl(text);
}

QVariantMap HomeAssistantWeb::options() const {
    return d->options;
}

QVariantList HomeAssistantWeb::monitors() const {
    QVariantList result{QVariantMap{{"name", ""}, {"label", "Automatic"}}};
    QStringList names{QString()};
    for (const auto *screen : QGuiApplication::screens()) {
        names.append(screen->name());
        result.append(QVariantMap{{"name", screen->name()},
                                  {"label", QString("%1 (%2 x %3)")
                                                .arg(screen->name())
                                                .arg(screen->size().width())
                                                .arg(screen->size().height())}});
    }
    for (const auto *key : {"desktopMonitor", "overlayMonitor"}) {
        const auto saved = d->options[key].toString();
        if (!names.contains(saved)) {
            result.append(QVariantMap{{"name", saved}, {"label", "Unavailable: " + saved}});
            names.append(saved);
        }
    }
    return result;
}

QVariantMap HomeAssistantWeb::diagnostics() const {
    return {{"enabled", d->active},
            {"runtime", d->runtimeVersion.isEmpty() ? "Not started" : d->runtimeVersion},
            {"host", QUrl(d->source).host()},
            {"route", d->usingCloud ? "Cloud fallback" : "Primary address"},
            {"mode", d->overlayVisible ? d->overlayMode : d->presentationMode},
            {"state", !d->web        ? (d->creating ? "Starting" : "Stopped")
                      : d->presented ? "Visible"
                      : d->suspended ? "Suspended"
                                     : "Hidden"},
            {"lastError", d->lastError},
            {"retries", d->retries},
            {"browserPid", d->browserPid},
            {"retryPending", d->recovery.isActive()},
            {"muted", d->options["muted"].toBool() || (!d->presented && !d->options["keepAudio"].toBool())}};
}

bool HomeAssistantWeb::canGoBack() const {
    return d->back;
}

bool HomeAssistantWeb::canGoForward() const {
    return d->forward;
}

bool HomeAssistantWeb::validHotkey(const QString &text) {
    return WebOptions::validHotkey(text);
}

bool HomeAssistantWeb::validMode(const QString &mode) {
    return mode == "windowed" || mode == "fullscreen" || mode == "desktop" || mode == "pip";
}

void HomeAssistantWeb::registerHotkey() {
    // Release both first, so swapping their combinations works in a single Apply.
    if (d->registered)
        UnregisterHotKey(nullptr, overlayHotkeyId);
    if (d->pipRegistered)
        UnregisterHotKey(nullptr, pipHotkeyId);
    auto claim = [this](int id, const QString &shortcut, const QString &label,
                        bool &registered, QString &error) {
        registered = false;
        error.clear();
        if (d->testing || !d->active || shortcut.isEmpty())
            return;
        UINT modifiers = 0, key = 0;
        if (hotkeyParts(shortcut, modifiers, key))
            registered = RegisterHotKey(nullptr, id, modifiers, key);
        if (!registered)
            error = label + " hotkey is unavailable or already used by another app. Choose a different combination.";
    };
    claim(overlayHotkeyId, d->shortcut, "Overlay", d->registered, d->shortcutError);
    claim(pipHotkeyId, d->options["pipHotkey"].toString(), "Picture-in-picture",
          d->pipRegistered, d->pipShortcutError);
}

void HomeAssistantWeb::setEnabled(bool enabled) {
    if (d->active == enabled)
        return;
    d->active = enabled;
    d->settings.setValue("modules/haweb/enabled", enabled);
    if (!enabled) {
        resetRecovery();
        hideOverlay();
        stopBrowser();
    }
    registerHotkey();
    syncDesktop();
    emit configurationChanged();
    emit browserChanged();
}

void HomeAssistantWeb::setScale(int percent) {
    percent = qBound(50, percent, 200);
    if (d->zoom == percent)
        return;
    const int previous = d->zoom;
    // Native callbacks can synchronise the window during put_ZoomFactor.
    // Publish the desired value first so they cannot restore the old zoom.
    d->zoom = percent;
    // Zoom the existing session without navigating or resetting its recovery state.
    if (d->controller && !(d->overlayVisible && d->overlayMode == "pip") &&
        FAILED(d->controller->put_ZoomFactor(percent / 100.0))) {
        d->zoom = previous;
        emit notice("The browser could not change its render scale. Try reloading the dashboard.");
        return;
    }
    d->settings.setValue("modules/haweb/scale", percent);
    emit scaleChanged();
}

int HomeAssistantWeb::pipScale() const {
    return d->options["pipScale"].toInt();
}

void HomeAssistantWeb::setPipScale(int percent) {
    percent = qBound(50, percent, 200);
    if (pipScale() == percent)
        return;
    d->options["pipScale"] = percent;
    d->settings.setValue("modules/haweb/options", d->options);
    syncBrowser();
    emit configurationChanged();
}

bool HomeAssistantWeb::configure(const QString &address, const QString &mode, int scale,
                                 const QString &shortcut, const QVariantMap &options) {
    if (d->clearing)
        return false;
    const auto cleanUrl = address.trimmed();
    const auto cleanKey = shortcut.trimmed();
    if (!validUrl(cleanUrl)) {
        emit notice("Use an http:// or https:// dashboard address without an embedded username or password.");
        return false;
    }
    if (!validHotkey(cleanKey)) {
        emit notice(
            "Use Ctrl, Alt or Windows with a letter, number, function key or Space. Leave it blank to disable the hotkey.");
        return false;
    }
    if (!validMode(mode) || scale < 50 || scale > 200)
        return false;
    if (!WebOptions::valid(options)) {
        emit notice(
            "Check the advanced Web settings. Cloud fallback needs HTTPS; dashboard overrides need an HTTP(S) address or a path starting with /.");
        return false;
    }
    const auto nextOptions = options.isEmpty() ? d->options : WebOptions::merged(options);
    const auto pipKey = nextOptions["pipHotkey"].toString();
    if (!pipKey.isEmpty() && QKeySequence(pipKey) == QKeySequence(cleanKey)) {
        emit notice("Choose different shortcuts for the overlay and picture-in-picture.");
        return false;
    }
    const bool routeChanged = d->options["cloudUrl"] != nextOptions["cloudUrl"] ||
                              d->options["fallback"] != nextOptions["fallback"];
    const bool sizeChanged = d->options["pipWindowScale"] != nextOptions["pipWindowScale"];
    d->options = nextOptions;
    d->settings.setValue("modules/haweb/options", d->options);
    resetRecovery();
    if (routeChanged || d->address != cleanUrl)
        d->usingCloud = false;
    const bool navigate = d->address != cleanUrl;
    d->address = cleanUrl;
    const bool modeChanged = d->viewMode != mode;
    d->viewMode = mode;
    if (modeChanged)
        d->presentationMode = mode;
    setScale(scale);
    d->shortcut = QKeySequence(cleanKey, QKeySequence::PortableText).toString(QKeySequence::PortableText);
    d->settings.setValue("modules/haweb/url", d->address);
    d->settings.setValue("modules/haweb/mode", d->viewMode);
    d->settings.setValue("modules/haweb/hotkey", d->shortcut);
    registerHotkey();
    if (modeChanged || cleanUrl.isEmpty())
        hideOverlay();
    syncDesktop();
    if (navigate) {
        stopBrowser();
        if (d->presented)
            startBrowser();
    }
    navigateDashboard();
    updateWindowGeometry();
    if (sizeChanged)
        applyPipWindowScale();
    syncBrowser();
    syncSidebar();
    emit configurationChanged();
    emit browserChanged();
    return true;
}

void HomeAssistantWeb::setOverlayWindow(QWindow *window) {
    if (d->overlay) {
        savePictureInPicture();
        d->overlay->removeEventFilter(this);
    }
    d->overlay = window;
    if (window)
        window->installEventFilter(this);
}

void HomeAssistantWeb::setDesktopWindow(QWindow *window) {
    d->desktopPlacement.detach();
    d->desktop = window;
    syncDesktop();
}

QRect HomeAssistantWeb::fitPictureInPicture(QRect bounds, const QRect &workArea) {
    if (workArea.isEmpty())
        return bounds;
    if (!bounds.isValid()) {
        const QSize size(qMin(640, workArea.width()), qMin(480, workArea.height()));
        bounds = QRect(workArea.bottomRight() - QPoint(size.width() + 15, size.height() + 15), size);
    }
    bounds.setWidth(qMin(workArea.width(), qMax(360, bounds.width())));
    bounds.setHeight(qMin(workArea.height(), qMax(260, bounds.height())));
    bounds.moveLeft(qBound(workArea.left(), bounds.x(), workArea.right() - bounds.width() + 1));
    bounds.moveTop(qBound(workArea.top(), bounds.y(), workArea.bottom() - bounds.height() + 1));
    return bounds;
}

QRect HomeAssistantWeb::snapPictureInPicture(QRect bounds, const QRect &workArea, int distance) {
    bounds = fitPictureInPicture(bounds, workArea);
    if (qAbs(bounds.left() - workArea.left()) <= distance)
        bounds.moveLeft(workArea.left());
    else if (qAbs(bounds.right() - workArea.right()) <= distance)
        bounds.moveRight(workArea.right());
    if (qAbs(bounds.top() - workArea.top()) <= distance)
        bounds.moveTop(workArea.top());
    else if (qAbs(bounds.bottom() - workArea.bottom()) <= distance)
        bounds.moveBottom(workArea.bottom());
    return bounds;
}

QRect HomeAssistantWeb::resizePictureInPicture(QRect bounds, double ratio, int edges, QSize minimum) {
    if (ratio <= 0)
        return bounds;
    // Keep the opposite edge fixed while Windows sizes the native PiP window.
    const auto anchor = bounds.bottomRight();
    int width = bounds.width();
    if (!(edges & (Qt::LeftEdge | Qt::RightEdge)))
        width = qRound(bounds.height() * ratio);
    width = qMax(width, qMax(minimum.width(), qCeil(minimum.height() * ratio)));
    bounds.setSize(QSize(width, qRound(width / ratio)));
    if (edges & Qt::LeftEdge)
        bounds.moveRight(anchor.x());
    if (edges & Qt::TopEdge)
        bounds.moveBottom(anchor.y());
    return bounds;
}

void HomeAssistantWeb::savePictureInPicture() {
    d->saveGeometry.stop();
    if (!d->overlayVisible || d->overlayMode != "pip" || !d->overlay)
        return;
    auto bounds = d->overlay->geometry();
    // Moving the rolled-up title bar changes position, never the saved full height.
    if (d->pipMinimised) {
        if (bounds.topLeft() == d->pipDockedPosition)
            bounds = d->pipRestoreBounds;
        else
            bounds.setSize(d->pipRestoreBounds.size());
    }
    const QVariantList values{bounds.x(), bounds.y(), bounds.width(), bounds.height()};
    if (d->settings.value("modules/haweb/pip_bounds").toList() != values) {
        d->settings.setValue("modules/haweb/pip_bounds", values);
        emit configurationChanged();
    }
}

void HomeAssistantWeb::applyPipWindowScale() {
    // A percentage always starts from 640x480, never from an already scaled
    // size. Manual resizing remains saved until this setting changes again.
    const auto saved = d->settings.value("modules/haweb/pip_bounds").toList();
    auto *screen = d->overlay ? d->overlay->screen() : QGuiApplication::primaryScreen();
    if (!screen)
        return;
    QRect bounds = fitPictureInPicture({}, screen->availableGeometry());
    if (d->overlayVisible && d->overlayMode == "pip" && d->overlay)
        bounds = d->overlay->geometry();
    else if (saved.size() == 4)
        bounds.moveTopLeft(QPoint(qBound(-1000000, saved[0].toInt(), 1000000),
                                 qBound(-1000000, saved[1].toInt(), 1000000)));
    const int percent = d->options["pipWindowScale"].toInt();
    bounds.setSize(QSize(640 * percent / 100, 480 * percent / 100));
    const auto aspect = d->options["pipAspectRatio"].toString();
    const double ratio = aspect == "16:9" ? 16.0 / 9 : aspect == "4:3" ? 4.0 / 3 : aspect == "1:1" ? 1 : 0;
    bounds = resizePictureInPicture(bounds, ratio, Qt::RightEdge | Qt::BottomEdge, QSize(360, 260));
    bounds = fitPictureInPicture(bounds, screen->availableGeometry());
    d->settings.setValue("modules/haweb/pip_bounds",
                         QVariantList{bounds.x(), bounds.y(), bounds.width(), bounds.height()});
    if (d->overlayVisible && d->overlayMode == "pip" && d->overlay) {
        if (d->pipMinimised) {
            d->pipRestoreBounds = bounds;
            bounds.moveTopLeft(d->overlay->position());
            bounds.setHeight(pipTitleHeight);
        }
        d->overlay->setGeometry(bounds);
    }
}

void HomeAssistantWeb::updateWindowGeometry() {
    emit monitorsChanged();
    if (d->desktopActive)
        syncDesktop();
    if (!d->overlayVisible || !d->overlay)
        return;
    auto *screen = d->overlay->screen();
    for (auto *candidate : QGuiApplication::screens())
        if (candidate->name() == d->options["overlayMonitor"].toString())
            screen = candidate;
    if (!screen || !QGuiApplication::screens().contains(screen))
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    d->overlay->setScreen(screen);
    auto bounds = d->overlay->geometry();
    if (d->pipMinimised) {
        const auto workArea = screen->availableGeometry();
        const bool docked = bounds.topLeft() == d->pipDockedPosition;
        d->pipRestoreBounds = fitPictureInPicture(d->pipRestoreBounds, workArea);
        bounds.setWidth(qMin(bounds.width(), workArea.width()));
        bounds.moveLeft(qBound(workArea.left(), bounds.left(), workArea.right() - bounds.width() + 1));
        bounds.moveTop(qBound(workArea.top(), bounds.top(), workArea.bottom() - bounds.height() + 1));
        if (docked) {
            bounds.moveBottom(workArea.bottom());
            d->pipDockedPosition = bounds.topLeft();
        }
    } else
        bounds = d->overlayMode == "pip" ? fitPictureInPicture(bounds, screen->availableGeometry())
                                         : screen->geometry();
    d->overlay->setGeometry(bounds);
}

void HomeAssistantWeb::syncDesktop() {
    const bool active = d->desktop && d->active && d->presentationMode == "desktop" && !d->address.isEmpty();
    const bool changed = active != d->desktopActive;
    d->desktopActive = active;
    if (changed)
        emit overlayChanged();
    if (!active || d->overlayVisible) {
        d->desktopPlacement.detach();
        if (d->desktop)
            d->desktop->hide();
        return;
    }
    const auto preferred = d->options["desktopMonitor"].toString();
    const auto name =
        preferred.isEmpty() ? d->settings.value("modules/haweb/desktop_screen").toString() : preferred;
    auto *screen = QGuiApplication::primaryScreen();
    for (auto *candidate : QGuiApplication::screens())
        if (candidate->name() == name)
            screen = candidate;
    if (!screen)
        return;
    d->desktop->setScreen(screen);
    d->desktop->setGeometry(screen->availableGeometry());
    d->desktopPlacement.attach(d->desktop);
    d->desktop->show();
}

void HomeAssistantWeb::toggleOverlay() {
    if (d->overlayVisible) {
        hideOverlay();
        return;
    }
    showOverlay(d->viewMode == "pip" ? "pip" : "fullscreen");
}

void HomeAssistantWeb::togglePictureInPicture() {
    if (d->overlayVisible && d->overlayMode == "pip")
        hideOverlay();
    else
        showOverlay("pip");
}

void HomeAssistantWeb::togglePipMinimised() {
    if (!d->overlayVisible || d->overlayMode != "pip" || !d->overlay)
        return;
    auto bounds = d->overlay->geometry();
    if (!d->pipMinimised) {
        savePictureInPicture();
        d->pipRestoreBounds = bounds;
        d->pipRestoreMaximumHeight = d->overlay->maximumHeight();
        d->pipMinimised = true;
        // QML hides the native browser before shrinking its host window.
        emit overlayChanged();
        d->overlay->setMinimumSize(QSize(360, pipTitleHeight));
        d->overlay->setMaximumHeight(pipTitleHeight);
        bounds.setHeight(pipTitleHeight);
        // availableGeometry excludes a bottom taskbar. A top/side taskbar
        // leaves its bottom at the screen edge. Dock once, then allow dragging.
        if (auto *screen = d->overlay->screen())
            bounds.moveBottom(screen->availableGeometry().bottom());
        d->pipDockedPosition = bounds.topLeft();
    } else {
        if (bounds.topLeft() == d->pipDockedPosition)
            bounds = d->pipRestoreBounds;
        else
            bounds.setSize(d->pipRestoreBounds.size());
        if (auto *screen = d->overlay->screen())
            bounds = fitPictureInPicture(bounds, screen->availableGeometry());
        d->overlay->setMaximumHeight(d->pipRestoreMaximumHeight);
        d->overlay->setMinimumSize(QSize(360, 260));
        d->pipMinimised = false;
    }
    d->overlay->setGeometry(bounds);
    emit overlayChanged();
}

void HomeAssistantWeb::showOverlay(const QString &mode) {
    if (mode != "pip" && mode != "fullscreen")
        return;
    if (!d->active || d->address.isEmpty() || !d->overlay) {
        emit pageRequested();
        return;
    }
    savePictureInPicture();
    if (d->pipMinimised)
        d->overlay->setMaximumHeight(d->pipRestoreMaximumHeight);
    d->pipMinimised = false;
    if (!d->overlayVisible)
        d->previousForeground = GetForegroundWindow();
    d->overlayMode = mode;
    auto *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    bool preferred = false;
    for (auto *candidate : QGuiApplication::screens())
        if (candidate->name() == d->options["overlayMonitor"].toString()) {
            screen = candidate;
            preferred = true;
        }
    if (!screen)
        return;
    QRect bounds = screen->geometry();
    if (mode == "pip") {
        if (!d->settings.contains("modules/haweb/pip_bounds"))
            applyPipWindowScale();
        const auto saved = d->settings.value("modules/haweb/pip_bounds").toList();
        QRect previous;
        if (saved.size() == 4) {
            // Reject damaged geometry before constructing a QRect, whose stored
            // right/bottom coordinates could otherwise overflow.
            const auto x = saved[0].toLongLong(), y = saved[1].toLongLong();
            const auto width = saved[2].toLongLong(), height = saved[3].toLongLong();
            if (x >= -1000000 && x <= 1000000 && y >= -1000000 && y <= 1000000 && width > 0 &&
                width <= 32768 && height > 0 && height <= 32768)
                previous = QRect(int(x), int(y), int(width), int(height));
        }
        if (auto *previousScreen = QGuiApplication::screenAt(previous.center());
            previous.isValid() && previousScreen && !preferred)
            screen = previousScreen;
        bounds = fitPictureInPicture(previous, screen->availableGeometry());
    }
    d->overlay->setScreen(screen);
    d->overlay->setMinimumSize(mode == "pip" ? QSize(360, 260) : QSize());
    d->overlay->setGeometry(bounds);
    d->overlayVisible = true;
    emit overlayChanged();
    navigateDashboard();
    syncBrowser();
    // QML may reject opening over an existing modal prompt.
    if (!d->overlayVisible)
        return;
    syncDesktop();
    d->overlay->show();
    d->overlay->raise();
    d->overlay->requestActivate();
    if (d->controller)
        d->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void HomeAssistantWeb::hideOverlay() {
    if (!d->overlayVisible)
        return;
    savePictureInPicture();
    const auto foreground = GetForegroundWindow();
    const bool restore =
        d->overlay && GetAncestor(foreground, GA_ROOT) == reinterpret_cast<HWND>(d->overlay->winId());
    d->overlayVisible = false;
    if (d->overlay)
        d->overlay->hide();
    emit overlayChanged();
    syncDesktop();
    navigateDashboard();
    syncBrowser();
    if (restore && IsWindow(d->previousForeground))
        SetForegroundWindow(d->previousForeground);
}

void HomeAssistantWeb::open() {
    openView(d->viewMode);
}

void HomeAssistantWeb::openView(const QString &mode) {
    if (!validMode(mode))
        return;
    if (!d->active || d->address.isEmpty()) {
        emit pageRequested();
        return;
    }
    if (mode == "desktop") {
        d->presentationMode = mode;
        if (auto *screen = QGuiApplication::screenAt(QCursor::pos())) {
            d->settings.setValue("modules/haweb/desktop_screen", screen->name());
            emit configurationChanged();
        }
        hideOverlay();
        syncDesktop();
        navigateDashboard();
        emit notice("Home Assistant is on your desktop. Press Win+D to view it.");
    } else if (mode == "fullscreen" || mode == "pip") {
        if (d->overlayVisible && d->overlayMode == mode)
            hideOverlay();
        else
            showOverlay(mode);
    } else {
        d->presentationMode = "windowed";
        hideOverlay();
        syncDesktop();
        navigateDashboard();
        emit pageRequested();
    }
}

bool HomeAssistantWeb::nativeEventFilter(const QByteArray &, void *raw, qintptr *result) {
    const auto *message = static_cast<MSG *>(raw);
    if ((message->message == WM_EXITSIZEMOVE || message->message == WM_SIZING) && d->overlayVisible &&
        d->overlayMode == "pip" && d->overlay && d->overlay->handle() &&
        message->hwnd == reinterpret_cast<HWND>(d->overlay->winId())) {
        if (message->message == WM_EXITSIZEMOVE && !d->pipMinimised && d->options["pipSnap"].toBool()) {
            const auto screen = d->overlay->screen();
            if (screen)
                d->overlay->setGeometry(
                    snapPictureInPicture(d->overlay->geometry(), screen->availableGeometry()));
        } else if (message->message == WM_SIZING && !d->pipMinimised) {
            const auto ratioName = d->options["pipAspectRatio"].toString();
            const double ratio = ratioName == "16:9"  ? 16.0 / 9
                                 : ratioName == "4:3" ? 4.0 / 3
                                 : ratioName == "1:1" ? 1.0
                                                      : 0;
            if (ratio > 0) {
                const int edges[] = {0,
                                     Qt::LeftEdge,
                                     Qt::RightEdge,
                                     Qt::TopEdge,
                                     Qt::TopEdge | Qt::LeftEdge,
                                     Qt::TopEdge | Qt::RightEdge,
                                     Qt::BottomEdge,
                                     Qt::BottomEdge | Qt::LeftEdge,
                                     Qt::BottomEdge | Qt::RightEdge};
                const auto edge = message->wParam;
                auto *native = reinterpret_cast<RECT *>(message->lParam);
                if (native && edge < std::size(edges)) {
                    const auto ratioBounds =
                        resizePictureInPicture(QRect(native->left, native->top, native->right - native->left,
                                                     native->bottom - native->top),
                                               ratio, edges[edge],
                                               QSize(qRound(360 * d->overlay->devicePixelRatio()),
                                                     qRound(260 * d->overlay->devicePixelRatio())));
                    *native = {ratioBounds.left(), ratioBounds.top(), ratioBounds.right() + 1,
                               ratioBounds.bottom() + 1};
                    if (result)
                        *result = TRUE;
                    return true;
                }
            }
        }
    }
    if (message->message != WM_HOTKEY)
        return false;
    if (message->wParam == overlayHotkeyId && d->registered)
        toggleOverlay();
    else if (message->wParam == pipHotkeyId && d->pipRegistered)
        togglePictureInPicture();
    else
        return false;
    // Thread messages (including RegisterHotKey's WM_HOTKEY) have no native
    // window procedure result in Qt's dispatcher.
    if (result)
        *result = 0;
    return true;
}

bool HomeAssistantWeb::eventFilter(QObject *object, QEvent *event) {
    if (object == d->overlay && d->overlayVisible && d->overlayMode == "pip" &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        d->saveGeometry.start();
    if (object == d->window.get()) {
        if (event->type() == QEvent::PlatformSurface) {
            const auto *surface = static_cast<QPlatformSurfaceEvent *>(event);
            d->replacingSurface =
                surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed;
            if (d->replacingSurface && d->controller) {
                // WindowContainer can destroy/recreate its HWND when moved to
                // another Qt window. Park WebView2 first so Windows does not
                // destroy its child windows along with the old host.
                d->controller->put_IsVisible(FALSE);
                d->controller->put_ParentWindow(d->parkingWindow);
                d->browserParent = d->parkingWindow;
            } else if (!d->replacingSurface) {
                QTimer::singleShot(0, this, &HomeAssistantWeb::syncBrowser);
            }
        } else if (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
                   event->type() == QEvent::DevicePixelRatioChange)
            syncBrowser();
        else if (event->type() == QEvent::FocusIn && d->controller)
            d->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    return QObject::eventFilter(object, event);
}
