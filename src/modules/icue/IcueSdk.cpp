#include "modules/icue/IcueSdk.h"
#include "third_party/icue/iCUESDK.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QSet>
#include <QMap>
#include <QStringList>
#include <cstring>
#include <QDir>
#include <QFileInfo>
#include <cmath>
#include <windows.h>

namespace {
QString failure(CorsairError error) {
    switch (error) {
    case CE_Success:
        return {};
    case CE_NotConnected:
        return "iCUE disconnected: start iCUE and enable SDK software/game integrations";
    case CE_NoControl:
        return "Another application has exclusive iCUE lighting control";
    case CE_DeviceNotFound:
        return "iCUE device was disconnected";
    case CE_IncompatibleProtocol:
        return "iCUE version is incompatible with this SDK; update iCUE";
    case CE_NotAllowed:
        return "iCUE blocked access. Open iCUE and approve WHAT in 'SDK: New Client Access Confirmation', or allow it in Settings > SDK.";
    default:
        return QString("iCUE SDK error %1").arg(int(error));
    }
}

bool integer(QJsonValue value, double low, double high) {
    return value.isDouble() && std::isfinite(value.toDouble()) &&
           value.toDouble() == std::floor(value.toDouble()) && value.toDouble() >= low &&
           value.toDouble() <= high;
}

QString propertyName(int id) {
    return QMap<int, QString>{{2, "Microphone enabled"}, {3, "Surround sound"},  {4, "Sidetone"},
                              {5, "Equalizer preset"},   {6, "Physical layout"}, {7, "Logical layout"},
                              {9, "Battery level"}}
        .value(id);
}
} // namespace

struct IcueSdk::Impl {
    QString libraryPath;
    HMODULE library = nullptr;
    std::atomic_int state{CSS_Closed};
    bool started = false;
#define API(name) decltype(&Corsair##name) name = nullptr;
    API(Connect)

    API(Disconnect)
    API(GetSessionDetails)
    API(GetDevices)
    API(GetLedPositions)
    API(GetLedColors)
    API(SetLedColors)
    API(GetLedLuidForKeyName)
    API(GetDevicePropertyInfo)
    API(ReadDeviceProperty)
    API(WriteDeviceProperty)
    API(FreeProperty)
#undef API

    static void callback(void *context, const CorsairSessionStateChanged *event) {
        if (event)
            static_cast<Impl *>(context)->state.store(event->state);
    }

    QString load() {
        if (library)
            return {};
        const auto path =
            QDir::toNativeSeparators(libraryPath.isEmpty() ? QCoreApplication::applicationDirPath() +
                                                                 "/helpers/iCUESDK.x64_2019.dll"
                                                           : libraryPath)
                .toStdWString();
        if (!QFileInfo::exists(QString::fromStdWString(path)))
            return "iCUE SDK DLL missing from helpers. Build WHAT again: CMake now prepares it automatically for Debug and Release.";
        library = LoadLibraryExW(path.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            return QString(
                       "iCUE SDK DLL found, but Windows could not load it (error %1): %2. Rebuild or repair the application to restore its bundled dependencies.")
                .arg(GetLastError())
                .arg(QString::fromStdWString(path));
#define LOAD(name)                                                                                           \
    name = reinterpret_cast<decltype(name)>(GetProcAddress(library, "Corsair" #name));                       \
    if (!name) {                                                                                             \
        FreeLibrary(library);                                                                                \
        library = nullptr;                                                                                   \
        return "Incompatible iCUE SDK DLL";                                                                  \
    }
        LOAD(Connect)
        LOAD(Disconnect)
        LOAD(GetDevices)
        LOAD(GetLedPositions)
        LOAD(GetLedColors)
        LOAD(SetLedColors)
        LOAD(GetDevicePropertyInfo)
        LOAD(ReadDeviceProperty)
        LOAD(WriteDeviceProperty)
        LOAD(FreeProperty)
#undef LOAD
        GetSessionDetails = reinterpret_cast<decltype(GetSessionDetails)>(
            GetProcAddress(library, "CorsairGetSessionDetails"));
        GetLedLuidForKeyName = reinterpret_cast<decltype(GetLedLuidForKeyName)>(
            GetProcAddress(library, "CorsairGetLedLuidForKeyName"));
        return {};
    }

    ~Impl() {
        if (started)
            Disconnect(); /* Keep SDK code loaded until process exit, including its internal threads. */
    }
};

IcueSdk::IcueSdk(QString libraryPath) : p(std::make_unique<Impl>()) {
    p->libraryPath = libraryPath;
}

IcueSdk::~IcueSdk() = default;

QJsonObject IcueSdk::poll(bool enabled) {
    QJsonObject result{
        {"enabled", enabled}, {"connected", false}, {"status", "Disabled"}, {"devices", QJsonArray{}}};
    if (!enabled) {
        if (p->started)
            p->Disconnect();
        p->started = false;
        p->state = CSS_Closed;
        return result;
    }
    const auto error = p->load();
    if (!error.isEmpty()) {
        result["status"] = error;
        return result;
    }
    if (!p->started) {
        const auto code = p->Connect(&Impl::callback, p.get());
        if (code != CE_Success) {
            result["status"] = failure(code);
            return result;
        }
        p->started = true;
    }
    if (p->state != CSS_Connected) {
        result["status"] =
            QString(
                "iCUE SDK session %1: start iCUE and enable integrations in Settings > SDK. If 'SDK: New Client Access Confirmation' appears, approve WHAT in iCUE.")
                .arg(p->state.load());
        return result;
    }
    // Query on the SDK worker, never from the callback or GUI thread.
    CorsairSessionDetails details{};
    if (p->GetSessionDetails && p->GetSessionDetails(&details) == CE_Success) {
        auto version = [](CorsairVersion value) {
            return value.major || value.minor || value.patch
                ? QString("%1.%2.%3").arg(value.major).arg(value.minor).arg(value.patch) : QString();
        };
        result["app_version"] = version(details.serverHostVersion);
        result["sdk_version"] = version(details.clientVersion);
        result["server_version"] = version(details.serverVersion);
    }
    CorsairDeviceInfo devices[CORSAIR_DEVICE_COUNT_MAX]{};
    CorsairDeviceFilter filter{int(CDT_All)};
    int count = 0;
    auto code = p->GetDevices(&filter, CORSAIR_DEVICE_COUNT_MAX, devices, &count);
    if (code != CE_Success) {
        result["status"] = failure(code);
        return result;
    }
    QJsonArray rows;
    for (int i = 0; i < qBound(0, count, 64); ++i) {
        const auto &device = devices[i];
        const QString id = QString::fromUtf8(device.id, strnlen(device.id, 128));
        if (id.isEmpty())
            continue;
        QJsonObject row{{"id", id},
                        {"model", QString::fromUtf8(device.model, strnlen(device.model, 128))},
                        {"type", int(device.type)},
                        {"channels", qBound(0, device.channelCount, 512)}};
        CorsairLedPosition positions[CORSAIR_DEVICE_LEDCOUNT_MAX]{};
        CorsairLedColor colors[CORSAIR_DEVICE_LEDCOUNT_MAX]{};
        int leds = 0;
        QJsonArray ledRows;
        if (p->GetLedPositions(device.id, 512, positions, &leds) == CE_Success) {
            leds = qBound(0, leds, 512);
            for (int j = 0; j < leds; ++j)
                colors[j].id = positions[j].id;
            const bool readable = p->GetLedColors(device.id, leds, colors) == CE_Success;
            for (int j = 0; j < leds; ++j)
                ledRows.append(QJsonObject{
                    {"id", double(colors[j].id)},
                    {"rgb", readable ? QJsonValue(QJsonArray{colors[j].r, colors[j].g, colors[j].b})
                                     : QJsonValue::Null}});
        }
        if (device.type == CDT_Keyboard && p->GetLedLuidForKeyName) {
            for (char key : QByteArray("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")) {
                CorsairLedLuid luid = 0;
                if (p->GetLedLuidForKeyName(device.id, key, &luid) != CE_Success)
                    continue;
                for (int j = 0; j < ledRows.size(); ++j) {
                    auto led = ledRows[j].toObject();
                    if (led["id"].toDouble() == double(luid)) {
                        led["name"] = QString(QChar::fromLatin1(key)) + " key";
                        ledRows[j] = led;
                        break;
                    }
                }
            }
        }
        // SDK LUID: high 16 bits = group (DIY channels 1..3 are groups 11..13), low 16 = 1-based LED index.
        // https://corsairofficial.github.io/cue-sdk/#led-identification
        int mapped = 0;
        for (int channel = 0; channel < qMin(device.channelCount, 3); ++channel) {
            auto arrayProperty = [&](CorsairDevicePropertyId property) {
                QList<int> result;
                CorsairProperty value{};
                if (p->ReadDeviceProperty(device.id, property, channel, &value) != CE_Success)
                    return result;
                if (value.type == CT_Int32_Array && value.value.int32_array.count <= 512 &&
                    value.value.int32_array.items)
                    for (unsigned j = 0; j < value.value.int32_array.count; ++j)
                        result.append(value.value.int32_array.items[j]);
                p->FreeProperty(&value);
                return result;
            };
            const auto sizes = arrayProperty(CDPI_ChannelDeviceLedCountArray),
                       types = arrayProperty(CDPI_ChannelDeviceTypeArray);
            if (sizes.isEmpty() || sizes.size() != types.size())
                continue;
            int total = 0;
            bool valid = true;
            for (int size : sizes) {
                if (size < 0 || size > 512 || total > 512 - size) {
                    valid = false;
                    break;
                }
                total += size;
            }
            // Require a complete, contiguous channel before attributing LEDs to a physical device.
            QSet<int> indices;
            for (auto value : ledRows) {
                const auto luid = quint32(value.toObject()["id"].toDouble());
                if ((luid >> 16) == quint32(11 + channel))
                    indices.insert(luid & 65535);
            }
            if (!valid || indices.size() != total)
                continue;
            for (int j = 1; j <= total; ++j)
                if (!indices.contains(j))
                    valid = false;
            if (!valid)
                continue;
            const QMap<int, QString> names{
                {1, "HD fan"}, {2, "SP fan"},         {3, "LL fan"},       {4, "ML fan"},
                {5, "QL fan"}, {6, "8-LED fan"},      {7, "LED strip"},    {8, "Lighting accessory"},
                {9, "Pump"},   {10, "Memory module"}, {11, "Water block"}, {12, "QX fan"}};
            int start = 1;
            for (int unit = 0; unit < sizes.size(); ++unit) {
                const auto segment = QString("ch%1-device%2").arg(channel + 1).arg(unit + 1);
                const auto name = QString("Channel %1 · %2 %3")
                                      .arg(channel + 1)
                                      .arg(names.value(types[unit], "Device"))
                                      .arg(unit + 1);
                for (int j = 0; j < ledRows.size(); ++j) {
                    auto led = ledRows[j].toObject();
                    const auto luid = quint32(led["id"].toDouble());
                    const int index = luid & 65535;
                    if ((luid >> 16) != quint32(11 + channel) || index < start ||
                        index >= start + sizes[unit])
                        continue;
                    led["segment"] = segment;
                    led["segment_name"] = name;
                    led["segment_led"] = index - start + 1;
                    ledRows[j] = led;
                    ++mapped;
                }
                start += sizes[unit];
            }
        }
        if (device.channelCount > 0)
            row["topology_status"] =
                QString("%1 of %2 LEDs assigned to connected devices using SDK channel metadata")
                    .arg(mapped)
                    .arg(ledRows.size());
        row["leds"] = ledRows;
        QJsonArray properties;
        for (int property : {2, 3, 4, 5, 6, 7, 9}) {
            CorsairDataType type;
            unsigned int flags = 0;
            if (p->GetDevicePropertyInfo(device.id, CorsairDevicePropertyId(property), 0, &type, &flags) !=
                    CE_Success ||
                !(flags & CPF_CanRead))
                continue;
            CorsairProperty value{};
            if (p->ReadDeviceProperty(device.id, CorsairDevicePropertyId(property), 0, &value) != CE_Success)
                continue;
            QJsonValue data = QJsonValue::Null;
            if (value.type == CT_Boolean)
                data = value.value.boolean;
            else if (value.type == CT_Int32)
                data = value.value.int32;
            p->FreeProperty(&value);
            if (!data.isNull())
                properties.append(
                    QJsonObject{{"id", property},
                                {"name", propertyName(property)},
                                {"value", data},
                                {"writable", bool(flags & CPF_CanWrite) && property >= 2 && property <= 5}});
        }
        row["properties"] = properties;
        rows.append(row);
    }
    result["connected"] = true;
    result["status"] = QString("Connected to iCUE: %1 devices").arg(rows.size());
    result["devices"] = rows;
    return result;
}

QString IcueSdk::validate(const QString &action, const QJsonObject &args) {
    if (!QStringList{"icue_colors", "icue_release", "icue_property"}.contains(action))
        return "Unknown iCUE command";
    const auto id = args["device"].toString();
    if (id.isEmpty() || id.toUtf8().size() > 127 || id.contains(QChar(0)))
        return "Invalid iCUE device ID";
    const QSet<QString> keys = action == "icue_colors"     ? QSet<QString>{"device", "leds", "rgb"}
                               : action == "icue_property" ? QSet<QString>{"device", "property", "value"}
                                                           : QSet<QString>{"device"};
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!keys.contains(it.key()))
            return "Unexpected iCUE argument";
    if (action == "icue_colors") {
        if (!args["rgb"].isArray() || args["rgb"].toArray().size() != 3)
            return "RGB must contain three values";
        for (auto value : args["rgb"].toArray())
            if (!integer(value, 0, 255))
                return "RGB channels must be integers from 0 to 255";
        if (args.contains("leds")) {
            if (!args["leds"].isArray() || args["leds"].toArray().isEmpty() ||
                args["leds"].toArray().size() > 512)
                return "Select 1–512 LEDs or omit leds for the whole device";
            QSet<quint32> seen;
            for (auto value : args["leds"].toArray()) {
                if (!integer(value, 0, 4294967295.0) || seen.contains(quint32(value.toDouble())))
                    return "Invalid or duplicate LED ID";
                seen.insert(quint32(value.toDouble()));
            }
        }
    }
    if (action == "icue_property") {
        if (!integer(args["property"], 2, 5))
            return "Only microphone, surround, sidetone and equalizer controls are supported";
        if (args["property"].toInt() == 5 ? !integer(args["value"], 1, 5) : !args["value"].isBool())
            return "Invalid device property value";
    }
    return {};
}

QString IcueSdk::command(const QString &action, const QJsonObject &args) {
    const auto error = validate(action, args);
    if (!error.isEmpty())
        return error;
    if (!p->started || p->state != CSS_Connected)
        return "iCUE is not connected";
    const auto id = args["device"].toString().toUtf8();
    if (action == "icue_property") {
        const auto property = CorsairDevicePropertyId(args["property"].toInt());
        CorsairDataType type;
        unsigned int flags = 0;
        auto code = p->GetDevicePropertyInfo(id.constData(), property, 0, &type, &flags);
        if (code != CE_Success)
            return failure(code);
        if (!(flags & CPF_CanWrite))
            return "This iCUE property is read-only";
        CorsairProperty value{};
        value.type = type;
        if (type == CT_Boolean && args["value"].isBool())
            value.value.boolean = args["value"].toBool();
        else if (type == CT_Int32 && property == CDPI_EqualizerPreset)
            value.value.int32 = args["value"].toInt();
        else
            return "Unexpected iCUE property type";
        return failure(p->WriteDeviceProperty(id.constData(), property, 0, &value));
    }
    CorsairLedPosition positions[512]{};
    int count = 0;
    const auto code = p->GetLedPositions(id.constData(), 512, positions, &count);
    if (code != CE_Success)
        return failure(code);
    QSet<quint32> available, wanted;
    for (int i = 0; i < qBound(0, count, 512); ++i)
        available.insert(positions[i].id);
    for (auto v : args["leds"].toArray())
        wanted.insert(quint32(v.toDouble()));
    if (wanted.isEmpty())
        wanted = available;
    if (wanted.isEmpty())
        return "No controllable LEDs on this device";
    for (auto led : wanted)
        if (!available.contains(led))
            return "LED ID is no longer available";
    QList<CorsairLedColor> colors;
    const auto rgb = args["rgb"].toArray();
    for (auto led : wanted)
        colors.append(CorsairLedColor{led, static_cast<unsigned char>(rgb.size() == 3 ? rgb[0].toInt() : 0),
                                      static_cast<unsigned char>(rgb.size() == 3 ? rgb[1].toInt() : 0),
                                      static_cast<unsigned char>(rgb.size() == 3 ? rgb[2].toInt() : 0),
                                      static_cast<unsigned char>(action == "icue_release" ? 0 : 255)});
    return failure(p->SetLedColors(id.constData(), colors.size(), colors.constData()));
}
