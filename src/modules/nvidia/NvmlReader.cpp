#include "NvmlReader.h"
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDir>
#include <QScopeGuard>
#include <windows.h>
#include <shlobj.h>
#include <algorithm>

namespace Nvidia {
static QString errorText(const Api &a, int rc) {
    return a.error ? QString::fromUtf8(a.error(rc)).left(256) : QString("NVML error %1").arg(rc);
}

QJsonObject collect(const Api &a) {
    QJsonArray rows, devices;
    QStringList errors;
    if (!a.count || !a.handle || !a.uuid || !a.name)
        return {
            {"connected", false}, {"status", "NVML is missing required query functions"}, {"readings", rows}};
    unsigned count = 0;
    int rc = a.count(&count);
    if (rc != 0)
        return {{"connected", false},
                {"status", "GPU enumeration failed: " + errorText(a, rc)},
                {"readings", rows}};
    char driver[128]{};
    if (a.driver && a.driver(driver, sizeof(driver)) != 0)
        driver[0] = 0;
    for (unsigned i = 0; i < std::min(count, 64u); ++i) {
        Device gpu = nullptr;
        rc = a.handle(i, &gpu);
        if (rc || !gpu) {
            errors << QString("GPU %1: handle unavailable (%2)").arg(i).arg(rc);
            continue;
        }
        char uuid[128]{}, name[128]{};
        if (a.uuid(gpu, uuid, sizeof(uuid)) != 0 || !uuid[0]) {
            errors << QString("GPU %1: stable UUID unavailable").arg(i);
            continue;
        }
        if (a.name(gpu, name, sizeof(name)) != 0)
            strcpy_s(name, "NVIDIA GPU");
        uuid[127] = 0;
        name[127] = 0;
        const QString identity = QString::fromUtf8(uuid), model = QString::fromUtf8(name);
        const QString prefix =
            "nvidia_" +
            QString(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
        const QString group = model + " [" + identity.right(8) + "]";
        QJsonArray failures, unsupported;
        int supported = 0;
        auto add = [&](QString suffix, QString label, QJsonValue value, QString unit, int result,
                       bool recommended = false) {
            // Unsupported is not zero. Keep a discovered row unavailable on temporary failures.
            if (result == 3 || result == 13) {
                unsupported.append(label);
                return;
            }
            const bool ok = result == 0;
            if (!ok)
                failures.append(label + ": " + errorText(a, result));
            else
                ++supported;
            rows.append(QJsonObject{{"id", prefix + "_" + suffix},
                                    {"source", "nvidia"},
                                    {"group", group},
                                    {"name", label},
                                    {"value", ok ? value : QJsonValue(QJsonValue::Null)},
                                    {"available", ok},
                                    {"unit", unit},
                                    {"kind", value.isString() ? "text" : "number"},
                                    {"type", 0},
                                    {"state_class", value.isString() ? "" : "measurement"},
                                    {"recommended", recommended}});
        };
        add("model", "GPU model", model, "", 0);
        add("uuid", "GPU UUID", identity, "", 0);
        if (driver[0])
            add("driver", "Driver version", QString::fromUtf8(driver), "", 0);
        unsigned value = 0;
        if (a.temperature) {
            rc = a.temperature(gpu, 0, &value);
            add("temperature", "GPU temperature", double(value), QString::fromUtf8("°C"), rc, true);
        }
        Utilization utilization{};
        if (a.utilization) {
            rc = a.utilization(gpu, &utilization);
            add("utilization", "GPU utilisation", double(utilization.gpu), "%", rc, true);
            add("memory_activity", "Memory controller utilisation", double(utilization.memory), "%", rc);
        }
        Memory memory{};
        if (a.memory) {
            rc = a.memory(gpu, &memory);
            add("memory_total", "VRAM total", double(memory.total), "B", rc);
            add("memory_used", "VRAM used", double(memory.used), "B", rc, true);
            add("memory_free", "VRAM free", double(memory.free), "B", rc);
        }
        if (a.fan) {
            rc = a.fan(gpu, &value);
            add("fan", "Fan speed", double(value), "%", rc);
        }
        if (a.power) {
            rc = a.power(gpu, &value);
            add("power", "GPU power", value / 1000.0, "W", rc, true);
        }
        if (a.powerLimit) {
            rc = a.powerLimit(gpu, &value);
            add("power_limit", "Power limit", value / 1000.0, "W", rc);
        }
        if (a.clock) {
            rc = a.clock(gpu, 0, &value);
            add("graphics_clock", "Graphics clock", double(value), "MHz", rc);
            rc = a.clock(gpu, 2, &value);
            add("memory_clock", "Memory clock", double(value), "MHz", rc);
        }
        unsigned period = 0;
        if (a.encoder) {
            rc = a.encoder(gpu, &value, &period);
            add("encoder", "Encoder utilisation", double(value), "%", rc);
        }
        if (a.decoder) {
            rc = a.decoder(gpu, &value, &period);
            add("decoder", "Decoder utilisation", double(value), "%", rc);
        }
        devices.append(QJsonObject{{"uuid", identity},
                                   {"name", model},
                                   {"readings", supported},
                                   {"errors", failures},
                                   {"unsupported", unsupported}});
    }
    return {{"connected", !devices.isEmpty()},
            {"driver_version", QString::fromUtf8(driver, int(strnlen(driver, sizeof(driver))))},
            {"status", devices.isEmpty() ? "No accessible NVIDIA GPUs"
                                         : QString("Connected: %1 NVIDIA GPU(s)").arg(devices.size())},
            {"devices", devices},
            {"errors", QJsonArray::fromStringList(errors)},
            {"readings", rows}};
}

QJsonObject read() {
    // Absolute driver paths only; never search the current directory or PATH.
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    QStringList paths{QString::fromWCharArray(system) + "/nvml.dll"};
    PWSTR program = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &program))) {
        paths << QString::fromWCharArray(program) + "/NVIDIA Corporation/NVSMI/nvml.dll";
        CoTaskMemFree(program);
    }
    HMODULE library = nullptr;
    QString loaded;
    for (const auto &path : paths) {
        library = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library) {
            loaded = path;
            break;
        }
    }
    if (!library)
        return {{"connected", false},
                {"status",
                 "NVIDIA driver NVML library unavailable; install or update the NVIDIA graphics driver"},
                {"readings", QJsonArray{}}};
    auto unload = qScopeGuard([&] { FreeLibrary(library); });
    auto init = reinterpret_cast<int (*)()>(GetProcAddress(library, "nvmlInit_v2"));
    auto shutdown = reinterpret_cast<int (*)()>(GetProcAddress(library, "nvmlShutdown"));
    if (!init || !shutdown)
        return {{"connected", false},
                {"status", "NVML initialization functions missing"},
                {"readings", QJsonArray{}}};
    Api api;
#define RESOLVE(field, symbol)                                                                               \
    api.field = reinterpret_cast<decltype(api.field)>(GetProcAddress(library, symbol))
    RESOLVE(count, "nvmlDeviceGetCount_v2");
    RESOLVE(handle, "nvmlDeviceGetHandleByIndex_v2");
    RESOLVE(name, "nvmlDeviceGetName");
    RESOLVE(uuid, "nvmlDeviceGetUUID");
    RESOLVE(driver, "nvmlSystemGetDriverVersion");
    RESOLVE(temperature, "nvmlDeviceGetTemperature");
    RESOLVE(utilization, "nvmlDeviceGetUtilizationRates");
    RESOLVE(memory, "nvmlDeviceGetMemoryInfo");
    RESOLVE(fan, "nvmlDeviceGetFanSpeed");
    RESOLVE(power, "nvmlDeviceGetPowerUsage");
    RESOLVE(powerLimit, "nvmlDeviceGetPowerManagementLimit");
    RESOLVE(clock, "nvmlDeviceGetClockInfo");
    RESOLVE(encoder, "nvmlDeviceGetEncoderUtilization");
    RESOLVE(decoder, "nvmlDeviceGetDecoderUtilization");
    RESOLVE(error, "nvmlErrorString");
#undef RESOLVE
    const int rc = init();
    if (rc)
        return {{"connected", false},
                {"status", "NVML initialization failed: " + errorText(api, rc)},
                {"readings", QJsonArray{}}};
    auto finish = qScopeGuard([&] { shutdown(); });
    auto result = collect(api);
    result["library"] = loaded;
    return result;
}
} // namespace Nvidia
