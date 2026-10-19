#pragma once
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QSettings>
#include <QWindow>
#include <memory>

// Owns one browser session shared by the WHAT page, desktop and overlays.
// WebView2's host API runs on the GUI STA; rendering and networking run in
// WebView2 processes. Creation and browsing-data removal are asynchronous.
class HomeAssistantWeb final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY configurationChanged)
    Q_PROPERTY(QString url READ url NOTIFY configurationChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY configurationChanged)
    Q_PROPERTY(int scale READ scale WRITE setScale NOTIFY scaleChanged)
    Q_PROPERTY(int pipScale READ pipScale WRITE setPipScale NOTIFY configurationChanged)
    Q_PROPERTY(QString hotkey READ hotkey NOTIFY configurationChanged)
    Q_PROPERTY(QString hotkeyError READ hotkeyError NOTIFY configurationChanged)
    Q_PROPERTY(QString pipHotkeyError READ pipHotkeyError NOTIFY configurationChanged)
    Q_PROPERTY(bool pipMinimised READ pipMinimised NOTIFY overlayChanged)
    Q_PROPERTY(QString status READ status NOTIFY browserChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY browserChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY browserChanged)
    Q_PROPERTY(bool cacheBusy READ cacheBusy NOTIFY browserChanged)
    Q_PROPERTY(bool overlayVisible READ overlayVisible NOTIFY overlayChanged)
    Q_PROPERTY(QString overlayMode READ overlayMode NOTIFY overlayChanged)
    Q_PROPERTY(bool desktopActive READ desktopActive NOTIFY overlayChanged)
    Q_PROPERTY(QWindow *browserWindow READ browserWindow CONSTANT)
    Q_PROPERTY(QVariantMap options READ options NOTIFY configurationChanged)
    Q_PROPERTY(QVariantList monitors READ monitors NOTIFY monitorsChanged)
    Q_PROPERTY(QVariantMap diagnostics READ diagnostics NOTIFY browserChanged)
    Q_PROPERTY(QVariantList permissions READ permissions NOTIFY permissionsChanged)
    Q_PROPERTY(bool permissionsBusy READ permissionsBusy NOTIFY permissionsChanged)
    Q_PROPERTY(QString permissionsStatus READ permissionsStatus NOTIFY permissionsChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY browserChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY browserChanged)
public:
    HomeAssistantWeb(QSettings &settings, const QString &directory, bool testing, QObject *parent = nullptr);
    ~HomeAssistantWeb() override;
    bool enabled() const;
    QString url() const;
    QString mode() const;
    int scale() const;
    QString hotkey() const;
    QString hotkeyError() const;
    QString pipHotkeyError() const;
    bool pipMinimised() const;
    QString status() const;
    QStringList issues() const;
    bool loading() const;
    bool ready() const;
    bool cacheBusy() const;
    bool overlayVisible() const;
    QString overlayMode() const;
    bool desktopActive() const;
    QWindow *browserWindow() const;
    QVariantMap options() const;
    QVariantList monitors() const;
    QVariantMap diagnostics() const;
    QVariantList permissions() const;
    bool permissionsBusy() const;
    QString permissionsStatus() const;
    bool canGoBack() const;
    bool canGoForward() const;
    void setEnabled(bool enabled);
    void setScale(int percent);
    int pipScale() const;
    void setPipScale(int percent);
    Q_INVOKABLE bool configure(const QString &url, const QString &mode, int scale, const QString &hotkey,
                               const QVariantMap &options = {});
    // The host calls this when its native surface becomes visible or hidden.
    // Hidden views can suspend unless background audio is enabled.
    Q_INVOKABLE void setPresented(bool presented);
    Q_INVOKABLE void reload();
    Q_INVOKABLE void goHome();
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void goForward();
    Q_INVOKABLE void openInBrowser();
    Q_INVOKABLE void toggleMuted();
    Q_INVOKABLE void refreshPermissions();
    Q_INVOKABLE void setPermission(const QString &origin, int kind, int state);
    Q_INVOKABLE void resetPermissions();
    // Asynchronous. includeLogin also removes cookies and site storage; the
    // default keeps sign-in data. Neither option changes WHAT's pairing identity.
    Q_INVOKABLE void clearCache(bool includeLogin = false);
    Q_INVOKABLE void setOverlayWindow(QWindow *window);
    Q_INVOKABLE void setDesktopWindow(QWindow *window);
    Q_INVOKABLE void showOverlay(const QString &mode);
    Q_INVOKABLE void toggleOverlay();
    Q_INVOKABLE void togglePictureInPicture();
    Q_INVOKABLE void togglePipMinimised();
    // Tray actions choose a view for this session without changing the startup mode.
    Q_INVOKABLE void openView(const QString &mode);
    Q_INVOKABLE void hideOverlay();
    Q_INVOKABLE void open();
    static bool validUrl(const QString &text);
    static bool validHotkey(const QString &text);
    static bool validMode(const QString &mode);
    static QRect fitPictureInPicture(QRect bounds, const QRect &workArea);
    static QRect snapPictureInPicture(QRect bounds, const QRect &workArea, int distance = 16);
    static QRect resizePictureInPicture(QRect bounds, double ratio, int edges, QSize minimum);
    bool nativeEventFilter(const QByteArray &, void *, qintptr *) override;
signals:
    void scaleChanged();
    void configurationChanged();
    void browserChanged();
    void overlayChanged();
    void pageRequested();
    void focusToolbarRequested();
    void revealToolbarRequested();
    void monitorsChanged();
    void permissionsChanged();
    void notice(QString message);
    void diagnostic(QString message);

private:
    struct State;
    std::unique_ptr<State> d;
    void registerHotkey();
    void syncDesktop();
    void updateWindowGeometry();
    void savePictureInPicture();
    void applyPipWindowScale();
    void startBrowser();
    void stopBrowser();
    void attachBrowser();
    void syncBrowser();
    void syncSidebar();
    void clearBrowsingData();
    void fail(const QString &message);
    void navigationFailed(int error, int httpStatus);
    void scheduleRecovery();
    void resetRecovery();
    QString dashboardAddress() const;
    void navigateDashboard(bool force = false);
    bool internalAddress(const QString &address) const;
    void applyNextPermission();
    bool eventFilter(QObject *, QEvent *) override;
};
