#pragma once
#include "modules/beacn/BeacnModule.h"
#include "app/SensorTrayManager.h"
#include "app/ColourPalette.h"
#include "modules/custom/CustomModule.h"
#include "modules/haweb/HomeAssistantWeb.h"
#include "modules/windows/media/Announcements.h"
#include "modules/windows/notifications/RichNotifications.h"
#include "modules/hwinfo/HwinfoReader.h"
#include "comms/TlsIdentity.h"
#include "comms/Discovery.h"
#include "modules/windows/WindowsCollector.h"
#include "app/BrowserModel.h"
#include "app/SensorCatalog.h"
#include "core/BackgroundWorker.h"
#include "core/AsyncDisposal.h"
#include "comms/NetworkService.h"
#include "modules/windows/media/AudioMonitor.h"
#include "modules/icue/IcueSdk.h"
#include "modules/nvidia/NvmlReader.h"
#include "modules/coretemp/CoreTempReader.h"
#include "modules/windows/lighting/DynamicLighting.h"
#include "modules/icue/IcueAutomation.h"
#include <QJsonArray>
#include <memory>
#include <optional>
#include <QAbstractNativeEventFilter>
#include <QMap>
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QSet>
#include <QTimer>
#include <QElapsedTimer>
#include <QVariantList>
#include <QWebSocketServer>
#include <QWebSocket>

// GUI-thread coordinator for settings, cached UI state and module workers.
// Worker jobs receive owned snapshots; they must not read this object or its QSettings.
class Controller : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    Q_PROPERTY(QString uiTheme READ uiTheme WRITE setUiTheme NOTIFY uiThemeChanged)
    Q_PROPERTY(int sidebarWidth READ sidebarWidth NOTIFY sidebarChanged)
    Q_PROPERTY(bool sidebarCollapsed READ sidebarCollapsed NOTIFY sidebarChanged)
    Q_PROPERTY(QStringList moduleOrder READ moduleOrder NOTIFY moduleOrderChanged)
    Q_PROPERTY(qint64 lastHaCommunication READ lastHaCommunication NOTIFY haCommunicationChanged)
    Q_PROPERTY(bool haFeedbackSupported READ haFeedbackSupported NOTIFY haCommunicationChanged)
    Q_PROPERTY(QVariantMap haEntity READ haEntity NOTIFY haEntityChanged)
    Q_PROPERTY(SensorTrayManager *trayManager READ trayManager CONSTANT)
    Q_PROPERTY(ColourPalette *colourPalette READ colourPalette CONSTANT)
    Q_PROPERTY(CustomModule *customSources MEMBER customSources CONSTANT)
    Q_PROPERTY(HomeAssistantWeb *homeAssistantWeb MEMBER homeAssistantWeb CONSTANT)
    Q_PROPERTY(Announcements *announcements MEMBER announcements CONSTANT)
    Q_PROPERTY(QVariantMap info READ uiInfo NOTIFY uiChanged)
    Q_PROPERTY(QString executablePath READ executablePath CONSTANT)
    Q_PROPERTY(QVariantList sensors READ sensors NOTIFY sensorsChanged)
    Q_PROPERTY(QVariantList browser READ browser NOTIFY sensorsChanged)
    Q_PROPERTY(QAbstractItemModel *sensorModel READ sensorModel CONSTANT)
    Q_PROPERTY(bool browserLoading READ browserLoading NOTIFY browserLoadingChanged)
    Q_PROPERTY(QString commandStream READ commandStream NOTIFY commandStreamChanged)
    Q_PROPERTY(QVariantMap displays READ displays NOTIFY uiChanged)
    Q_PROPERTY(QVariantMap dynamicLighting READ dynamicLightingState NOTIFY dynamicLightingChanged)
    Q_PROPERTY(QVariantMap beacn READ beacnState NOTIFY uiChanged)
    Q_PROPERTY(QVariantMap wallpaper READ wallpaperState NOTIFY uiChanged)
    Q_PROPERTY(QVariantMap uiTimings READ uiTimings NOTIFY uiChanged)
    Q_PROPERTY(QString logs READ logs NOTIFY logChanged)
    Q_PROPERTY(QString diagnostics READ uiDiagnostics NOTIFY diagnosticsChanged)
    Q_PROPERTY(QVariantMap entityCleanup READ entityCleanup NOTIFY uiChanged)
    Q_PROPERTY(QVariantList installations READ installations NOTIFY installationsChanged)
    Q_PROPERTY(QVariantList installationConfiguration READ installationConfiguration NOTIFY installationConfigurationChanged)
    Q_PROPERTY(QVariantMap installationStatus READ installationStatus NOTIFY installationsChanged)
    Q_PROPERTY(QString installationError READ installationError NOTIFY installationsChanged)
    Q_PROPERTY(QString diagnosticReport READ diagnosticReport NOTIFY diagnosticReportChanged)
    Q_PROPERTY(bool diagnosticReportBusy READ diagnosticReportBusy NOTIFY diagnosticReportChanged)
    Q_PROPERTY(QString specs READ specs NOTIFY specsChanged)
    Q_PROPERTY(bool specsRefreshing READ specsRefreshing NOTIFY specsChanged)
    Q_PROPERTY(QString specsLastRefreshed READ specsLastRefreshed NOTIFY specsChanged)
    Q_PROPERTY(QString specsNotice READ specsNotice NOTIFY specsChanged)
    Q_PROPERTY(QStringList specsIssues READ specsIssues NOTIFY specsChanged)
public:
    CustomModule *customSources = nullptr;
    HomeAssistantWeb *homeAssistantWeb = nullptr;
    Announcements *announcements = nullptr;
    RichNotifications *richNotifications = nullptr;
    void initAnnouncements();
    void handleAnnouncementCommand(const QString &session, const QJsonObject &command);
    Q_INVOKABLE void runCustomAction(const QString &id);
    Q_SIGNAL void customActionFinished(bool ok, const QString &message);
    void handleCustomAction(const QString &session, const QJsonObject &command);
    void initCustomSources();
    void refreshCustomReadings();
    QTimer customRefreshTimer;
    BackgroundWorker customCatalogJob{"Custom catalogue"};
    quint64 customGeneration = 0;
    Controller(const QString &directory, bool testMode, QObject *parent = nullptr);
    ~Controller() override;
    QVariantMap dynamicLightingState() const;
    QJsonObject lightingShared() const;
    Q_INVOKABLE void setDynamicLightingOption(const QString &option, bool enabled);
    Q_INVOKABLE void refreshDynamicLighting();
    Q_INVOKABLE void applyDynamicLighting(const QVariantMap &command);
    void handleDynamicLightingCommand(const QString &session, const QJsonObject &command);
    DynamicLighting *lighting = nullptr;
    QString lightingFeedback;
    Q_SIGNAL void dynamicLightingChanged();
    Q_INVOKABLE void exportConfiguration();
    // Read and validate a file asynchronously, then emit a preview without applying it.
    Q_INVOKABLE void previewConfiguration();
    // Save the accepted draft for the next start. Permission changes require an explicit opt-in.
    Q_INVOKABLE void stageConfiguration(bool includePermissions);
    Q_INVOKABLE void cancelConfigurationImport();
    Q_SIGNAL void configurationResult(const QString &message);
    Q_SIGNAL void configurationPreview(const QString &text);
    bool configurationBusy = false;
    QJsonObject configurationDraft;
    Q_INVOKABLE void addSensorToTray(const QString &id);

    SensorTrayManager *trayManager() const {
        return sensorTray;
    }

    ColourPalette *colourPalette() const {
        return savedColours;
    }

    QVariantMap info() const;
    QString executablePath() const;
    QVariantMap uiInfo();
    QVariantList sensors() const;
    QVariantList browser() const;

    // Defer model preparation while the browser is hidden, keeping the latest pending change.
    Q_INVOKABLE void setBrowserVisible(bool visible) {
        browserVisible = visible;
        if (visible)
            browserRefreshTimer.start();
        else
            browserRefreshTimer.stop();
        if (visible && !browserInteracting && browserPending)
            refreshBrowserAsync();
    }

    // Hold model replacements during scrolling/interaction so delegates do not move under input.
    Q_INVOKABLE void setBrowserInteracting(bool active) {
        browserInteracting = active;
        if (!active && browserVisible && browserPending)
            refreshBrowserAsync();
    }

    QAbstractItemModel *sensorModel() {
        return &browserModel;
    }

    bool browserLoading() const {
        // Live polling must not hide a completed search, including zero matches.
        return !browserResultsReady &&
               (browserPending || browserJob.isRunning() || browserSearchJob.isRunning());
    }
    Q_SIGNAL void browserLoadingChanged();

    Q_INVOKABLE void setModuleEnabled(const QString &module, bool enabled);
    Q_INVOKABLE void setPollingInterval(const QString &module, int milliseconds);
    bool moduleEnabled(const QString &module) const;
    bool readingModuleEnabled(const QJsonObject &row) const;
    bool readingBrowserEnabled(const QJsonObject &row) const;

    QVariantMap displays() const {
        const auto state = displaysState();
        if (state != displayVariantInput) {
            displayVariantInput = state;
            displayVariantCache = state.toVariantMap();
        }
        return displayVariantCache;
    }

    Q_INVOKABLE void setDisplaysEnabled(bool enabled);
    Q_INVOKABLE void setDisplayControl(bool enabled);
    Q_INVOKABLE void localDisplayCommand(const QVariantMap &arguments);

    Q_INVOKABLE void refreshDisplays() {
        displayClock.invalidate();
        pollDisplays();
    }

    Q_INVOKABLE void setLicense(const QString &license);
    Q_INVOKABLE void setBrowserFilters(const QString &query, const QString &source, bool selectedOnly,
                                       bool favouritesOnly, bool unsharedOnly = false);
    Q_INVOKABLE void setCategoryCollapsed(const QString &category, bool collapsed);
    Q_INVOKABLE void collapseCategories(bool collapsed);
    Q_INVOKABLE void setCategoryEnabled(const QString &category, bool enabled);
    QVariantMap beacnState() const;
    Q_INVOKABLE void saveBeacn(bool enabled, bool control, bool cloud);
    Q_INVOKABLE void beacnCommand(const QVariantMap &args);
    Q_INVOKABLE void browseBeacn(bool executable);
    Q_INVOKABLE void reconnectBeacn();
    Q_INVOKABLE void saveBeacnPaths(const QString &exe, const QString &dll);
    void handleBeacnCommand(const QString &session, const QJsonObject &command);
    QVariantMap wallpaperState() const;
    Q_INVOKABLE void saveWallpaper(bool enabled, bool control, bool sharePath, bool cloud,
                                   const QString &path);
    Q_INVOKABLE QString browseWallpaper(bool executable);
    Q_INVOKABLE void wallpaperCommand(const QVariantMap &args);
    Q_INVOKABLE void refreshWallpaper();
    void handleWallpaperCommand(const QString &session, const QJsonObject &command);
    Q_INVOKABLE QString browseMqttFile(bool privateKey = false);
    QVariantMap trayConnectionInfo() const;
    void startTrayConnection();
    void stopTrayConnection();
    void restartTrayConnection();
    Q_INVOKABLE void setSensorPublishing(const QString &id, int scope, bool high, int decimals = -1);
    Q_INVOKABLE void setCategoryPublishing(const QString &category, int bit, bool enabled);
    Q_INVOKABLE void setPublishingIntervals(int normalMs, int highMs);
    Q_INVOKABLE void setFavourite(const QString &id, bool enabled);
    Q_INVOKABLE void saveSources(const QVariantMap &options);
    Q_INVOKABLE void setStartupEnabled(bool enabled);
    Q_INVOKABLE void setControlPermission(const QString &permission, bool enabled);
    Q_INVOKABLE void setIcueControl(bool enabled);
    Q_INVOKABLE void setIcueOption(const QString &option, bool enabled);
    bool icueDirectEnabled() const;
    bool icueAutomationEnabled() const;
    Q_INVOKABLE QVariantMap icueLightingRow(const QString &id) const;
    // Queue a local edit. requestId correlates replies with the editor session that sent it.
    Q_INVOKABLE void localIcueCommand(const QString &action, const QVariantMap &arguments,
                                      const QString &requestId = {});
    Q_INVOKABLE void requestNotificationAccess();

    Q_INVOKABLE void setStartMinimised(bool enabled) {
        settings.setValue("start_minimised", enabled);
        emit changed();
    }

    void enableNativeEvents(quintptr window);
    bool nativeEventFilter(const QByteArray &, void *, qintptr *) override;

    QString commandStream() const {
        return commandStreamText;
    }

    Q_INVOKABLE void clearCommandStream();

    QString logs() const {
        return logLines.join('\n');
    }

    QString diagnostics() const;
    QString uiDiagnostics();

    QVariantMap entityCleanup() const {
        return entityCleanupState;
    }

    // Removal is a two-step HA exchange. Confirmation requires the current, unexpired preview.
    Q_INVOKABLE void previewEntityCleanup(const QString &installation = {});
    Q_INVOKABLE void confirmEntityCleanup();
    Q_INVOKABLE void copyDiagnostics();
    Q_INVOKABLE void copyLogSnapshot(const QString &text);
    Q_INVOKABLE void saveLogSnapshot(const QString &text);

    QString specs() const {
        return specsText;
    }

    Q_INVOKABLE void refreshSpecs();
    bool specsRefreshing() const {
        return specsBusy;
    }

    QString specsLastRefreshed() const {
        return specsRefreshTime;
    }

    QString specsNotice() const {
        return specsCollectionNotice;
    }

    QStringList specsIssues() const {
        return specsCollectionIssues;
    }

    Q_INVOKABLE void ensureSpecs() {
        if (!specsLoaded)
            refreshSpecs();
    }

    Q_INVOKABLE void copySpecs();
    QString windowMode() const;
    Q_INVOKABLE void setWindowMode(const QString &mode);
    QVariantMap uiTimings() const;
    Q_INVOKABLE void setUiTiming(const QString &key, int milliseconds);
    Q_INVOKABLE void resetUiTimings();

    Q_INVOKABLE void setDiagnosticLogging(bool enabled);
    Q_INVOKABLE void setAppControlEnabled(bool enabled);
    void setAppUpdate(const QJsonObject &update);
    std::function<QString()> restartLauncher;
    std::function<bool()> restartBlocked;

    Q_INVOKABLE void setCrashDumpsEnabled(bool enabled) {
        settings.setValue("diagnostics/dumps", enabled);
        scheduleSettingsFlush();
        emit changed();
    }

    Q_INVOKABLE void openDiagnosticFolder();

    QString uiTheme() const;
    int sidebarWidth() const;
    bool sidebarCollapsed() const;
    Q_INVOKABLE void setSidebarLayout(int width, bool collapsed);
    Q_SIGNAL void sidebarChanged();
    QStringList moduleOrder() const;
    Q_INVOKABLE void setModuleOrder(const QStringList &order);
    Q_SIGNAL void moduleOrderChanged();
    void setUiTheme(const QString &name);
    Q_SIGNAL void uiThemeChanged();
    Q_INVOKABLE void applyWidgetTheme(const QVariantMap &colours);
    qint64 lastHaCommunication() const { return haCommunicationTime; }
    bool haFeedbackSupported() const { return haFeedbackAvailable; }
    QVariantMap haEntity() const { return haEntityLookup; }
    Q_INVOKABLE void requestHaEntity(const QVariantMap &row);
    Q_INVOKABLE bool copyHaEntity(const QString &entity);
    Q_SIGNAL void haCommunicationChanged();
    Q_SIGNAL void haEntityChanged();

    Q_INVOKABLE void setWindowAnimationsEnabled(bool enabled) {
        settings.setValue("ui/window_animations", enabled);
        scheduleSettingsFlush();
        emit changed();
    }

    Q_INVOKABLE void setAnimationsEnabled(bool enabled) {
        settings.setValue("ui/animations", enabled);
        emit changed();
    }

    Q_INVOKABLE void setStartupNotice(bool enabled) {
        settings.setValue("startup_notice", enabled);
        log(enabled ? "Startup notice enabled" : "Startup notice disabled");
        emit changed();
    }

    QString diagnosticReport() const {
        return reportText;
    }

    bool diagnosticReportBusy() const {
        return reportBusy;
    }

    Q_INVOKABLE void prepareDiagnosticReport();

    Q_INVOKABLE void setDiagnosticsVisible(bool visible) {
        if (visible) {
            diagnosticsDirty = true;
            diagnosticsRefreshTimer.start();
            emit diagnosticsChanged();
        } else {
            diagnosticsRefreshTimer.stop();
        }
    }

    Q_INVOKABLE void refreshDiagnostics() {
        sample();
    }

    Q_INVOKABLE void copyPairing();
    QVariantList installations() const;
    QVariantList installationConfiguration() const;
    QVariantMap installationStatus() const;
    QString installationError() const { return installationFailure; }
    Q_INVOKABLE bool saveInstallation(const QVariantMap &definition, const QString &password = {},
                                      bool clearPassword = false);
    Q_INVOKABLE bool setInstallationEnabled(const QString &id, bool enabled);
    Q_INVOKABLE bool deleteInstallation(const QString &id);
    Q_INVOKABLE void copyInstallationPairing(const QString &id);
    Q_INVOKABLE void copyInstallationToken(const QString &id);
    Q_INVOKABLE bool rotateInstallationToken(const QString &id);
    Q_INVOKABLE void copyToken();
    Q_INVOKABLE void copyFingerprint();
    Q_INVOKABLE void setSensorEnabled(const QString &id, bool enabled);
    Q_INVOKABLE void selectSensors(const QString &mode);
    Q_INVOKABLE void selectFiltered(bool enabled);
    Q_INVOKABLE bool startService();
    QVariantMap connectionProgress() const;
    qint64 connectionWaitingSince = 0;
    QString connectionTransport = "none", connectionTls;
    Q_INVOKABLE void stopService();
    Q_INVOKABLE bool restartService();
    Q_INVOKABLE void saveSettings(double sampleSeconds, int publishSeconds, int port, const QString &license,
                                  bool startup);
    Q_INVOKABLE void rotateToken();
    Q_INVOKABLE void renewCertificate();
    Q_INVOKABLE void exportLogs();
    void log(const QString &message);
    std::function<QString(const QString &, const QString &, const QString &)> notificationSender;
signals:
    void installationsChanged();
    void installationConfigurationChanged();
    void installationSaved(const QString &id);
    void certificateRenewalFinished(bool ok, const QString &message);
    void diagnosticReportChanged();
    void commandStreamChanged();
    void localIcueReply(const QString &requestId, bool ok, const QString &message);
    void localIcueFinished(bool ok, const QString &message);
    void localBeacnFinished(const QString &control, bool ok, const QString &message);
    void changed();
    void sensorsChanged();
    void logChanged();
    void activityRecorded(const QString &module, const QString &message);
    void specsChanged();
    void uiChanged();
    void diagnosticsChanged();

private:
    friend class ControllerTests;
    QTimer uiRefreshTimer, diagnosticsRefreshTimer;
    BackgroundWorker diagnosticsJob{"Live diagnostics"};
    bool diagnosticsDirty = true;
    QString cachedDiagnostics;
    std::function<QString()> captureDiagnostics(bool includeVersions = true) const;
    std::function<QJsonObject()> captureDiagnosticVersions() const;
    QVariantMap buildInfo(int activeSelected) const;
    QVariantMap moduleIssues() const;
    BackgroundWorker selectionCountJob{"Selection counts"};
    QSet<QString> countedSelection;
    QHash<QString, int> selectedSourceCounts;
    quint64 catalogMetadataRevision = 0, countedMetadataRevision = 0;
    bool selectedCountsValid = false;
    mutable QVariantMap cachedUiInfo;
    mutable bool uiInfoValid = false;
    mutable QJsonObject displayVariantInput;
    mutable QVariantMap displayVariantCache;
    mutable QJsonObject displayCacheInput[2], displayCacheResult[2];
    mutable QJsonArray displayCacheLayouts[2];
    mutable QVariantList displayCacheFlags[2];
    QTimer settingsFlushTimer;
    BackgroundWorker settingsFlushJob{"Settings persistence"};
    bool settingsFlushPending = false;
    // Coalesce UI edits before syncing a separate QSettings instance on the storage worker.
    void scheduleSettingsFlush();
    void flushSettingsAsync();
    QTimer snapshotTimer;
    BackgroundWorker snapshotJob{"Snapshot preparation"}, trayJob{"Tray readings"};

    struct SnapshotTask {
        std::function<QJsonObject()> build;
        bool event = false, reset = false;
        quint64 epoch = 0;
    };

    QQueue<SnapshotTask> snapshotTasks;
    quint64 snapshotEpoch = 0;
    bool snapshotPending = false;
    void dispatchSnapshot();
    // Capture on the GUI thread; invoke the returned builders on a worker without touching this.
    std::function<QJsonObject()> captureSnapshot() const;
    std::function<QJsonArray()> captureReadings(const QSet<QString> *ids = nullptr) const;

    struct CommandStreamBuffer {
        QMutex mutex;
        QString pending;
    };

    std::shared_ptr<CommandStreamBuffer> commandStreamBuffer = std::make_shared<CommandStreamBuffer>();
    QString commandStreamText;
    QTimer commandStreamTimer;
    void appendCommandStream(const QString &text);
    void flushCommandStream();
    BrowserModel browserModel;

    struct BrowserFolding {
        bool defaultCollapsed = true;
        QSet<QString> exceptions;

        bool isCollapsed(const QString &category) const {
            return defaultCollapsed != exceptions.contains(category);
        }

        QSet<QString> closed(const QSet<QString> &categories) const {
            return defaultCollapsed ? categories - exceptions : exceptions;
        }

        bool operator!=(const BrowserFolding &other) const {
            return defaultCollapsed != other.defaultCollapsed || exceptions != other.exceptions;
        }
    } browserFolding;
    QSet<QString> browserCategories;
    QHash<QString, QStringList> browserCategoryReadings;
    quint64 browserCategoriesRevision = 0, browserCategoriesPolicy = 0;
    BackgroundWorker browserJob{"Sensor browser preparation"};
    BackgroundWorker browserSearchJob{"Sensor browser search"};
    QVariantList browserInput() const;
    QStringList categorySensorIds(const QString &category) const;

    struct BrowserInput {
        QVariantList rows;
        QHash<QString, QStringList> categoryReadings;
    };

    struct BrowserSearchIndex {
        struct Entry {
            QString text, source;
            bool selected = false, favourite = false;
        };

        BrowserModel::Categories grouped;
        QList<Entry> entries;
    };

    struct CachedBrowserInput {
        BrowserSearchIndex index;
        QHash<QString, QStringList> categoryReadings;
        QSet<QString> selected, favourites;
        QJsonObject options;
        quint64 policyGeneration, metadataRevision, permissionsRevision;
    };

    std::shared_ptr<const CachedBrowserInput> browserInputCache;
    // Capture source data and permissions together so formatting sees a consistent policy.
    std::function<BrowserInput()> captureBrowserInput(bool readingsOnly = false) const;
    static QVariantList makeIcueBrowserRows(QVariantList rows, QJsonObject icueData, bool directEnabled,
                                            bool available, QJsonObject state, QSet<QString> favourites);
    static BrowserSearchIndex indexBrowserRows(const QVariantList &rows);
    static BrowserModel::Categories filterBrowserIndex(const BrowserSearchIndex &index,
                                                       const QVariantMap &filters);
    static QVariantList formatBrowser(QVariantList rows, QVariantMap filters, QSet<QString> collapsed);
    std::optional<QVariantMap> currentBrowserFilters;
    QVariantMap browserFilters() const;
    void refreshBrowserAsync();
    quint64 browserGeneration = 0;
    bool browserInteracting = false, browserPending = true, browserVisible = true;
    bool browserResultsReady = false;
    QStringList cachedAddresses;
    QTimer addressTimer;
    BackgroundWorker addressJob{"Network interface inventory"};
    void sample();
    void sampleHwinfo();
    void publishEvent(QJsonArray ids = {});
    void publishPolicy();
    QJsonObject controlState() const;
    QJsonObject controlPermissions() const;
    QElapsedTimer notificationClock;
    QString sendDesktopNotification(const QJsonObject &args);
    void handleNotificationCommand(const QString &session, const QJsonObject &command);
    void handleCommand(const QString &session, const QJsonObject &command);
    void handleAppCommand(const QString &session, const QJsonObject &command);
    void startAppMonitoring();
    QJsonObject appState() const;
    QTimer appStatusTimer;
    BackgroundWorker appJob{"WHAT process diagnostics"};
    bool appRestartPending = false;
    QJsonObject appMetrics{{"uptime_seconds", 0}, {"cpu_percent", QJsonValue::Null},
                           {"memory_bytes", QJsonValue::Null}};
    QJsonObject appUpdate;
    bool displaysEnabled() const;
    QJsonObject displaysState(bool forSharing = false) const;
    void setupDisplays();
    void pollDisplays();
    void handleDisplayCommand(const QString &session, const QJsonObject &command);
    void runDisplayCommand(const QString &session, const QJsonObject &args, const QString &id = {});
    BackgroundWorker displayJob{"Windows display configuration"};
    bool displayQuerying = false;
    QJsonObject queuedDisplayArgs;
    QString queuedDisplaySession, queuedDisplayId;
    quint64 queuedDisplayEpoch = 0;
    bool displayCommandQueued = false;
    void drainDisplayCommand();
    QElapsedTimer displayClock;
    QJsonObject displayInfo{
        {"available", false}, {"status", "Displays disabled"}, {"monitors", QJsonArray{}}};
    QJsonArray displayLayouts, displayRollback;
    QString displayMessage;
    void pollIcue();
    void acceptIcue(QJsonObject data);
    void acceptIcueAutomation(QJsonObject data);
    QJsonObject icueAutomationState() const;
    void handleIcueCommand(const QString &session, const QJsonObject &command);
    QVariantList icueBrowserRows(QVariantList rows) const;
    void runLocalIcueCommand();
    QList<QJsonObject> localIcueQueue;
    std::shared_ptr<std::atomic_uint64_t> localIcueEpoch = std::make_shared<std::atomic_uint64_t>(0);
    std::shared_ptr<IcueSdk> icueSdk = makeAsyncDisposed<IcueSdk>();
    std::shared_ptr<IcueAutomation> icueAutomation = std::make_shared<IcueAutomation>();
    QJsonObject automationData;
    QString lastAutomationTelemetryStatus;
    QElapsedTimer automationClock;
    BackgroundWorker icueJob{"iCUE SDK"}, icueAutomationJob{"iCUE Automation"};
    bool icueAutomationPending = false, icueRgbHandoff = false;
    bool icueCommandBusy(const QString &action, const QJsonObject &args) const;
    void submitIcueCommand(const QString &action, const QJsonObject &args, std::function<bool()> cancelled,
                           std::function<void(QString)> done);
    QTimer icueTimer;
    BackgroundWorker reportJob{"Diagnostic report"};
    QString reportText;
    bool reportBusy = false;
    QVariantMap entityCleanupState;
    QJsonObject entityCleanupRequest;
    QString entityCleanupToken, entityCleanupSession, entityCleanupInstallation;
    QElapsedTimer entityCleanupPreviewClock;
    void sendEntityCleanup(const QString &operation);
    void handleEntityCleanupResult(const QString &session, const QJsonObject &command);
    BackgroundWorker coretempJob{"Core Temp reader"};
    QTimer coretempTimer;
    QElapsedTimer coretempClock;
    quint64 coretempGeneration = 0;
    QJsonObject coretempData;
    void pollCoreTemp();
    void acceptCoreTemp(QJsonObject data);
    bool coretempHealthy() const;
    BackgroundWorker nvidiaJob{"NVIDIA NVML"};
    QTimer nvidiaTimer;
    QElapsedTimer nvidiaClock;
    quint64 nvidiaGeneration = 0;
    SensorTrayManager *sensorTray = nullptr;
    QTimer sensorTrayTimer;
    BeacnModule beacnModule;
    QVariantMap beacnData;
    void initBeacn();
    void acceptBeacn(const QVariantMap &state);
    void configureBeacn();
    QJsonObject beacnShared() const;
    QJsonObject wallpaperData;
    BackgroundWorker wallpaperJob{"Wallpaper Engine"};
    QTimer wallpaperTimer;
    QJsonObject wallpaperShared() const;
    QJsonObject nvidiaData;
    void pollNvidia();
    void acceptNvidia(QJsonObject data);
    bool nvidiaHealthy() const;

    bool icueReset = false;
    QElapsedTimer icueClock;
    QJsonObject icueData{{"enabled", false},
                         {"connected", false},
                         {"control_enabled", false},
                         {"status", "Disabled"},
                         {"devices", QJsonArray{}}};
    // Shell commands can run for minutes, so they get their own worker and don't
    // hold up quick actions such as volume, media keys or locking the PC.
    BackgroundWorker commandJob{"Windows commands"}, shellJob{"Shell commands"}, powerJob{"Power plans"};
    QElapsedTimer powerClock;
    QJsonObject powerInfo;
    std::shared_ptr<std::atomic_uint64_t> commandEpoch = std::make_shared<std::atomic_uint64_t>(0);
    void workerStatus(const QString &name, const QString &error);
    QMap<QString, QString> workerErrors;
    AudioMonitor audioMonitor;
    BackgroundWorker inventoryJob{"Storage inventory"};
    std::shared_ptr<WindowsCollector> inventory = std::make_shared<WindowsCollector>();
    QElapsedTimer inventorySchedule, inventoryClock;
    QTimer hwinfoTimer;
    QElapsedTimer slowSchedule;
    QHash<QString, quint64> eventVersions;
    quint64 eventCounter = 0;
    void publish();
    QJsonObject snapshot() const;
    void persistSelection();
    void restoreSharingScope(const QString &id);
    void generateToken();
    void refreshStartup(std::optional<bool> enabled = {});
    bool startupActive = false, startupBusy = false;
    std::optional<bool> pendingStartup;
    bool healthy() const;
    qint64 age() const;
    QString sourceStatus() const;
    QString wifiIssue;
    QJsonObject sourceOptions() const;
    QJsonArray allReadings(const QSet<QString> *ids = nullptr) const;
    QJsonObject publishingPolicies;
    void refreshPublishingPolicies();
    QTimer browserRefreshTimer, metadataTimer;
    BackgroundWorker metadataJob{"Catalogue persistence"};
    bool metadataPending = false;
    void persistCatalog();
    void mergeReadings(const QString &source, const QJsonArray &rows, int scope = 0);

    struct ReadingsUpdate {
        SensorCatalog before, after;
        QSet<QString> sourceIds, recommended, autoSelected;
        bool metadataChanged = false, hasReadings = false;
    };

    std::function<ReadingsUpdate(const QJsonArray &)> prepareReadingsUpdate(const QString &source,
                                                                            int scope = 0);
    void applyReadingsUpdate(const QString &source, const ReadingsUpdate &update, int scope = 0);
    void sampleWindows();
    std::shared_ptr<WindowsCollector> windows = std::make_shared<WindowsCollector>();
    std::shared_ptr<WindowsCollector> slowWindows = std::make_shared<WindowsCollector>();
    BackgroundWorker windowsJob{"Windows sensors"}, slowJob{"Slow Windows sources"},
        hwinfoJob{"HWiNFO reader"};
    BackgroundWorker mediaJob{"Windows media sessions"};
    QElapsedTimer mediaSchedule, mediaClock;
    QElapsedTimer windowsSlowClock;
    bool tlsReady = false;
    QSslConfiguration tlsConfiguration;
    BackgroundWorker certificateJob{"TLS certificate renewal"};
    SensorCatalog catalog;
    QHash<QString, QSet<QString>> catalogSourceIds;
    qsizetype catalogSourceIndexSize = -1;
    QSet<QString> favourites;
    QElapsedTimer appClock, windowsClock;
    QDateTime lastTransmission;
    quint64 connections = 0;
    QJsonValue displayState = QJsonValue(QJsonValue::Null);
    void *powerNotification = nullptr;
    quintptr nativeWindow = 0;
    bool refreshSlow = true;
    quint64 collectionGeneration = 0, jobGeneration = 0;
    QString settingsStatus;
    QString directory;
    void initialiseInstallations();
    void ensureInstallationColours();
    void applyInstallations();
    QHash<QString, QVariantMap> installationStates;
    QString installationFailure;
    bool testMode;
    QSettings settings;
    ColourPalette *savedColours = nullptr;
    HwinfoReader reader;
    HwinfoSnapshot latest;
    QElapsedTimer pollClock;
    qint64 lastPoll = 0, initialAge = 0;
    QString deviceId, token, fingerprint, serverStatus, discoveryStatus;
    int activePort = 0;
    QTimer sampleTimer, publishTimer;
    NetworkService server;
    Discovery discovery;
    int clientCount = 0;
    qint64 haCommunicationTime = 0;
    bool haFeedbackAvailable = false;
    QVariantMap haEntityLookup;
    QSet<QString> selected;
    QStringList logLines;
    QDateTime lastHwinfoAttempt, lastHwinfoSuccess;
    bool specsLoaded = false;
    QString specsText = "Collecting computer specifications in the background…";
    bool specsBusy = false;
    QString specsRefreshTime;
    QString specsCollectionNotice;
    QStringList specsCollectionIssues;
};
