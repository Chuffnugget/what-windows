#pragma once
#include <windows.h>
#include <shellscalingapi.h>
#include <QJsonObject>
#include <QString>

namespace SpecsDisplayDetails {
inline QString connection(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology) {
    switch (technology) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15:
        return "VGA";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI:
        return "DVI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI:
        return "HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL:
        return "DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
        return "Embedded DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
        return "LVDS";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
        return "Internal";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST:
        return "Miracast";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED:
        return "Indirect wired";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL:
        return "Virtual";
    default:
        return "Unavailable";
    }
}

inline void append(QJsonObject &row, const DISPLAYCONFIG_PATH_INFO &path, const QString &source) {
    row["connection"] = connection(path.targetInfo.outputTechnology);
    if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE))
        return;

    struct ScaleQuery {
        QString source;
        int percent = 0;
        HRESULT result = E_FAIL;
        bool matched = false;
    } scale{source};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
            auto &query = *reinterpret_cast<ScaleQuery *>(context);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info) || query.source != QString::fromWCharArray(info.szDevice))
                return TRUE;
            DEVICE_SCALE_FACTOR factor{};
            query.matched = true;
            query.result = GetScaleFactorForMonitor(monitor, &factor);
            if (SUCCEEDED(query.result))
                query.percent = int(factor);
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&scale));
    if (scale.percent > 0)
        row["scalePercent"] = scale.percent;
    else
        row["scaleError"] = scale.matched
            ? QString("GetScaleFactorForMonitor returned 0x%1").arg(quint32(scale.result), 8, 16, QLatin1Char('0'))
            : "Windows did not match this display to a monitor handle";

    // GET_ADVANCED_COLOR_INFO_2 has this public ABI in newer Windows SDKs.
    // Keep the request local so older SDK headers can still build WHAT. Older
    // Windows returns an unsupported request; WCG alone must not be called HDR.
    struct AdvancedColourInfo {
        DISPLAYCONFIG_DEVICE_INFO_HEADER header;
        UINT32 flags = 0;
        DISPLAYCONFIG_COLOR_ENCODING encoding{};
        UINT32 bitsPerChannel = 0;
        UINT32 activeMode = 0;
    } colour{};

    static_assert(sizeof(AdvancedColourInfo) == 36);
    constexpr auto advancedColourInfo2 = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
    constexpr UINT32 hdrSupported = 1U << 4;
    constexpr UINT32 hdrMode = 2;
    colour.header = {advancedColourInfo2, sizeof(colour), path.targetInfo.adapterId, path.targetInfo.id};
    const auto hdrResult = DisplayConfigGetDeviceInfo(&colour.header);
    if (hdrResult == ERROR_SUCCESS) {
        row["hdrSupported"] = bool(colour.flags & hdrSupported);
        row["hdrEnabled"] = colour.activeMode == hdrMode;
        if (colour.bitsPerChannel > 0)
            row["bitsPerChannel"] = int(colour.bitsPerChannel);
    } else {
        row["hdrError"] = QString("DisplayConfigGetDeviceInfo HDR query returned Windows error %1").arg(hdrResult);
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO basic{};
        basic.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(basic),
                        path.targetInfo.adapterId, path.targetInfo.id};
        const auto basicResult = DisplayConfigGetDeviceInfo(&basic.header);
        if (basicResult == ERROR_SUCCESS && basic.bitsPerColorChannel > 0)
            row["bitsPerChannel"] = int(basic.bitsPerColorChannel);
        else
            row["colourError"] = QString("DisplayConfigGetDeviceInfo colour query returned Windows error %1; bits per channel %2")
                .arg(basicResult).arg(basic.bitsPerColorChannel);
    }
}
} // namespace SpecsDisplayDetails
