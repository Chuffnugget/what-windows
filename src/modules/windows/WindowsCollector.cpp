#include "modules/windows/WindowsCollector.h"
#include <QThread>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "security/SecurityStatus.h"
#include <psapi.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wlanapi.h>
#include <wtsapi32.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <winevt.h>
#include <QDateTime>
#include <QTimeZone>
#include <QSysInfo>
#include <QSettings>
#include <QStorageInfo>
#include <QCryptographicHash>
#include <QXmlStreamReader>
#include <QProcess>
#include <QCoreApplication>
#include <QFileInfo>
#include <cmath>
#include <vector>
#include <QNetworkInterface>
#include <QJsonDocument>
#include <QHash>
#include <QSet>
#include <QScopeGuard>

namespace {
QString stable(const QString &s) {
    return QString::fromLatin1(
        QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
}

QJsonObject reading(QString id, QString group, QString name, QJsonValue value, QString kind = "number",
                    QString unit = "", bool recommended = false) {
    if (kind == "text" && value.isString())
        value = value.toString().left(255);
    return {{"id", "win_" + id},
            {"source", "windows"},
            {"group", group},
            {"name", name},
            {"value", value},
            {"kind", kind},
            {"unit", unit},
            {"type", 0},
            {"available", !value.isNull() && !value.isUndefined()},
            {"recommended", recommended},
            {"state_class", kind == "number" ? "measurement" : ""}};
}

struct Pages {
    quint64 allocated = 0, used = 0, peak = 0;
    DWORD pageSize = 4096;
    QJsonArray rows;
};

BOOL CALLBACK pageCallback(LPVOID context, PENUM_PAGE_FILE_INFORMATION info, LPCWSTR filename) {
    auto &p = *static_cast<Pages *>(context);
    auto id = stable(QString::fromWCharArray(filename));
    auto group = "Memory / Page files / " + QString::fromWCharArray(filename);
    p.allocated += info->TotalSize * quint64(p.pageSize);
    p.used += info->TotalInUse * quint64(p.pageSize);
    p.peak += info->PeakUsage * quint64(p.pageSize);
    p.rows.append(reading("page_" + id + "_allocated", group, "Allocated",
                          double(info->TotalSize) * p.pageSize, "number", "B"));
    p.rows.append(reading("page_" + id + "_used", group, "Current usage",
                          double(info->TotalInUse) * p.pageSize, "number", "B"));
    p.rows.append(reading("page_" + id + "_peak", group, "Peak usage", double(info->PeakUsage) * p.pageSize,
                          "number", "B"));
    return TRUE;
}

void audio(QJsonArray &out, EDataFlow flow, const QString &prefix, const QString &group) {
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioEndpointVolume *volume = nullptr;
    QJsonValue level(QJsonValue::Null), mute(QJsonValue::Null), name(QJsonValue::Null);
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator)))) {
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &device))) {
            IPropertyStore *properties = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR)
                    name = QString::fromWCharArray(value.pwszVal);
                PropVariantClear(&value);
                properties->Release();
            }
            if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void **>(&volume)))) {
                float scalar = 0;
                BOOL muted = FALSE;
                if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&scalar)))
                    level = std::round(scalar * 1000) / 10;
                if (SUCCEEDED(volume->GetMute(&muted)))
                    mute = bool(muted);
            }
        }
    }
    out.append(reading(prefix + "_device", group, "Default device", name, "text"));
    out.append(reading(prefix + "_volume", group, "Volume", level, "number", "%", flow == eRender));
    out.append(reading(prefix + "_muted", group, "Muted", mute, "binary", "", flow == eRender));
    if (volume)
        volume->Release();
    if (device)
        device->Release();
    if (enumerator)
        enumerator->Release();
}

QJsonArray eventRows(bool details) {
    QJsonArray result;
    for (const auto channel : {L"System", L"Application"}) {
        QString group = "Events / " + QString::fromWCharArray(channel),
                prefix = "events_" + QString::fromWCharArray(channel).toLower();
        auto query =
            EvtQuery(nullptr, channel,
                     L"*[System[(Level=1 or Level=2) and TimeCreated[timediff(@SystemTime) <= 86400000]]]",
                     EvtQueryChannelPath | EvtQueryReverseDirection);
        int errors = 0, critical = 0, count = 0;
        bool ok = query != nullptr, capped = false;
        QJsonValue source(QJsonValue::Null), eventId(QJsonValue::Null), time(QJsonValue::Null),
            message(QJsonValue::Null);
        if (query) {
            EVT_HANDLE event;
            DWORD returned = 0;
            QElapsedTimer budget;
            budget.start();
            while (count < 2000 && budget.elapsed() < 1500 &&
                   !QThread::currentThread()->isInterruptionRequested()) {
                if (!EvtNext(query, 1, &event, 100, 0, &returned)) {
                    if (GetLastError() != ERROR_NO_MORE_ITEMS)
                        ok = false;
                    break;
                }
                DWORD needed = 0, props = 0;
                EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &needed, &props);
                QByteArray xml(int(needed), 0);
                if (needed &&
                    EvtRender(nullptr, event, EvtRenderEventXml, needed, xml.data(), &needed, &props)) {
                    QXmlStreamReader parser(
                        QString::fromWCharArray(reinterpret_cast<const wchar_t *>(xml.constData())));
                    QString provider, stamp;
                    int level = 0, id = 0;
                    while (!parser.atEnd()) {
                        parser.readNext();
                        if (!parser.isStartElement())
                            continue;
                        if (parser.name() == u"Provider")
                            provider = parser.attributes().value("Name").toString();
                        else if (parser.name() == u"TimeCreated")
                            stamp = parser.attributes().value("SystemTime").toString();
                        else if (parser.name() == u"EventID")
                            id = parser.readElementText().toInt();
                        else if (parser.name() == u"Level")
                            level = parser.readElementText().toInt();
                    }
                    if (parser.hasError())
                        ok = false;
                    if (level == 1)
                        ++critical;
                    if (level == 2)
                        ++errors;
                    if (count == 0) {
                        source = provider;
                        eventId = id;
                        time = stamp;
                        if (details) {
                            auto metadata = EvtOpenPublisherMetadata(
                                nullptr, reinterpret_cast<LPCWSTR>(provider.utf16()), nullptr, 0, 0);
                            if (metadata) {
                                DWORD length = 0;
                                EvtFormatMessage(metadata, event, 0, 0, nullptr, EvtFormatMessageEvent, 0,
                                                 nullptr, &length);
                                if (length && length < 65536) {
                                    std::vector<wchar_t> buffer(length);
                                    if (EvtFormatMessage(metadata, event, 0, 0, nullptr,
                                                         EvtFormatMessageEvent, length, buffer.data(),
                                                         &length))
                                        message = QString::fromWCharArray(buffer.data()).left(255);
                                }
                                EvtClose(metadata);
                            }
                        }
                    }
                } else
                    ok = false;
                EvtClose(event);
                ++count;
            }
            capped = count == 2000 || budget.elapsed() >= 1500 ||
                     QThread::currentThread()->isInterruptionRequested();
            EvtClose(query);
        }
        result.append(reading(prefix + "_status", group, "Collection status",
                              !ok      ? "Access or query error"
                              : capped ? "Event query limit reached; counts unavailable"
                                       : "Ready",
                              "text"));
        result.append(reading(prefix + "_errors", group, "Errors in last 24 hours",
                              ok && !capped ? QJsonValue(errors) : QJsonValue(QJsonValue::Null)));
        result.append(reading(prefix + "_critical", group, "Critical events in last 24 hours",
                              ok && !capped ? QJsonValue(critical) : QJsonValue(QJsonValue::Null)));
        result.append(reading(prefix + "_source", group, "Latest source", source, "text"));
        result.append(reading(prefix + "_id", group, "Latest event ID",
                              eventId.isNull() ? eventId : QJsonValue(QString::number(eventId.toInt())),
                              "text"));
        result.append(reading(prefix + "_time", group, "Latest event time", time, "timestamp"));
        if (details)
            result.append(reading(prefix + "_message", group, "Latest message (first 255 characters)",
                                  message, "text"));
    }
    return result;
}
} // namespace

void WindowsCollector::restoreVolumes(const QJsonArray &catalog) {
    for (const auto &value : catalog) {
        const auto row = value.toObject();
        auto id = row["id"].toString();
        if (id.startsWith("win_disk_") && id.endsWith("_connected"))
            volumes[id.mid(4, id.size() - 14)] = {{"group", row["group"]}};
    }
}

QJsonArray WindowsCollector::collect(const QJsonObject &options) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto cleanup = qScopeGuard([com] {
        if (SUCCEEDED(com))
            CoUninitialize();
    });
    QJsonArray rows;
    auto add = [&](QString id, QString group, QString name, QJsonValue value, QString kind = "number",
                   QString unit = "", bool recommended = false) {
        rows.append(reading(id, group, name, value, kind, unit, recommended));
    };
    if (options["_inventory_only"].toBool() ||
        (!options["_fast_only"].toBool() && !options["_slow_only"].toBool())) {
        QSet<QString> present;
        for (const auto &disk : QStorageInfo::mountedVolumes()) {
            wchar_t guid[MAX_PATH]{};
            auto root = disk.rootPath();
            if (!root.endsWith('/'))
                root += '/';
            root.replace('/', '\\');
            if (!GetVolumeNameForVolumeMountPointW(reinterpret_cast<LPCWSTR>(root.utf16()), guid, MAX_PATH))
                continue; // Only local volumes have stable volume GUIDs.
            auto id = "disk_" + stable(QString::fromWCharArray(guid));
            present.insert(id);
            auto group = "Storage / " + disk.rootPath() + (disk.name().isEmpty() ? "" : " · " + disk.name());
            volumes[id] = {{"group", group}, {"mount", disk.rootPath()}};
            add(id + "_connected", group, "Connected", true, "binary");
            add(id + "_label", group, "Volume label", disk.name(), "text");
            add(id + "_mount", group, "Mount point", disk.rootPath(), "text");
            add(id + "_filesystem", group, "File system", QString::fromLatin1(disk.fileSystemType()), "text");
            add(id + "_removable", group, "Removable drive",
                GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16())) == DRIVE_REMOVABLE, "binary");
            const bool ready = disk.isReady() && disk.bytesTotal() >= 0;
            add(id + "_capacity", group, "Total capacity",
                ready ? QJsonValue(double(disk.bytesTotal())) : QJsonValue(QJsonValue::Null), "number", "B");
            add(id + "_free", group, "Free space",
                ready ? QJsonValue(double(disk.bytesFree())) : QJsonValue(QJsonValue::Null), "number", "B");
            add(id + "_available", group, "Space available to this user",
                ready ? QJsonValue(double(disk.bytesAvailable())) : QJsonValue(QJsonValue::Null), "number",
                "B");
            add(id + "_used", group, "Used space",
                ready ? QJsonValue(double(disk.bytesTotal() - disk.bytesFree()))
                      : QJsonValue(QJsonValue::Null),
                "number", "B");
            add(id + "_percent", group, "Space used",
                ready && disk.bytesTotal() > 0
                    ? QJsonValue(100.0 * (disk.bytesTotal() - disk.bytesFree()) / disk.bytesTotal())
                    : QJsonValue(QJsonValue::Null),
                "number", "%", true);
        }
        for (auto it = volumes.cbegin(); it != volumes.cend(); ++it)
            if (!present.contains(it.key()))
                add(it.key() + "_connected", it.value()["group"].toString(), "Connected", false, "binary");
    }
    if (options["_inventory_only"].toBool())
        return rows;
    if (!options["_slow_only"].toBool()) {
        const auto now = QDateTime::currentDateTime();
        const auto uptime = GetTickCount64() / 1000;
        add("time", "System / Time", "System time", now.toUTC().toString(Qt::ISODateWithMs), "timestamp");
        add("timezone", "System / Time", "Time zone", QString::fromUtf8(QTimeZone::systemTimeZoneId()),
            "text");
        add("utc_offset", "System / Time", "UTC offset", now.offsetFromUtc(), "number", "s");
        add("uptime", "System / Time", "Uptime (includes sleep)", double(uptime), "number", "s", true);
        ULONGLONG awake = 0;
        add("awake_uptime", "System / Time", "Awake uptime",
            QueryUnbiasedInterruptTime(&awake) ? QJsonValue(double(awake) / 1e7)
                                               : QJsonValue(QJsonValue::Null),
            "number", "s");
        add("last_boot", "System / Time", "Last kernel boot (Fast Startup may preserve it)",
            now.addSecs(-qint64(uptime)).toUTC().toString(Qt::ISODate), "timestamp", "", true);
        QSettings os("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     QSettings::NativeFormat);
        add("os_name", "System / Windows", "Operating system", QSysInfo::prettyProductName(), "text");
        for (const auto key : {"EditionID", "DisplayVersion", "CurrentBuildNumber", "UBR"})
            add("os_" + QString(key).toLower(), "System / Windows", QString(key), os.value(key).toString(),
                "text");
        add("os_arch", "System / Windows", "Architecture", QSysInfo::currentCpuArchitecture(), "text");
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        bool memOk = GlobalMemoryStatusEx(&memory);
        auto mem = [&](const char *id, const char *name, quint64 value) {
            add(id, "Memory / Physical", name,
                memOk ? QJsonValue(double(value)) : QJsonValue(QJsonValue::Null), "number", "B");
        };
        mem("ram_total", "Total RAM", memory.ullTotalPhys);
        mem("ram_free", "Available RAM", memory.ullAvailPhys);
        mem("ram_used", "Used RAM", memory.ullTotalPhys - memory.ullAvailPhys);
        add("ram_load", "Memory / Physical", "RAM usage",
            memOk ? QJsonValue(int(memory.dwMemoryLoad)) : QJsonValue(QJsonValue::Null), "number", "%");
        PERFORMANCE_INFORMATION performance{};
        performance.cb = sizeof(performance);
        bool perfOk = GetPerformanceInfo(&performance, sizeof(performance));
        for (const auto &pair : {qMakePair("commit_used", performance.CommitTotal),
                                 qMakePair("commit_limit", performance.CommitLimit),
                                 qMakePair("commit_peak", performance.CommitPeak)})
            add(pair.first, "Memory / Commit", QString(pair.first).replace('_', ' '),
                perfOk ? QJsonValue(double(pair.second) * performance.PageSize)
                       : QJsonValue(QJsonValue::Null),
                "number", "B");
        Pages pages;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        pages.pageSize = system.dwPageSize;
        bool pageOk = EnumPageFilesW(pageCallback, &pages);
        for (const auto &r : pages.rows)
            rows.append(r);
        add("page_allocated", "Memory / Page files", "Total allocated",
            pageOk ? QJsonValue(double(pages.allocated)) : QJsonValue(QJsonValue::Null), "number", "B");
        add("page_used", "Memory / Page files", "Total current usage",
            pageOk ? QJsonValue(double(pages.used)) : QJsonValue(QJsonValue::Null), "number", "B");
        add("page_peak", "Memory / Page files", "Sum of per-file peak usage",
            pageOk ? QJsonValue(double(pages.peak)) : QJsonValue(QJsonValue::Null), "number", "B");
        audio(rows, eRender, "audio", "Audio / Output");
        if (options["microphone"].toBool())
            audio(rows, eCapture, "microphone", "Audio / Microphone");
        DWORD sessionId = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
        if (options["session"].toBool(true)) {
            wchar_t username[257]{};
            DWORD size = 257;
            add("user", "Session", "Windows user",
                GetUserNameW(username, &size) ? QJsonValue(QString::fromWCharArray(username))
                                              : QJsonValue(QJsonValue::Null),
                "text");
            LASTINPUTINFO input{sizeof(input), 0};
            add("idle", "Session", "Idle time",
                GetLastInputInfo(&input) ? QJsonValue(double(DWORD(GetTickCount() - input.dwTime)) / 1000)
                                         : QJsonValue(QJsonValue::Null),
                "number", "s");
            LPWSTR buffer = nullptr;
            DWORD bytes = 0;
            QJsonValue locked(QJsonValue::Null);

            // MinGW omits WTSINFOEX; read only its documented Windows x64 prefix.
            struct SessionPrefix {
                DWORD level;
                alignas(8) ULONG session;
                WTS_CONNECTSTATE_CLASS state;
                LONG flags;
            };

            static_assert(offsetof(SessionPrefix, flags) == 16);
            if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                            static_cast<WTS_INFO_CLASS>(25), &buffer, &bytes)) {
                SessionPrefix info{};
                if (bytes >= sizeof(info)) {
                    memcpy(&info, buffer, sizeof(info));
                    if (info.level == 1 && (info.flags == 0 || info.flags == 1))
                        locked = info.flags == 0;
                }
                WTSFreeMemory(buffer);
            }
            add("locked", "Session", "Session locked", locked, "binary");
            add("remote", "Session", "Remote desktop session", bool(GetSystemMetrics(SM_REMOTESESSION)),
                "binary");
        }
        if (options["foreground"].toBool()) {
            auto window = GetForegroundWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            wchar_t path[32768]{};
            DWORD size = 32768;
            QJsonValue app(QJsonValue::Null);
            if (process) {
                if (QueryFullProcessImageNameW(process, 0, path, &size))
                    app = QFileInfo(QString::fromWCharArray(path)).fileName();
                CloseHandle(process);
            }
            add("foreground_app", "Session / Foreground", "Current application", app, "text");
            if (options["window_titles"].toBool()) {
                wchar_t title[256]{};
                add("foreground_title", "Session / Foreground", "Current window title",
                    window && GetWindowTextW(window, title, 256) ? QJsonValue(QString::fromWCharArray(title))
                                                                 : QJsonValue(QJsonValue::Null),
                    "text");
            }
        }
        SYSTEM_POWER_STATUS power{};
        bool powerOk = GetSystemPowerStatus(&power);
        add("ac_power", "Power", "AC power",
            powerOk && power.ACLineStatus != 255 ? QJsonValue(power.ACLineStatus == 1)
                                                 : QJsonValue(QJsonValue::Null),
            "binary", "", true);
        add("battery_present", "Power", "Battery present",
            powerOk && power.BatteryFlag != 255 ? QJsonValue(!(power.BatteryFlag & 128))
                                                : QJsonValue(QJsonValue::Null),
            "binary");
        add("battery_level", "Power", "Battery level",
            powerOk && !(power.BatteryFlag & 128) && power.BatteryLifePercent <= 100
                ? QJsonValue(int(power.BatteryLifePercent))
                : QJsonValue(QJsonValue::Null),
            "number", "%", true);
        add("battery_remaining", "Power", "Estimated battery time",
            powerOk && power.BatteryLifeTime != DWORD(-1) ? QJsonValue(double(power.BatteryLifeTime))
                                                          : QJsonValue(QJsonValue::Null),
            "number", "s");
        add("battery_charging", "Power", "Battery charging",
            powerOk && power.BatteryFlag != 255 && !(power.BatteryFlag & 128)
                ? QJsonValue(bool(power.BatteryFlag & 8))
                : QJsonValue(QJsonValue::Null),
            "binary");
        auto elapsed = networkClock.isValid() ? networkClock.restart() : 0;
        if (!networkClock.isValid())
            networkClock.start();
        PMIB_IF_TABLE2 table = nullptr;
        if (GetIfTable2(&table) == NO_ERROR) {
            // interfaceFromIndex() rescans every adapter on Windows. Take one
            // address snapshot per poll, so VPN/adapter count does not multiply
            // that work. Rebuild it next poll to pick up changed or removed IPs.
            QHash<int, QStringList> addresses;
            for (const auto &network : networkInterfaces()) {
                auto &ips = addresses[network.index()];
                for (const auto &entry : network.addressEntries())
                    ips << entry.ip().toString();
            }
            for (ULONG i = 0; i < table->NumEntries; ++i) {
                const auto &iface = table->Table[i];
                if (iface.Type == IF_TYPE_SOFTWARE_LOOPBACK || iface.Type == IF_TYPE_TUNNEL)
                    continue;
                wchar_t guid[40]{};
                StringFromGUID2(iface.InterfaceGuid, guid, 40);
                auto id = "net_" + stable(QString::fromWCharArray(guid));
                auto group = "Network / " + QString::fromWCharArray(iface.Alias);
                bool up = iface.OperStatus == IfOperStatusUp;
                add(id + "_connected", group, "Connected", up, "binary");
                add(id + "_link", group, "Link speed",
                    up ? QJsonValue(double(iface.TransmitLinkSpeed)) : QJsonValue(QJsonValue::Null), "number",
                    "bit/s");
                auto previous = networkBytes.value(id);
                bool rateOk = elapsed > 0 && networkBytes.contains(id) && iface.InOctets >= previous.first &&
                              iface.OutOctets >= previous.second && up;
                add(id + "_download", group, "Download rate",
                    rateOk ? QJsonValue(double(iface.InOctets - previous.first) * 1000 / elapsed)
                           : QJsonValue(QJsonValue::Null),
                    "number", "B/s");
                add(id + "_upload", group, "Upload rate",
                    rateOk ? QJsonValue(double(iface.OutOctets - previous.second) * 1000 / elapsed)
                           : QJsonValue(QJsonValue::Null),
                    "number", "B/s");
                add(id + "_received", group, "Bytes received since adapter reset", double(iface.InOctets),
                    "number", "B");
                add(id + "_sent", group, "Bytes sent since adapter reset", double(iface.OutOctets), "number",
                    "B");
                networkBytes[id] = {iface.InOctets, iface.OutOctets};
                const auto ips = addresses.value(int(iface.InterfaceIndex));
                add(id + "_ip", group, "IP addresses", ips.join(", ").left(255), "text");
            }
            FreeMibTable(table);
        }
        wifiAccessIssue.clear();
        if (options["wifi"].toBool(true)) {
            HANDLE wlan = nullptr;
            DWORD negotiated = 0;
            if (WlanOpenHandle(2, nullptr, &negotiated, &wlan) == ERROR_SUCCESS) {
                PWLAN_INTERFACE_INFO_LIST list = nullptr;
                if (WlanEnumInterfaces(wlan, nullptr, &list) == ERROR_SUCCESS) {
                    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                        auto &iface = list->InterfaceInfo[i];
                        DWORD bytes = 0;
                        PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;
                        WLAN_OPCODE_VALUE_TYPE type;
                        wchar_t guid[40]{};
                        StringFromGUID2(iface.InterfaceGuid, guid, 40);
                        auto id = "net_" + stable(QString::fromWCharArray(guid));
                        QJsonValue signal(QJsonValue::Null), ssid(QJsonValue::Null);
                        const DWORD result = WlanQueryInterface(
                            wlan, &iface.InterfaceGuid, wlan_intf_opcode_current_connection,
                            nullptr, &bytes, reinterpret_cast<PVOID *>(&attributes), &type);
                        if (result == ERROR_SUCCESS) {
                            signal = int(attributes->wlanAssociationAttributes.wlanSignalQuality);
                            auto &s = attributes->wlanAssociationAttributes.dot11Ssid;
                            ssid =
                                QString::fromUtf8(reinterpret_cast<const char *>(s.ucSSID), int(s.uSSIDLength));
                            WlanFreeMemory(attributes);
                        } else if (result == ERROR_ACCESS_DENIED) {
                            // Windows 11 protects Wi-Fi connection details with location consent.
                            // Keep the same readings unavailable rather than dropping their IDs.
                            wifiAccessIssue = "Windows denied access to Wi-Fi details. Allow location access "
                                              "in Windows Location Settings to read the network name and signal. "
                                              "Other Windows readings and Home Assistant connections still work.";
                        }
                        auto group = "Network / " + QString::fromWCharArray(iface.strInterfaceDescription);
                        add(id + "_wifi_signal", group, "Wi-Fi signal", signal, "number", "%");
                        add(id + "_ssid", group, "Wi-Fi network", ssid, "text");
                    }
                    WlanFreeMemory(list);
                }
                WlanCloseHandle(wlan, nullptr);
            }
        }
    }
    if (!options["_fast_only"].toBool()) {
        if (!slowClock.isValid() || slowClock.elapsed() >= 60000 || options["refresh_slow"].toBool()) {
            slowRows =
                options["events"].toBool() ? eventRows(options["event_messages"].toBool()) : QJsonArray{};
            securityRows = options["security"].toBool() ? WindowsSecurity::read() : QJsonArray{};
            slowClock.restart();
        }
        if (options["events"].toBool())
            for (const auto &r : slowRows) {
                if (!options["event_messages"].toBool() && r.toObject()["id"].toString().endsWith("_message"))
                    continue;
                rows.append(r);
            }
        if (options["security"].toBool())
            for (const auto &row : securityRows)
                rows.append(row);
        if (options["notifications"].toBool()) {
            if (!bridgeClock.isValid() || bridgeClock.elapsed() >= 15000 ||
                options["refresh_slow"].toBool()) {
                bridgeRows = {};
                QProcess bridge;
                QStringList args;
                // This helper is polled every 15 seconds; never create a console window.
                bridge.setCreateProcessArgumentsModifier(
                    [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
                if (options["notifications"].toBool())
                    args << "--notifications";
                if (options["notification_contents"].toBool())
                    args << "--notification-contents";
                bridge.start(QCoreApplication::applicationDirPath() + "/helpers/WHAT.WindowsHelper.exe",
                             args);
                QString error;
                if (!bridge.waitForStarted(1500))
                    error = "Windows bridge is missing or could not start";
                else if (!bridge.waitForFinished(6000)) {
                    bridge.kill();
                    bridge.waitForFinished(1000);
                    error = "Windows bridge timed out";
                } else {
                    auto output = QJsonDocument::fromJson(bridge.readAllStandardOutput());
                    if (!output.isArray() || bridge.exitCode() != 0)
                        error = QString("Windows helper failed (exit %1)").arg(bridge.exitCode());
                    else
                        bridgeRows = output.array();
                }
                if (!error.isEmpty()) {
                    if (options["notifications"].toBool())
                        bridgeRows.append(reading("notifications_status", "Notifications",
                                                  "Collection status", error, "text"));
                }
                bridgeClock.restart();
            }
            for (const auto &r : bridgeRows)
                rows.append(r);
        }
    }
    if (!options["_slow_only"].toBool())
        add("collector_status", "App health", "Windows collector", "Ready", "text", "", true);
    return rows;
}
