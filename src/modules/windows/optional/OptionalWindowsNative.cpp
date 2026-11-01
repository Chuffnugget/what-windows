#include "OptionalWindowsSources.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shobjidl.h>
#include <winspool.h>
#include <wuapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Devices.Geolocation.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QPointer>
#include <QSettings>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IPropertyValue;
using winrt::Windows::Foundation::PropertyType;
using namespace winrt::Windows::Devices;
using namespace std::chrono_literals;

namespace {
QString text(const winrt::hstring &s) {
    return QString::fromWCharArray(s.c_str());
}

QString identity(const QString &s) {
    return QString::fromLatin1(
        QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
}

QString failure(HRESULT hr) {
    return QString("Windows error 0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

void check(HRESULT hr) {
    winrt::check_hresult(hr);
}

void add(OptionalWindows::Result &r, const QString &feature, const QString &key, const QString &group,
         const QString &name, QJsonValue value, const QString &kind = "number", const QString &unit = {}) {
    r.rows.append(QJsonObject{{"id", "win_opt_" + feature + '_' + key},
                              {"source", "windows"},
                              {"group", group},
                              {"name", name},
                              {"value", value},
                              {"kind", kind},
                              {"unit", unit},
                              {"type", 0},
                              {"recommended", false},
                              {"available", !value.isNull()},
                              {"state_class", kind == "number" ? "measurement" : ""}});
}

template <class Operation> auto bounded(Operation operation, std::chrono::seconds timeout = 5s) {
    if (operation.wait_for(timeout) != AsyncStatus::Completed) {
        if (operation.Status() == AsyncStatus::Error)
            winrt::throw_hresult(operation.ErrorCode());
        operation.Cancel();
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    return operation.GetResults();
}

QString processName(DWORD pid) {
    if (!pid)
        return "Windows system sounds";
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle)
        return QString("Process %1").arg(pid);
    wchar_t path[32768];
    DWORD length = DWORD(std::size(path));
    const bool ok = QueryFullProcessImageNameW(handle, 0, path, &length);
    CloseHandle(handle);
    return ok ? QFileInfo(QString::fromWCharArray(path, int(length))).fileName()
              : QString("Process %1").arg(pid);
}

struct Endpoint {
    QString key, name, group;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioMeterInformation> meter;
    ComPtr<IAudioEndpointVolume> volume;
    ComPtr<IAudioSessionManager2> sessions;
};

struct Beacon {
    QString name;
    int rssi = 0;
    qint64 seen = 0;
};

struct Beacons {
    std::mutex mutex;
    QMap<quint64, Beacon> values;
};

struct SearchCompleted
    : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                   ISearchCompletedCallback> {
    HRESULT STDMETHODCALLTYPE Invoke(ISearchJob *, ISearchCompletedCallbackArgs *) override {
        return S_OK;
    }
};
} // namespace

struct OptionalWindows::Backend::State {
    State() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        clock.start();
    }

    ~State() {
        if (watcher) {
            try {
                watcher.Received(token);
                watcher.Stop();
            } catch (...) {
            }
            watcher = nullptr;
        }
        endpoints.clear();
        enumerator.Reset();
        winrt::uninit_apartment();
    }

    QElapsedTimer clock;
    qint64 nextDevices = 0, nextActivity = 0;
    ComPtr<IMMDeviceEnumerator> enumerator;
    std::vector<Endpoint> endpoints;
    QJsonArray activity;
    Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher watcher{nullptr};
    winrt::event_token token{};
    std::shared_ptr<Beacons> beacons = std::make_shared<Beacons>();
    OptionalWindows::Result audio(const QJsonObject &options);
    OptionalWindows::Result ble();
};

OptionalWindows::Backend::Backend() : state(std::make_unique<State>()) {}

OptionalWindows::Backend::~Backend() = default;

OptionalWindows::Result OptionalWindows::Backend::State::audio(const QJsonObject &options) {
    Result result;
    const bool levels = options["audio_levels"].toBool(), usage = options["audio_activity"].toBool();
    if (!enumerator)
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
    if (clock.elapsed() >= nextDevices) {
        nextDevices = clock.elapsed() + 5000;
        endpoints.clear();
        ComPtr<IMMDeviceCollection> devices;
        check(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &devices));
        UINT count = 0;
        check(devices->GetCount(&count));
        for (UINT i = 0; i < std::min(count, 32u); ++i) {
            Endpoint endpoint;
            if (FAILED(devices->Item(i, &endpoint.device)))
                continue;
            LPWSTR id = nullptr;
            if (FAILED(endpoint.device->GetId(&id)))
                continue;
            endpoint.key = identity(QString::fromWCharArray(id));
            CoTaskMemFree(id);
            ComPtr<IMMEndpoint> direction;
            EDataFlow flow = eRender;
            if (SUCCEEDED(endpoint.device.As(&direction)))
                direction->GetDataFlow(&flow);
            ComPtr<IPropertyStore> store;
            if (SUCCEEDED(endpoint.device->OpenPropertyStore(STGM_READ, &store))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
                    endpoint.name = QString::fromWCharArray(value.pwszVal);
                PropVariantClear(&value);
            }
            if (endpoint.name.isEmpty())
                endpoint.name = flow == eCapture ? "Microphone" : "Speakers";
            endpoint.group =
                QString("Audio / %1 / %2").arg(flow == eCapture ? "Inputs" : "Outputs", endpoint.name);
            endpoints.push_back(std::move(endpoint));
        }
        nextActivity = 0;
    }
    if (usage && clock.elapsed() >= nextActivity) {
        nextActivity = clock.elapsed() + 1000;
        Result current;
        for (auto &e : endpoints) {
            if (!e.sessions)
                e.device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &e.sessions);
            if (!e.volume)
                e.device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &e.volume);
            QStringList apps;
            bool known = false;
            ComPtr<IAudioSessionEnumerator> sessions;
            if (e.sessions && SUCCEEDED(e.sessions->GetSessionEnumerator(&sessions))) {
                int count = 0;
                known = SUCCEEDED(sessions->GetCount(&count));
                for (int i = 0; i < std::min(count, 128); ++i) {
                    ComPtr<IAudioSessionControl> session;
                    AudioSessionState status{};
                    if (FAILED(sessions->GetSession(i, &session)) || FAILED(session->GetState(&status)) ||
                        status != AudioSessionStateActive)
                        continue;
                    ComPtr<IAudioSessionControl2> detail;
                    DWORD pid = 0;
                    if (SUCCEEDED(session.As(&detail)) && SUCCEEDED(detail->GetProcessId(&pid)))
                        apps.append(processName(pid));
                    else
                        apps.append("Unidentified application");
                }
            }
            apps.removeDuplicates();
            apps.sort();
            add(current, "audio_activity", e.key + "_in_use", e.group, "In Use",
                known ? QJsonValue(!apps.isEmpty()) : QJsonValue(), "binary");
            add(current, "audio_activity", e.key + "_applications", e.group, "Applications Using Audio",
                known ? QJsonValue(apps.isEmpty() ? "None" : apps.join(", ").left(255)) : QJsonValue(),
                "text");
            float volume = 0;
            BOOL muted = FALSE;
            add(current, "audio_activity", e.key + "_volume", e.group, "Volume",
                e.volume && SUCCEEDED(e.volume->GetMasterVolumeLevelScalar(&volume))
                    ? QJsonValue(volume * 100)
                    : QJsonValue(),
                "number", "%");
            add(current, "audio_activity", e.key + "_muted", e.group, "Muted",
                e.volume && SUCCEEDED(e.volume->GetMute(&muted)) ? QJsonValue(bool(muted)) : QJsonValue(),
                "binary");
        }
        activity = current.rows;
    }
    if (usage)
        result.rows = activity;
    if (levels)
        for (auto &e : endpoints) {
            if (!e.meter)
                e.device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, &e.meter);
            float peak = 0;
            const bool ok = e.meter && SUCCEEDED(e.meter->GetPeakValue(&peak));
            const QJsonValue value =
                ok ? QJsonValue(std::clamp(double(peak) * 100, 0.0, 100.0)) : QJsonValue();
            add(result, "audio_levels", e.key + "_peak", e.group, "Signal Level", value, "number", "%");
            result.meters["win_opt_audio_levels_" + e.key + "_peak"] = value.toVariant();
            UINT channels = 0;
            if (!ok || FAILED(e.meter->GetMeteringChannelCount(&channels)) || channels == 0 || channels > 32)
                continue;
            std::vector<float> values(channels);
            if (FAILED(e.meter->GetChannelsPeakValues(channels, values.data())))
                continue;
            for (UINT channel = 0; channel < std::min(channels, 8u); ++channel) {
                const auto key = e.key + "_channel_" + QString::number(channel + 1);
                const double level = std::clamp(double(values[channel]) * 100, 0.0, 100.0);
                add(result, "audio_levels", key, e.group, QString("Channel %1 Signal Level").arg(channel + 1),
                    level, "number", "%");
                result.meters["win_opt_audio_levels_" + key] = level;
            }
        }
    if (endpoints.empty())
        result.error = "No active audio endpoints are available.";
    return result;
}

namespace {
OptionalWindows::Result camera() {
    OptionalWindows::Result result;
    QSettings registry(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam",
        QSettings::NativeFormat);
    QStringList active;
    int inspected = 0;
    bool sawRecord = false;
    const auto inspect = [&](const QString &group) {
        registry.beginGroup(group);
        const auto start = registry.value("LastUsedTimeStart").toULongLong();
        const auto stop = registry.value("LastUsedTimeStop").toULongLong();
        sawRecord |= start != 0 || stop != 0;
        // ConsentStore is a best-effort Windows usage indicator, not camera capture.
        if (OptionalWindows::reportedInUse(start, stop)) {
            QString name = group;
            name.replace('#', '\\');
            active.append(QFileInfo(name).fileName());
        }
        registry.endGroup();
    };
    for (const auto &group : registry.childGroups()) {
        if (++inspected > 256)
            break;
        if (group == "NonPackaged") {
            registry.beginGroup(group);
            for (const auto &app : registry.childGroups()) {
                if (++inspected > 256)
                    break;
                inspect(app);
            }
            registry.endGroup();
        } else
            inspect(group);
    }
    active.removeDuplicates();
    active.sort();
    const bool readable = registry.status() == QSettings::NoError && sawRecord;
    add(result, "camera_usage", "active", "Camera Activity", "Windows Reports Camera In Use",
        readable ? QJsonValue(!active.isEmpty()) : QJsonValue(), "binary");
    add(result, "camera_usage", "applications", "Camera Activity", "Reported Camera Applications",
        readable ? QJsonValue(active.isEmpty() ? "None" : active.join(", ").left(255)) : QJsonValue(),
        "text");
    if (!readable)
        result.error =
            "Windows has no readable camera usage records. Camera activity is unavailable; WHAT does not open the camera.";
    return result;
}

OptionalWindows::Result printers() {
    OptionalWindows::Result result;
    DWORD size = 0, count = 0;
    SetLastError(ERROR_SUCCESS);
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2, nullptr, 0, &size, &count);
    if (!size) {
        if (GetLastError() != ERROR_SUCCESS)
            check(HRESULT_FROM_WIN32(GetLastError()));
        add(result, "printers", "count", "Printers", "Installed Printers", 0);
        return result;
    }
    if (size > 16 * 1024 * 1024)
        throw winrt::hresult_error(E_OUTOFMEMORY);
    std::vector<BYTE> buffer(size);
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2, buffer.data(), size, &size,
                       &count))
        check(HRESULT_FROM_WIN32(GetLastError()));
    auto entries = reinterpret_cast<PRINTER_INFO_2W *>(buffer.data());
    add(result, "printers", "count", "Printers", "Installed Printers", int(count));
    for (DWORD i = 0; i < std::min(count, DWORD(128)); ++i) {
        const auto &p = entries[i];
        const QString name = QString::fromWCharArray(p.pPrinterName);
        const auto key = identity(name), group = "Printers / " + name;
        add(result, "printers", key + "_jobs", group, "Queued Jobs", int(p.cJobs));
        add(result, "printers", key + "_offline", group, "Offline", bool(p.Status & PRINTER_STATUS_OFFLINE),
            "binary");
        QStringList status;
        for (const auto &[flag, label] : std::initializer_list<std::pair<DWORD, const char *>>{
                 {PRINTER_STATUS_ERROR, "Error"},
                 {PRINTER_STATUS_PAPER_JAM, "Paper Jam"},
                 {PRINTER_STATUS_PAPER_OUT, "Out of Paper"},
                 {PRINTER_STATUS_PAUSED, "Paused"},
                 {PRINTER_STATUS_PRINTING, "Printing"},
                 {PRINTER_STATUS_OFFLINE, "Offline"},
                 {PRINTER_STATUS_USER_INTERVENTION, "Needs Attention"},
                 {PRINTER_STATUS_TONER_LOW, "Toner Low"}})
            if (p.Status & flag)
                status.append(QString::fromLatin1(label));
        add(result, "printers", key + "_status", group, "Status",
            status.isEmpty() ? QString("Ready") : status.join(", "), "text");
    }
    return result;
}

OptionalWindows::Result updates() {
    OptionalWindows::Result result;
    ComPtr<IUpdateSession> session;
    ComPtr<IUpdateSearcher> searcher;
    check(CoCreateInstance(__uuidof(UpdateSession), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&session)));
    check(session->CreateUpdateSearcher(&searcher));
    check(searcher->put_Online(VARIANT_FALSE));
    auto callback = Microsoft::WRL::Make<SearchCompleted>();
    ComPtr<ISearchJob> job;
    VARIANT context;
    VariantInit(&context);
    BSTR criteria = SysAllocString(L"IsInstalled=0 and IsHidden=0");
    const auto started = searcher->BeginSearch(criteria, callback.Get(), context, &job);
    SysFreeString(criteria);
    check(started);
    QElapsedTimer timeout;
    timeout.start();
    VARIANT_BOOL completed = VARIANT_FALSE;
    while (SUCCEEDED(job->get_IsCompleted(&completed)) && !completed && timeout.elapsed() < 15000 &&
           !QThread::currentThread()->isInterruptionRequested())
        QThread::msleep(100);
    if (!completed) {
        job->RequestAbort();
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    ComPtr<ISearchResult> found;
    check(searcher->EndSearch(job.Get(), &found));
    OperationResultCode code{};
    check(found->get_ResultCode(&code));
    if (code != orcSucceeded)
        throw winrt::hresult_error(E_FAIL, L"Windows Update could not read its cached catalogue.");
    ComPtr<IUpdateCollection> collection;
    check(found->get_Updates(&collection));
    LONG count = 0;
    check(collection->get_Count(&count));
    add(result, "windows_updates", "pending", "Windows Updates", "Pending Updates (Cached Catalogue)",
        int(count));
    ComPtr<ISystemInformation> information;
    VARIANT_BOOL reboot = VARIANT_FALSE;
    check(CoCreateInstance(__uuidof(SystemInformation), nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&information)));
    check(information->get_RebootRequired(&reboot));
    add(result, "windows_updates", "reboot", "Windows Updates", "Restart Required", reboot != VARIANT_FALSE,
        "binary");
    add(result, "windows_updates", "checked", "Windows Updates", "Cached Catalogue Read At",
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate), "timestamp");
    return result;
}

OptionalWindows::Result desktop() {
    OptionalWindows::Result result;
    GUID current{};
    ComPtr<IVirtualDesktopManager> manager;
    check(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager)));
    HWND foreground = GetForegroundWindow();
    if (!foreground)
        throw winrt::hresult_error(E_FAIL, L"No foreground window is available.");
    check(manager->GetWindowDesktopId(foreground, &current));
    if (current == GUID_NULL)
        throw winrt::hresult_error(E_FAIL, L"The current virtual desktop is not exposed for this window.");
    wchar_t guid[40];
    StringFromGUID2(current, guid, 40);
    QSettings root(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
        QSettings::NativeFormat);
    const auto ids = root.value("VirtualDesktopIDs").toByteArray();
    int index = -1;
    for (int offset = 0; offset + int(sizeof(GUID)) <= ids.size(); offset += sizeof(GUID))
        if (memcmp(ids.constData() + offset, &current, sizeof(GUID)) == 0) {
            index = offset / sizeof(GUID) + 1;
            break;
        }
    QString name = root.value("Desktops/" + QString::fromWCharArray(guid) + "/Name").toString();
    if (name.isEmpty() && index > 0)
        name = QString("Desktop %1").arg(index);
    add(result, "virtual_desktop", "id", "Virtual Desktop", "Current Desktop ID",
        QString::fromWCharArray(guid), "text");
    add(result, "virtual_desktop", "number", "Virtual Desktop", "Current Desktop Number",
        index > 0 ? QJsonValue(index) : QJsonValue());
    add(result, "virtual_desktop", "name", "Virtual Desktop", "Current Desktop Name",
        name.isEmpty() ? QJsonValue() : QJsonValue(name), "text");
    return result;
}

OptionalWindows::Result location() {
    using namespace Geolocation;
    using namespace winrt::Windows::Security::Authorization::AppCapabilityAccess;
    OptionalWindows::Result result;
    if (AppCapability::Create(L"location").CheckAccess() != AppCapabilityAccessStatus::Allowed)
        throw winrt::hresult_error(
            E_ACCESSDENIED,
            L"Location is not allowed. Use Request Location Access or Windows Location Settings in this module.");
    Geolocator locator;
    locator.DesiredAccuracy(PositionAccuracy::Default);
    const auto position = bounded(locator.GetGeopositionAsync(60s, 5s), 7s);
    const auto coordinate = position.Coordinate();
    const auto point = coordinate.Point().Position();
    add(result, "location", "latitude", "Location", "Latitude", point.Latitude);
    add(result, "location", "longitude", "Location", "Longitude", point.Longitude);
    add(result, "location", "accuracy", "Location", "Accuracy", coordinate.Accuracy(), "number", "m");
    add(result, "location", "altitude", "Location", "Altitude",
        coordinate.AltitudeAccuracy() ? QJsonValue(point.Altitude) : QJsonValue(), "number", "m");
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(coordinate.Timestamp().time_since_epoch())
            .count() -
        11644473600000LL;
    add(result, "location", "updated", "Location", "Position Updated",
        QDateTime::fromMSecsSinceEpoch(milliseconds, Qt::UTC).toString(Qt::ISODate), "timestamp");
    return result;
}

OptionalWindows::Result bluetooth() {
    using namespace Enumeration;
    OptionalWindows::Result result;
    const auto radios = bounded(Radios::Radio::GetRadiosAsync());
    int adapterIndex = 0;
    for (const auto &radio : radios)
        if (radio.Kind() == Radios::RadioKind::Bluetooth) {
            const auto name = text(radio.Name()), key = identity(name + QString::number(adapterIndex++));
            add(result, "bluetooth", key + "_enabled", "Bluetooth / " + name, "Radio Enabled",
                radio.State() == Radios::RadioState::On, "binary");
        }
    const auto properties = winrt::single_threaded_vector<winrt::hstring>(
        {L"System.Devices.Aep.IsConnected", L"System.Devices.BatteryLife"});
    int count = 0;
    for (const auto &selector : {Bluetooth::BluetoothDevice::GetDeviceSelectorFromPairingState(true),
                                 Bluetooth::BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)}) {
        const auto devices = bounded(DeviceInformation::FindAllAsync(
            selector, properties, DeviceInformationKind::AssociationEndpoint));
        for (const auto &device : devices) {
            if (++count > 128)
                break;
            const auto key = identity(text(device.Id())), name = text(device.Name());
            const auto group = "Bluetooth / " + (name.isEmpty() ? "Unnamed Device " + key.left(6) : name);
            const auto props = device.Properties();
            auto connected = props.HasKey(L"System.Devices.Aep.IsConnected")
                                 ? props.Lookup(L"System.Devices.Aep.IsConnected")
                                 : nullptr;
            add(result, "bluetooth", key + "_connected", group, "Connected",
                connected ? QJsonValue(winrt::unbox_value_or<bool>(connected, false)) : QJsonValue(),
                "binary");
            add(result, "bluetooth", key + "_paired", group, "Paired", true, "binary");
            // Read a battery property only if Windows supplies it. Never connect to a
            // device or poll GATT just to manufacture a battery reading.
            if (props.HasKey(L"System.Devices.BatteryLife")) {
                const auto property = props.Lookup(L"System.Devices.BatteryLife").try_as<IPropertyValue>();
                int battery = -1;
                if (property && property.Type() == PropertyType::UInt32)
                    battery = int(property.GetUInt32());
                else if (property && property.Type() == PropertyType::UInt8)
                    battery = property.GetUInt8();
                if (battery >= 0 && battery <= 100)
                    add(result, "bluetooth", key + "_battery", group, "Battery", battery, "number", "%");
            }
        }
    }
    add(result, "bluetooth", "paired_count", "Bluetooth", "Paired Devices", std::min(count, 128));
    if (!adapterIndex)
        result.error = "No Bluetooth radio is exposed by Windows.";
    return result;
}
} // namespace

OptionalWindows::Result OptionalWindows::Backend::State::ble() {
    using namespace Bluetooth::Advertisement;
    if (!watcher) {
        watcher = BluetoothLEAdvertisementWatcher{};
        watcher.ScanningMode(BluetoothLEScanningMode::Passive);
        token = watcher.Received(
            [data = beacons](const auto &, const BluetoothLEAdvertisementReceivedEventArgs &event) {
                std::lock_guard lock(data->mutex);
                const auto address = event.BluetoothAddress();
                if (!data->values.contains(address) && data->values.size() >= 128)
                    return;
                auto &value = data->values[address];
                const auto name = text(event.Advertisement().LocalName());
                if (!name.isEmpty())
                    value.name = name.left(128);
                value.rssi = event.RawSignalStrengthInDBm();
                value.seen = QDateTime::currentMSecsSinceEpoch();
            });
        watcher.Start();
    } else if (watcher.Status() == BluetoothLEAdvertisementWatcherStatus::Stopped ||
               watcher.Status() == BluetoothLEAdvertisementWatcherStatus::Aborted) {
        watcher.Start();
    }
    Result result;
    if (watcher.Status() == BluetoothLEAdvertisementWatcherStatus::Aborted) {
        result.error =
            "Bluetooth LE scanning is unavailable. Check the radio and Windows Bluetooth permissions.";
        return result;
    }
    QMap<quint64, Beacon> snapshot;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    {
        std::lock_guard lock(beacons->mutex);
        for (auto it = beacons->values.begin(); it != beacons->values.end();)
            if (now - it->seen > 900000 || it->seen > now)
                it = beacons->values.erase(it);
            else
                ++it;
        snapshot = beacons->values;
    }
    int recent = 0;
    for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it) {
        const auto key = identity(QString::number(it.key(), 16));
        const auto &b = it.value();
        const auto group = "Bluetooth LE / " + (b.name.isEmpty() ? "Unnamed Device " + key.left(6) : b.name);
        const bool seen = now - b.seen < 30000;
        recent += seen;
        add(result, "bluetooth_le", key + "_recent", group, "Seen in Last 30 Seconds", seen, "binary");
        add(result, "bluetooth_le", key + "_rssi", group, "Signal Strength",
            seen ? QJsonValue(b.rssi) : QJsonValue(), "number", "dBm");
        add(result, "bluetooth_le", key + "_seen", group, "Last Seen",
            QDateTime::fromMSecsSinceEpoch(b.seen, Qt::UTC).toString(Qt::ISODate), "timestamp");
    }
    add(result, "bluetooth_le", "nearby_count", "Bluetooth LE", "Recently Seen Devices", recent);
    return result;
}

OptionalWindows::Result OptionalWindows::Backend::collect(const QString &feature,
                                                          const QJsonObject &options) {
    try {
        if (feature == "audio")
            return state->audio(options);
        if (feature == "camera_usage")
            return camera();
        if (feature == "printers")
            return printers();
        if (feature == "windows_updates")
            return updates();
        if (feature == "virtual_desktop")
            return desktop();
        if (feature == "location")
            return location();
        if (feature == "bluetooth")
            return bluetooth();
        if (feature == "bluetooth_le")
            return state->ble();
    } catch (const winrt::hresult_error &e) {
        Result result;
        result.error = failure(e.code()) + ": " + text(e.message());
        return result;
    } catch (...) {
        Result result;
        result.error = "Windows could not read this source.";
        return result;
    }
    return {};
}

void OptionalWindowsSources::requestLocationAccess() {
    if (testMode || !options["location"].toBool())
        return;
    // Consent must originate on the foreground UI thread. Poll completion without
    // blocking that thread; destroying this owner cancels the polling callback.
    try {
        auto operation = Geolocation::Geolocator::RequestAccessAsync();
        auto poll = new QTimer(this);
        auto elapsed = std::make_shared<QElapsedTimer>();
        elapsed->start();
        connect(poll, &QTimer::timeout, this, [this, poll, operation, elapsed] {
            if (operation.Status() == AsyncStatus::Started && elapsed->elapsed() < 30000)
                return;
            poll->stop();
            poll->deleteLater();
            try {
                if (operation.Status() != AsyncStatus::Completed) {
                    operation.Cancel();
                    setIssue("location",
                             "Location permission was not granted. Check Windows Location Settings.");
                    return;
                }
                const bool allowed = operation.GetResults() == Geolocation::GeolocationAccessStatus::Allowed;
                if (!options["location"].toBool())
                    return;
                setIssue("location",
                         allowed
                             ? QString{}
                             : "Location permission was denied. Allow access in Windows Location Settings.");
                refreshLocation();
            } catch (...) {
                setIssue("location",
                         "Windows could not request location access. Open Windows Location Settings.");
            }
        });
        poll->start(100);
    } catch (...) {
        setIssue("location", "Windows could not request location access. Open Windows Location Settings.");
    }
}
