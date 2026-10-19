#include "SpecsHardware.h"
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <devguid.h>
#include <dxgi1_2.h>
#include <dxcore_interface.h>
#include <tbs.h>
#include <slpublic.h>
#include <wrl/client.h>
#include <QLibrary>
#include <QMap>
#include <QSettings>
#include <QStringList>
#include <QScopeGuard>

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

QString deviceText(DEVINST device, const DEVPROPKEY &key) {
    wchar_t buffer[1024]{};
    ULONG size = sizeof(buffer);
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(device, &key, &type, reinterpret_cast<PBYTE>(buffer), &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING)
        return {};
    buffer[std::size(buffer) - 1] = L'\0';
    return QString::fromWCharArray(buffer).simplified();
}
}

QString inputDeviceName(const QString &friendlyName, const QString &description, const QString &productName) {
    static const QStringList generic{
        "usb receiver", "wireless receiver", "usb input device", "bluetooth input device",
        "hid keyboard device", "hid-compliant mouse", "hid-compliant device", "hid-compliant touch pad",
        "hid-compliant touch screen", "keyboard", "mouse", "standard ps/2 keyboard",
        "ps/2 compatible mouse", "rdp keyboard device", "rdp mouse device"};
    for (const auto &candidate : {productName, friendlyName, description}) {
        const auto name = candidate.simplified();
        if (!name.isEmpty() && !generic.contains(name.toLower()) &&
            !name.contains("virtual", Qt::CaseInsensitive) && !name.contains("virhid", Qt::CaseInsensitive))
            return name;
    }
    return {};
}

QString inputSpecifications() {
    QStringList lines;
    for (const auto &category : {qMakePair(GUID_DEVCLASS_KEYBOARD, QString("Keyboard")),
                                 qMakePair(GUID_DEVCLASS_MOUSE, QString("Mouse / pointing device"))}) {
        const auto devices = SetupDiGetClassDevsW(&category.first, nullptr, nullptr, DIGCF_PRESENT);
        if (devices == INVALID_HANDLE_VALUE) {
            lines << category.second + ": " + unavailable("SetupDiGetClassDevs", GetLastError());
            continue;
        }
        QMap<QString, int> names;
        for (DWORD index = 0;; ++index) {
            SP_DEVINFO_DATA device{};
            device.cbSize = sizeof(device);
            if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
                if (GetLastError() != ERROR_NO_MORE_ITEMS)
                    lines << category.second + ": " + unavailable("SetupDiEnumDeviceInfo", GetLastError());
                break;
            }
            const auto friendlyName = deviceText(device.DevInst, DEVPKEY_Device_FriendlyName);
            const auto description = deviceText(device.DevInst, DEVPKEY_Device_DeviceDesc);

            // HID children often say only "HID Keyboard Device". The immediate
            // parent usually carries the product name; do not walk up to USB hubs.
            auto product = deviceText(device.DevInst, DEVPKEY_Device_BusReportedDeviceDesc);
            DEVINST parent = 0;
            if (product.isEmpty() && CM_Get_Parent(&parent, device.DevInst, 0) == CR_SUCCESS)
                product = deviceText(parent, DEVPKEY_Device_BusReportedDeviceDesc);
            const auto name = inputDeviceName(friendlyName, description, product);
            if (!name.isEmpty())
                ++names[name];
        }
        SetupDiDestroyDeviceInfoList(devices);
        for (auto it = names.cbegin(); it != names.cend(); ++it)
            lines << category.second + ": " + it.key()
                + (it.value() > 1 ? QString(" (%1 interfaces)").arg(it.value()) : QString());
    }
    return lines.isEmpty() ? QString() : "Input devices\n" + lines.join('\n');
}

QString windowsActivationStatus() {
    HSLC context = nullptr;
    const auto opened = SLOpen(&context);
    if (FAILED(opened))
        return unavailable("SLOpen", opened);
    const auto close = qScopeGuard([context] { SLClose(context); });

    // Ask only about Windows, not Office or other licensed applications. This
    // reads local activation state; it neither activates nor verifies online.
    const SLID windowsId{0x55c92734, 0xd682, 0x4d71, {0x98, 0x3e, 0xd6, 0xec, 0x3f, 0x16, 0x05, 0x9f}};
    UINT count = 0;
    SL_LICENSING_STATUS *states = nullptr;
    const auto result = SLGetLicensingStatusInformation(context, &windowsId, nullptr, nullptr, &count, &states);
    const auto release = qScopeGuard([states] { SLFreeMemory(states); });
    if (FAILED(result))
        return unavailable("SLGetLicensingStatusInformation", result);
    if (!states || count == 0)
        return "Unavailable (Windows returned no activation status)";

    bool grace = false, notification = false, unlicensed = false;
    for (UINT i = 0; i < count; ++i) {
        if (states[i].eStatus == SL_LICENSING_STATUS_LICENSED)
            return "Activated";
        grace |= states[i].eStatus == SL_LICENSING_STATUS_IN_GRACE_PERIOD;
        notification |= states[i].eStatus == SL_LICENSING_STATUS_NOTIFICATION;
        unlicensed |= states[i].eStatus == SL_LICENSING_STATUS_UNLICENSED;
    }
    if (notification)
        return "Not activated (notification mode)";
    if (grace)
        return "Activation grace period";
    return unlicensed ? "Not activated" : "Unavailable (unrecognised licensing state)";
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
