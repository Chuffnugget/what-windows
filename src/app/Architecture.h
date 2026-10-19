#pragma once
#include <QString>
#include <QVariantMap>

namespace Architecture {
#if defined(_M_ARM64) || defined(__aarch64__)
inline constexpr bool arm64 = true;
#else
inline constexpr bool arm64 = false;
#endif

inline QString name() {
    return arm64 ? QStringLiteral("arm64") : QStringLiteral("x64");
}

inline QString updateManifest() {
    // Keep the existing x64 asset name for installed versions of WHAT.
    return arm64 ? QStringLiteral("update-arm64.json") : QStringLiteral("update.json");
}

inline const QVariantMap &unavailableModules() {
    // This is WHAT's supported module set, not a claim that external x64 apps
    // cannot run under Windows emulation. Never load their DLLs into ARM64 WHAT.
    static const QVariantMap modules = arm64 ? QVariantMap{
        {"beacn", "BEACN is unavailable in native ARM64 WHAT. Its API DLL and volume hook require x64."},
        {"icue", "iCUE is unavailable in native ARM64 WHAT. The bundled RGB SDK requires x64; Automation is also disabled in this build."},
        {"coretemp", "Core Temp is not supported by WHAT's ARM64 build. Use HWiNFO ARM64 for hardware temperatures."},
        {"nvidia", "NVIDIA monitoring is not supported by WHAT's ARM64 build. A compatible native Windows NVML driver is required."},
        {"wallpaper", "Wallpaper Engine is not supported by WHAT's ARM64 build. Its Windows integration targets x86/x64."}
    } : QVariantMap{};
    return modules;
}

inline bool moduleAvailable(const QString &module) {
    return !unavailableModules().contains(module);
}
}
