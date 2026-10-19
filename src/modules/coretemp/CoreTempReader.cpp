#include "CoreTempReader.h"
#include <QJsonArray>
#include <QtEndian>
#include <windows.h>
#include <cmath>
#include <cstring>
#include <limits>

// Independent reader of ALCPU's documented, 4-byte-aligned shared-memory ABI.
// https://www.alcpu.com/CoreTemp/developers.html
namespace CoreTemp {
QJsonObject parse(const QByteArray &bytes, bool extended) {
    auto invalid = [] {
        return QJsonObject{{"connected", false},
                           {"status", "Core Temp shared memory is incomplete or invalid"}};
    };
    if (bytes.size() < 2688)
        return invalid();
    auto u32 = [&](int offset) { return qFromLittleEndian<quint32>(bytes.constData() + offset); };
    auto f32 = [&](int offset) {
        quint32 bits = u32(offset);
        float value;
        std::memcpy(&value, &bits, 4);
        return double(value);
    };
    const auto cores = u32(1536), cpus = u32(1540);
    if (!cores || !cpus || cpus > 128 || cores > 256 || quint64(cores) * cpus > 256)
        return invalid();
    const auto fahrenheit = quint8(bytes[2684]), delta = quint8(bytes[2685]);
    if (fahrenheit > 1 || delta > 1)
        return invalid();
    if (extended && bytes.size() < 2692)
        return invalid();
    const auto version = extended ? u32(2688) : 1;
    if (extended && version >= 2 && bytes.size() < 4740)
        return invalid();
    QJsonArray rows;
    auto add = [&](QString id, QString group, QString name, QString unit, double value, double min,
                   double max, bool recommended = false) {
        if (!std::isfinite(value) || value < min || value > max)
            return;
        rows.append(QJsonObject{{"id", id},
                                {"source", "coretemp"},
                                {"group", group},
                                {"name", name},
                                {"unit", unit},
                                {"value", value},
                                {"kind", "number"},
                                {"type", 0},
                                {"available", true},
                                {"state_class", "measurement"},
                                {"recommended", recommended}});
    };
    for (quint32 cpu = 0; cpu < cpus; ++cpu) {
        const auto group = QString("CPU %1").arg(cpu + 1), prefix = QString("coretemp_cpu_%1_").arg(cpu);
        const auto tjmax = u32(1024 + cpu * 4);
        add(prefix + "tjmax", group, "TjMax", QString::fromUtf8("°C"), tjmax, 1, 200);
        double highestTemperature = -std::numeric_limits<double>::infinity();
        for (quint32 core = 0; core < cores; ++core) {
            const auto index = cpu * cores + core;
            const auto key = prefix + QString("core_%1_").arg(core);
            add(key + "load", group, QString("Core %1 load").arg(core + 1), "%", u32(index * 4), 0, 100);
            // Preserve delta mode explicitly instead of mislabelling distance as temperature.
            const double rawTemperature = f32(1544 + index * 4);
            const double temperature = fahrenheit ? (rawTemperature - (delta ? 0 : 32)) * 5 / 9
                                                  : rawTemperature;
            add(key + (delta ? "distance_to_tjmax" : "temperature"), group,
                QString("Core %1 %2").arg(core + 1).arg(delta ? "distance to TjMax" : "temperature"),
                QString::fromUtf8("°C"),
                temperature,
                delta ? 0 : -273.15, 300, !delta);
            // Aggregate this sample on the collection worker. Delta mode reports
            // a distance, so subtract it from this CPU's TjMax first.
            if (std::isfinite(temperature) && temperature >= (delta ? 0 : -273.15) &&
                temperature <= 300 && (!delta || (tjmax >= 1 && tjmax <= 200))) {
                const double absoluteTemperature = delta ? tjmax - temperature : temperature;
                if (absoluteTemperature >= -273.15)
                    highestTemperature = qMax(highestTemperature, absoluteTemperature);
            }
            if (version >= 2)
                add(key + "multiplier", group, QString("Core %1 multiplier").arg(core + 1), "",
                    f32(3716 + index * 4), 0.01, 1000);
        }
        add(prefix + "highest_core_temperature", group, "Highest Core Temperature",
            QString::fromUtf8("°C"), highestTemperature, -273.15, 300, true);
        if (version >= 2) {
            if (quint8(bytes[2686]) == 1)
                add(prefix + "tdp", group, "TDP", "W", u32(2692 + cpu * 4), 1, 5000);
            if (quint8(bytes[2687]) == 1)
                add(prefix + "power", group, "Power", "W", f32(3204 + cpu * 4), 0, 5000);
        }
    }
    add("coretemp_vid", "CPU", "VID", "V", f32(2568), 0.01, 5);
    add("coretemp_speed", "CPU", "CPU clock", "MHz", f32(2572), 0.01, 20000);
    add("coretemp_bus_speed", "CPU", "Bus clock", "MHz", f32(2576), 0.01, 10000);
    add("coretemp_multiplier", "CPU", "Multiplier", "", f32(2580), 0.01, 1000);
    return {{"connected", true},
            {"shared_memory_version", int(version)},
            {"status", QString("Core Temp connected - %1 readings").arg(rows.size())},
            {"readings", rows}};
}

QJsonObject read() {
    // Open and close each sample: process exit must not leave our mapping handle
    // keeping an obsolete Core Temp mapping alive.
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"CoreTempMappingObjectEx");
    bool extended = true;
    if (!mapping) {
        extended = false;
        mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"CoreTempMappingObject");
    }
    if (!mapping)
        return {{"connected", false}, {"status", "Start Core Temp and enable shared memory in its settings"}};
    SIZE_T size = extended ? 4740 : 2688;
    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, size);
    if (!view && extended) {
        size = 2692;
        view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, size);
    }
    QByteArray bytes;
    if (view) {
        bytes = QByteArray(static_cast<const char *>(view), int(size));
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return parse(bytes, extended);
}
} // namespace CoreTemp
