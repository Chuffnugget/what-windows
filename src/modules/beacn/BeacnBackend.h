#pragma once

#include <QObject>
#include <QHash>
#include <QLibrary>
#include <QVariantList>
#include <QStringList>
#include <QVector>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>

class QFile;
class QTimer;

class BeacnBackend final : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString exePath READ exePath WRITE setExePath NOTIFY installationChanged)
    Q_PROPERTY(QVariantMap installation READ installation NOTIFY installationChanged)
    Q_PROPERTY(bool discoveryBusy READ discoveryBusy NOTIFY installationChanged)
    Q_PROPERTY(bool reconnectPending READ reconnectPending NOTIFY connectedChanged)
    Q_PROPERTY(QVariantMap globalState READ globalState NOTIFY globalStateChanged)
    Q_PROPERTY(QVariantList deviceChoices READ deviceChoices NOTIFY globalStateChanged)
    Q_PROPERTY(QString globalStatus READ globalStatus NOTIFY globalStateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString dllPath READ dllPath WRITE setDllPath NOTIFY dllPathChanged)
    Q_PROPERTY(QString apiVersion READ apiVersion NOTIFY deviceInfoChanged)
    Q_PROPERTY(QString mixerVersion READ mixerVersion NOTIFY deviceInfoChanged)
    Q_PROPERTY(quint32 sampleRate READ sampleRate NOTIFY deviceInfoChanged)
    Q_PROPERTY(QString channelSummary READ channelSummary NOTIFY deviceInfoChanged)
    Q_PROPERTY(QString logPath READ logPath CONSTANT)
    Q_PROPERTY(QString recordingStatus READ recordingStatus NOTIFY recordingChanged)
    Q_PROPERTY(bool recording READ recording WRITE setRecording NOTIFY recordingChanged)
    Q_PROPERTY(QVariantList nativeStates READ nativeStates NOTIFY nativeStatesChanged)
    Q_PROPERTY(bool verboseLogging READ verboseLogging WRITE setVerboseLogging NOTIFY verboseLoggingChanged)
    Q_PROPERTY(bool writesArmed READ writesArmed WRITE setWritesArmed NOTIFY writesArmedChanged)
    Q_PROPERTY(QStringList sourceNames READ sourceNames NOTIFY sourceNamesChanged)
    Q_PROPERTY(QStringList busNames READ busNames NOTIFY deviceInfoChanged)
    Q_PROPERTY(QVariantList matrix READ matrix NOTIFY matrixChanged)
    Q_PROPERTY(QVariantList sourceLevels READ sourceLevels NOTIFY matrixChanged)
    Q_PROPERTY(QString profilePath READ profilePath NOTIFY profileChanged)
    Q_PROPERTY(QVariantList profileMixers READ profileMixers NOTIFY profileChanged)
    Q_PROPERTY(QString diffSummary READ diffSummary NOTIFY diffSummaryChanged)
    Q_PROPERTY(int detectedSourcePair READ detectedSourcePair NOTIFY detectionChanged)
    Q_PROPERTY(QString detectionSummary READ detectionSummary NOTIFY detectionChanged)
    Q_PROPERTY(bool notificationActive READ notificationActive NOTIFY notificationChanged)
    Q_PROPERTY(qulonglong notificationCount READ notificationCount NOTIFY notificationChanged)
    Q_PROPERTY(QString lastNotification READ lastNotification NOTIFY notificationChanged)
    Q_PROPERTY(bool beacnAppBridgeReady READ beacnAppBridgeReady NOTIFY beacnAppBridgeChanged)
    Q_PROPERTY(QString beacnAppBridgeStatus READ beacnAppBridgeStatus NOTIFY beacnAppBridgeChanged)
    Q_PROPERTY(int beacnAppSliderCount READ beacnAppSliderCount NOTIFY beacnAppBridgeChanged)
    Q_PROPERTY(int beacnAppMappedFaderCount READ beacnAppMappedFaderCount NOTIFY beacnAppBridgeChanged)
    Q_PROPERTY(QVariantList beacnAppFaders READ beacnAppFaders NOTIFY beacnAppBridgeChanged)
    Q_PROPERTY(QVariantList beacnAppLevels READ beacnAppLevels NOTIFY beacnAppLevelsChanged)
    Q_PROPERTY(QString winUsbStatus READ winUsbStatus NOTIFY winUsbProbeChanged)
    Q_PROPERTY(QString winUsbPath READ winUsbPath NOTIFY winUsbProbeChanged)
    Q_PROPERTY(QString winUsbGuid READ winUsbGuid NOTIFY winUsbProbeChanged)
    Q_PROPERTY(QVariantList winUsbParameters READ winUsbParameters NOTIFY winUsbProbeChanged)
    Q_PROPERTY(QString winUsbDiffSummary READ winUsbDiffSummary NOTIFY winUsbProbeChanged)

public:
    explicit BeacnBackend(QObject *parent = nullptr);
    ~BeacnBackend() override;

    QString exePath() const {
        return m_exePath;
    }

    QVariantMap installation() const {
        return m_installation;
    }

    bool discoveryBusy() const {
        return m_discoveryBusy;
    }

    bool reconnectPending() const {
        return m_reconnectPending;
    }

    void setExePath(const QString &path);
    Q_INVOKABLE void scanInstalledFiles(bool search = true);
    Q_INVOKABLE void selectInstalledFile(const QUrl &url, bool executable);

    QVariantMap globalState() const {
        return m_globalState;
    }

    QVariantList deviceChoices() const {
        return m_deviceChoices;
    }

    QString globalStatus() const {
        return m_globalStatus;
    }

    Q_INVOKABLE bool globalControl(int operation, int value = 0, const QString &target = QString());
    void readGlobalState(void *process, quintptr base, const QVector<quintptr> &mixers,
                         const QVector<quintptr> &profiles, const QVector<quintptr> &assignments,
                         const QVector<quintptr> &osds, const QVector<quintptr> &settings);

    bool connected() const {
        return m_connected;
    }

    void setCommandGuard(std::function<bool()> guard) {
        m_commandGuard = std::move(guard);
    }

    bool commandUncertain() const {
        return m_globalUncertain;
    }

    bool nativeDiscoveryPending() const {
        return m_nativeScanPending;
    }

    quint32 nativeProcessId() const {
        return m_globalPid;
    }

    QString status() const {
        return m_status;
    }

    QString dllPath() const {
        return m_dllPath;
    }

    QString apiVersion() const {
        return m_apiVersion;
    }

    QString mixerVersion() const {
        return m_mixerVersion;
    }

    quint32 sampleRate() const {
        return m_sampleRate;
    }

    QString channelSummary() const {
        return m_channelSummary;
    }

    QString logPath() const {
        return m_logPath;
    }

    QString recordingStatus() const {
        return m_recordingStatus;
    }

    bool recording() const {
        return m_recording;
    }

    void setRecording(bool enabled);
    Q_INVOKABLE void markObservation(const QString &text);
    Q_INVOKABLE bool stepGameVolume(double percentStep);
    Q_INVOKABLE bool setNativeSourcePercent(int sourcePair, double percent);
    Q_INVOKABLE bool nativeSourceAvailable(int sourcePair) const;

    QVariantList nativeStates() const {
        return m_nativeStates;
    }

    void refreshNativeStates(bool allowDiscovery = true);
    Q_INVOKABLE bool setNativeControl(int sourcePair, bool active, bool channelButton);
    static int nativeSourceType(int sourcePair);
    QString nativeSourceName(int sourcePair) const;
    bool writeNativeVolume(int sourcePair, double value, bool relative, int operation = 1);
    void recordTick();
    void exportRecording(bool heartbeat);

    bool verboseLogging() const {
        return m_verboseLogging;
    }

    bool writesArmed() const {
        return m_writesArmed;
    }

    QStringList sourceNames() const {
        return m_sourceNames;
    }

    QStringList busNames() const {
        return m_busNames;
    }

    QVariantList matrix() const {
        return m_matrix;
    }

    QVariantList sourceLevels() const;

    QString profilePath() const {
        return m_profilePath;
    }

    QVariantList profileMixers() const {
        return m_profileMixers;
    }

    QString diffSummary() const {
        return m_diffSummary;
    }

    int detectedSourcePair() const {
        return m_detectedSourcePair;
    }

    QString detectionSummary() const {
        return m_detectionSummary;
    }

    bool notificationActive() const {
        return m_notificationActive;
    }

    qulonglong notificationCount() const {
        return m_notificationCount;
    }

    QString lastNotification() const {
        return m_lastNotification;
    }

    bool beacnAppBridgeReady() const {
        return m_beacnAppBridgeReady;
    }

    QString beacnAppBridgeStatus() const {
        return m_beacnAppBridgeStatus;
    }

    int beacnAppSliderCount() const {
        return m_beacnAppSliderCount;
    }

    int beacnAppMappedFaderCount() const {
        return m_beacnAppMappedFaderCount;
    }

    QVariantList beacnAppFaders() const {
        return m_beacnAppFaders;
    }

    QVariantList beacnAppLevels() const {
        return m_beacnAppLevels;
    }

    QString winUsbStatus() const {
        return m_winUsbStatus;
    }

    QString winUsbPath() const {
        return m_winUsbPath;
    }

    QString winUsbGuid() const {
        return m_winUsbGuid;
    }

    QVariantList winUsbParameters() const {
        return m_winUsbParameters;
    }

    QString winUsbDiffSummary() const {
        return m_winUsbDiffSummary;
    }

    void setDllPath(const QString &path);
    void setVerboseLogging(bool enabled);
    void setWritesArmed(bool armed);

    Q_INVOKABLE bool connectDriver();
    Q_INVOKABLE void disconnectDriver();
    Q_INVOKABLE bool refreshDeviceInfo();
    Q_INVOKABLE bool refreshMatrix();
    Q_INVOKABLE QVariantMap readStereoRoute(int sourcePair, int busPair);
    Q_INVOKABLE bool setStereoRouteDb(int sourcePair, int busPair, double db);
    Q_INVOKABLE bool setStereoRoutePercent(int sourcePair, int busPair, double percent);
    Q_INVOKABLE bool setSourcePercent(int sourcePair, double percent);
    Q_INVOKABLE bool captureBaseline();
    Q_INVOKABLE bool compareBaseline();
    Q_INVOKABLE void refreshProfile();
    Q_INVOKABLE void setSourceAlias(int sourcePair, const QString &name);
    Q_INVOKABLE void clearLogView();
    Q_INVOKABLE bool restartNotificationMonitor();
    Q_INVOKABLE bool scanBeacnAppBridge();
    Q_INVOKABLE bool setBeacnAppFaderPercent(int sourcePair, int busPair, double percent);
    Q_INVOKABLE bool beacnAppFaderMapped(int sourcePair, int busPair) const;
    Q_INVOKABLE bool captureMixCreateParameterBaseline();
    Q_INVOKABLE bool compareMixCreateParameters();
    Q_INVOKABLE bool probeMixCreateParameters();
    Q_INVOKABLE bool verifyMixCreateParameterWritePath();

signals:
    void globalStateChanged();
    void connectedChanged();
    void statusChanged();
    void dllPathChanged();
    void installationChanged();
    void deviceInfoChanged();
    void recordingChanged();
    void nativeStatesChanged();
    void verboseLoggingChanged();
    void writesArmedChanged();
    void sourceNamesChanged();
    void matrixChanged();
    void profileChanged();
    void diffSummaryChanged();
    void detectionChanged();
    void notificationChanged();
    void beacnAppBridgeChanged();
    void beacnAppLevelsChanged();
    void winUsbProbeChanged();
    void logMessage(const QString &line);

private:
    friend class ControllerTests;
    static bool validateNativeFader(void *process, quintptr base, quintptr object,
                                    const QByteArray &name, bool linked, float &current);
    // Addresses come from the current validated readback, not a fresh heap walk per edit.
    bool m_nativeScanPending = false;
    std::shared_ptr<std::atomic_bool> m_nativeScanStop = std::make_shared<std::atomic_bool>(false);
    QVector<quintptr> m_nativeFaders;
    quintptr m_nativeBase = 0;
    quint64 m_nativeBirth = 0;
    std::function<bool()> m_commandGuard;
    using Status = std::uint32_t;
    using DeviceHandle = void *;

    using GetApiVersionFn = std::uint32_t (*)();
    using EnumerateDevicesFn = Status (*)();
    using GetDeviceCountFn = std::uint32_t (*)();
    using OpenDeviceByIndexFn = Status (*)(std::uint32_t, DeviceHandle *);
    using CloseDeviceFn = Status (*)(DeviceHandle);
    using GetCurrentSampleRateFn = Status (*)(DeviceHandle, std::uint32_t *);
    using GetDspPropertyFn = Status (*)(DeviceHandle, void *, std::uint32_t);
    using SetDspPropertyFn = Status (*)(DeviceHandle, const void *, std::uint32_t);
    using RegisterDeviceNotificationFn = Status (*)(DeviceHandle, std::uint32_t, void *, std::uint32_t);
    using ReadDeviceNotificationFn = Status (*)(DeviceHandle, std::uint32_t *, unsigned char *, std::uint32_t,
                                                std::uint32_t *);

    struct Api {
        GetApiVersionFn getApiVersion = nullptr;
        EnumerateDevicesFn enumerateDevices = nullptr;
        GetDeviceCountFn getDeviceCount = nullptr;
        OpenDeviceByIndexFn openDeviceByIndex = nullptr;
        CloseDeviceFn closeDevice = nullptr;
        GetCurrentSampleRateFn getCurrentSampleRate = nullptr;
        GetDspPropertyFn getDspProperty = nullptr;
        SetDspPropertyFn setDspProperty = nullptr;
        RegisterDeviceNotificationFn registerDeviceNotification = nullptr;
        ReadDeviceNotificationFn readDeviceNotification = nullptr;
    };

    struct RouteKey {
        int inputPin = 0;
        int inputIndex = 0;
        int outputPin = 0;
        int outputIndex = 0;

        bool operator==(const RouteKey &other) const noexcept {
            return inputPin == other.inputPin && inputIndex == other.inputIndex &&
                   outputPin == other.outputPin && outputIndex == other.outputIndex;
        }

        friend size_t qHash(const RouteKey &key, size_t seed = 0) noexcept {
            seed = ::qHash(key.inputPin, seed);
            seed = ::qHash(key.inputIndex, seed);
            seed = ::qHash(key.outputPin, seed);
            seed = ::qHash(key.outputIndex, seed);
            return seed;
        }
    };

    struct RouteValue {
        Status status = 0;
        std::int32_t raw = 0;
    };

    bool loadApi();
    void unloadApi();
    bool openDevice();
    void closeDevice();
    bool resolveFunctions();
    bool startNotificationMonitor();
    void stopNotificationMonitor();
    void drainDeviceNotifications();
    QString notificationEventName(std::uint32_t eventType) const;
    void releaseBeacnAppBridge();
#ifdef Q_OS_WIN
    bool readMixCreateParameters(QHash<quint32, quint32> &values, QString *errorText = nullptr,
                                 bool verifyNoopWrite = false);
#endif

    bool getInterfaceVersion(std::uint32_t &major, std::uint32_t &minor);
    bool getChannelCount(std::uint32_t propertyId, int &count);
    RouteValue readWeight(int inputPin, int inputIndex, int outputPin, int outputIndex, bool logIo = false);
    bool writeWeight(int inputPin, int inputIndex, int outputPin, int outputIndex, std::int32_t raw,
                     bool logIo = true);
    QHash<RouteKey, RouteValue> snapshotAllRoutes(bool *ok = nullptr);

    void rebuildSourceNames();
    void rebuildBusNames();
    void autoResolveSourceNames();
    void updateStatus(const QString &text);
    void appendLog(const QString &message);
    QString statusHex(Status status) const;
    QString routeText(int inputPin, int inputIndex, int outputPin, int outputIndex) const;
    static double rawToDb(std::int32_t raw);
    static std::int32_t dbToRaw(double db);
    static double rawToPercent(std::int32_t raw);
    static std::int32_t percentToRaw(double percent);

    static int storedPin(int publicPin) {
        return publicPin & 0x3;
    }

    QLibrary m_library;
    Api m_api;
    DeviceHandle m_device = nullptr;

    bool m_connected = false;
    bool m_verboseLogging = false;
    bool m_writesArmed = false;
    QString m_status = QStringLiteral("Disconnected");
    QString m_dllPath;
    QString m_exePath;
    QVariantMap m_installation;
    bool m_discoveryBusy = false, m_reconnectPending = false;
    quint64 m_installationGeneration = 0, m_connectionGeneration = 0;
    QTimer *m_reconnectTimer = nullptr;
    QString m_apiVersion = QStringLiteral("-");
    QString m_mixerVersion = QStringLiteral("-");
    std::uint32_t m_mixerMajor = 0;
    std::uint32_t m_mixerMinor = 0;
    quint32 m_sampleRate = 0;
    QString m_channelSummary = QStringLiteral("-");
    QString m_logPath;
    QString m_recordingStatus;
    bool m_recording = true;
    bool m_recordTick = false;
    int m_recordTicks = 0;
    QTimer *m_recordTimer = nullptr;
    QFile *m_csvFile = nullptr;
    QVariantList m_recordedMatrix;
    QVariantList m_nativeStates;
    QVariantMap m_globalState;
    QVariantList m_deviceChoices;
    QString m_globalStatus = QStringLiteral("Connect to read BEACN controls");
    quintptr m_globalMixer = 0, m_globalProfiles = 0, m_globalAssignments = 0, m_globalOsd = 0,
             m_globalSettings = 0;
    quint32 m_globalPid = 0;
    bool m_globalBusy = false, m_globalUncertain = false;
    quint64 m_globalSequence = 0;

    int m_deviceOutCount = 0;
    int m_deviceInCount = 0;
    int m_playbackCount = 0;
    int m_recordCount = 0;
    int m_virtualPlaybackCount = 0;
    int m_virtualRecordCount = 0;

    QStringList m_sourceNames;
    QStringList m_busNames;
    QHash<int, QString> m_sourceAliases;
    QHash<int, QString> m_autoSourceAliases;
    QVector<quint32> m_sourceRouteMasks;
    QVariantList m_matrix;

    QString m_profilePath;
    QVariantList m_profileMixers;

    QHash<RouteKey, RouteValue> m_baseline;
    QString m_diffSummary = QStringLiteral("No snapshot captured.");
    int m_detectedSourcePair = -1;
    QString m_detectionSummary = QStringLiteral("No source identified yet.");

    QFile *m_logFile = nullptr;

#ifdef Q_OS_WIN
    struct UiaFaderBinding {
        int sourcePair = -1;
        int busPair = -1;
        void *element = nullptr;
        double minimum = 0.0;
        double maximum = 100.0;
        QString sourceName;
        QString elementName;
        QString automationId;
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
    };

    void pollBeacnAppFaders();
    bool clickBeacnAppFader(UiaFaderBinding &binding, double percent);

    void *m_notificationEvent = nullptr;
    void *m_uiaAutomation = nullptr;
    void *m_beacnWindow = nullptr;
    bool m_uiaComOwned = false;
    QVector<UiaFaderBinding> m_uiaFaderBindings;
#endif
    QTimer *m_notificationPollTimer = nullptr;
    QTimer *m_beacnAppPollTimer = nullptr;
    bool m_notificationActive = false;
    qulonglong m_notificationCount = 0;
    QString m_lastNotification = QStringLiteral("No driver notification received yet.");

    bool m_beacnAppBridgeReady = false;
    QString m_beacnAppBridgeStatus = QStringLiteral("BEACN app bridge not scanned yet.");
    int m_beacnAppSliderCount = 0;
    int m_beacnAppMappedFaderCount = 0;
    QVariantList m_beacnAppFaders;
    QVariantList m_beacnAppLevels;

    QString m_winUsbStatus = QStringLiteral("Not probed yet.");
    QString m_winUsbPath;
    QString m_winUsbGuid;
    QVariantList m_winUsbParameters;
    QHash<quint32, quint32> m_winUsbBaseline;
    QString m_winUsbDiffSummary = QStringLiteral("No hardware parameter baseline captured.");
};
