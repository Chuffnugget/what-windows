#include "SpecsHardware.h"
#include <windows.h>
#include <dxgi1_2.h>
#include <dxcore_interface.h>
#include <tbs.h>
#include <wrl/client.h>
#include <QLibrary>
#include <QSettings>
#include <QStringList>

using Microsoft::WRL::ComPtr;

namespace {
QString unavailable(const char *query, unsigned long code) {
    wchar_t message[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, message, DWORD(std::size(message)), nullptr);
    const auto reason = QString::fromWCharArray(message).simplified();
    return QString("Unavailable (%1 returned 0x%2%3)")
        .arg(query).arg(code, 8, 16, QLatin1Char('0'))
        .arg(reason.isEmpty() ? QString() : ": " + reason);
}
}

QString graphicsSpecifications() {
    QStringList lines{"Graphics adapters (Windows-reported)"};
    ComPtr<IDXGIFactory1> factory;
    const auto factoryResult = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factoryResult))
        return "Graphics\n" + unavailable("CreateDXGIFactory1", factoryResult);

    // DXCore is optional on older Windows. Its driver-reported IsIntegrated flag
    // avoids guessing the GPU type from the manufacturer or memory size.
    wchar_t systemPath[MAX_PATH]{};
    const auto pathLength = GetSystemDirectoryW(systemPath, MAX_PATH);
    QLibrary library;
    ComPtr<IDXCoreAdapterFactory> core;
    if (pathLength > 0 && pathLength < MAX_PATH) {
        library.setFileName(QString::fromWCharArray(systemPath) + "/DXCore.dll");
        using CreateCoreFactory = HRESULT(WINAPI *)(REFIID, void **);
        const auto create =
            reinterpret_cast<CreateCoreFactory>(library.resolve("DXCoreCreateAdapterFactory"));
        if (create)
            create(IID_PPV_ARGS(&core));
    }

    for (UINT index = 0; index < 64; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(result)) {
            lines << unavailable("EnumAdapters1", result);
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        const auto descriptionResult = adapter->GetDesc1(&description);
        if (FAILED(descriptionResult)) {
            lines << QString("Graphics adapter %1: ").arg(index) + unavailable("GetDesc1", descriptionResult);
            continue;
        }
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        QString kind = "GPU type unavailable (DXCore did not provide the IsIntegrated property)";
        ComPtr<IDXCoreAdapter> detail;
        bool integrated = false;
        if (core && SUCCEEDED(core->GetAdapterByLuid(description.AdapterLuid, IID_PPV_ARGS(&detail))) &&
            detail->IsPropertySupported(DXCoreAdapterProperty::IsIntegrated) &&
            SUCCEEDED(detail->GetProperty(DXCoreAdapterProperty::IsIntegrated, &integrated)))
            kind = integrated ? "Integrated" : "Discrete";

        LARGE_INTEGER version{};
        const auto driverResult = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version);
        QString driver = unavailable("CheckInterfaceSupport", driverResult);
        if (SUCCEEDED(driverResult)) {
            const auto number = static_cast<quint64>(version.QuadPart);
            driver = QString("%1.%2.%3.%4")
                         .arg((number >> 48) & 0xffff)
                         .arg((number >> 32) & 0xffff)
                         .arg((number >> 16) & 0xffff)
                         .arg(number & 0xffff);
        }
        lines << QString("%1 | %2 | %3 GiB dedicated VRAM | driver %4")
                     .arg(QString::fromWCharArray(description.Description), kind)
                     .arg(double(description.DedicatedVideoMemory) / (1024.0 * 1024 * 1024), 2, 'f', 2)
                     .arg(driver);
    }
    if (lines.size() == 1)
        lines << "No hardware graphics adapters reported by Windows.";
    return lines.join('\n');
}

QString firmwareSpecifications() {
    FIRMWARE_TYPE type = FirmwareTypeUnknown;
    const bool known = GetFirmwareType(&type);
    QString boot = known ? "Unknown firmware type" : unavailable("GetFirmwareType", GetLastError());
    if (known && type == FirmwareTypeUefi)
        boot = "UEFI";
    else if (known && type == FirmwareTypeBios)
        boot = "Legacy BIOS";

    QString secure = "Unavailable (Windows did not expose a Secure Boot state)";
    if (known && type == FirmwareTypeBios)
        secure = "Not supported in legacy BIOS mode";
    else if (known && type == FirmwareTypeUefi) {
        QSettings state("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
                        QSettings::NativeFormat);
        const auto enabled = state.value("UEFISecureBootEnabled");
        if (enabled.isValid())
            secure = enabled.toUInt() ? "Enabled" : "Disabled";
    }
    // TPM Base Services reports presence/version without requiring an elevated
    // WMI query. A disabled or absent TPM is distinct from a query failure.
    TPM_DEVICE_INFO device{};
    const auto result = Tbsi_GetDeviceInfo(sizeof(device), &device);
    QString tpm = unavailable("Tbsi_GetDeviceInfo", result);
    if (result == TBS_SUCCESS) {
        const auto version = device.tpmVersion == TPM_VERSION_20   ? "2.0"
                             : device.tpmVersion == TPM_VERSION_12 ? "1.2"
                                                                   : "version unavailable";
        tpm = "Present, " + QString::fromLatin1(version);
    } else if (result == TBS_E_TPM_NOT_FOUND) {
        tpm = "Not detected (absent or disabled in firmware)";
    }
    return "Boot and security\nBoot mode: " + boot + "\nSecure Boot: " + secure + "\nTPM: " + tpm;
}
