#pragma once
// Windows Dynamic Lighting runs in a persistent helper so LampArray ownership survives commands.
#include <inspectable.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Lights.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <map>
#include <set>

// ILampArray2's first method, from Windows.Devices.winmd. The local 22000 SDK predates it.
// QueryInterface keeps older Windows versions supported without requiring a newer SDK to build.
struct __declspec(uuid("050C181F-60A8-4711-A1AF-1B1B4C658EA2")) LightingAvailability : ::IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsAvailable(bool *value) = 0;
};

inline int lightingServer() {
    using namespace Windows::Devices::Lights;
    using Windows::Devices::Enumeration::DeviceInformation;

    struct Device {
        LampArray lamps{nullptr};
        hstring name;
        JsonArray rgb;
        double brightness = 100;
        bool known = false;
    };

    std::map<std::wstring, Device> devices;
    auto available = [](const LampArray &lamps) {
        if (!lamps)
            return false;
        com_ptr<LightingAvailability> api;
        reinterpret_cast<::IUnknown *>(get_abi(lamps))
            ->QueryInterface(__uuidof(LightingAvailability), api.put_void());
        bool result = false;
        return api && SUCCEEDED(api->get_IsAvailable(&result)) && result;
    };
    auto scan = [&] {
        std::set<std::wstring> present;
        for (const auto &info : DeviceInformation::FindAllAsync(LampArray::GetDeviceSelector()).get()) {
            if (present.size() >= 64)
                break;
            const auto bytes = Windows::Security::Cryptography::CryptographicBuffer::ConvertStringToBinary(
                info.Id(), Windows::Security::Cryptography::BinaryStringEncoding::Utf8);
            const auto hash =
                Windows::Security::Cryptography::Core::HashAlgorithmProvider::OpenAlgorithm(L"SHA256")
                    .HashData(bytes);
            std::wstring id =
                L"win_lighting_" +
                std::wstring(Windows::Security::Cryptography::CryptographicBuffer::EncodeToHexString(hash))
                    .substr(0, 24);
            std::transform(id.begin(), id.end(), id.begin(),
                           [](wchar_t c) { return c >= L'A' && c <= L'F' ? wchar_t(c + 32) : c; });
            present.insert(id);
            if (!devices.contains(id) || !devices.at(id).lamps) {
                try {
                    auto lamps = LampArray::FromIdAsync(info.Id()).get();
                    devices.insert_or_assign(id, Device{lamps, info.Name()});
                } catch (const hresult_error &) {
                    // Show inaccessible devices without preventing other devices from working.
                    devices.insert_or_assign(id, Device{nullptr, info.Name()});
                }
            }
        }
        for (auto it = devices.begin(); it != devices.end();) {
            if (!present.contains(it->first))
                it = devices.erase(it);
            else
                ++it;
        }
    };
    auto snapshot = [&] {
        JsonArray result;
        for (auto &[id, device] : devices) {
            if (!available(device.lamps))
                device.known = false;
            JsonObject row;
            row.SetNamedValue(L"id", JsonValue::CreateStringValue(id));
            row.SetNamedValue(L"name", text(device.name));
            row.SetNamedValue(L"available", JsonValue::CreateBooleanValue(available(device.lamps)));
            row.SetNamedValue(L"lamp_count",
                              JsonValue::CreateNumberValue(device.lamps ? device.lamps.LampCount() : 0));
            row.SetNamedValue(L"rgb", device.known ? static_cast<IJsonValue>(device.rgb)
                                                   : JsonValue::CreateNullValue());
            row.SetNamedValue(L"brightness", device.known ? JsonValue::CreateNumberValue(device.brightness)
                                                          : JsonValue::CreateNullValue());
            result.Append(row);
        }
        return result;
    };
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    std::string line;
    char byte;
    DWORD count = 0;
    while (ReadFile(input, &byte, 1, &count, nullptr) && count == 1) {
        if (byte != '\n') {
            if (line.size() >= 8192)
                return 2;
            line += byte;
            continue;
        }
        JsonObject reply;
        try {
            const auto request = JsonObject::Parse(to_hstring(line));
            reply.SetNamedValue(L"request", request.GetNamedValue(L"request"));
            const auto operation = request.GetNamedString(L"operation");
            if (operation == L"scan")
                scan();
            else if (operation == L"set") {
                auto it = devices.find(std::wstring(request.GetNamedString(L"device")));
                if (it == devices.end())
                    throw hresult_invalid_argument(L"Lighting device disconnected. Refresh devices.");
                auto &device = it->second;
                if (!available(device.lamps))
                    throw hresult_access_denied(
                        L"Windows has not granted lighting control. Check Dynamic Lighting priority.");
                const auto rgb = request.GetNamedArray(L"rgb");
                const double brightness = request.GetNamedNumber(L"brightness");
                if (rgb.Size() != 3 || !std::isfinite(brightness) || brightness < 0 || brightness > 100)
                    throw hresult_invalid_argument(L"Invalid colour or brightness.");
                for (const auto value : rgb) {
                    const double channel = value.GetNumber();
                    if (!std::isfinite(channel) || channel < 0 || channel > 255 ||
                        std::floor(channel) != channel)
                        throw hresult_invalid_argument(L"Invalid RGB channel.");
                }
                device.lamps.IsEnabled(true);
                device.lamps.BrightnessLevel(brightness / 100.0);
                device.lamps.SetColor(Windows::UI::Color{255, uint8_t(rgb.GetNumberAt(0)),
                                                         uint8_t(rgb.GetNumberAt(1)),
                                                         uint8_t(rgb.GetNumberAt(2))});
                device.rgb = rgb;
                device.brightness = brightness;
                device.known = true;
            } else
                throw hresult_invalid_argument(L"Unsupported lighting operation.");
            reply.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(true));
            reply.SetNamedValue(L"status",
                                text(devices.empty() ? L"No compatible Dynamic Lighting devices found."
                                                     : L"Dynamic Lighting devices detected."));
            reply.SetNamedValue(L"devices", snapshot());
        } catch (const hresult_error &error) {
            reply.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(false));
            reply.SetNamedValue(L"status", text(L"Windows Dynamic Lighting: " + error.message()));
        } catch (...) {
            reply.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(false));
            reply.SetNamedValue(L"status", text(L"Windows lighting request failed."));
        }
        line.clear();
        auto data = to_string(reply.Stringify()) + "\n";
        DWORD written = 0;
        if (!WriteFile(output, data.data(), DWORD(data.size()), &written, nullptr) || written != data.size())
            break;
    }
    // EOF from WHAT (including a crashed parent) releases every LampArray handle.
    for (auto &[id, device] : devices) {
        try {
            if (device.lamps)
                device.lamps.IsEnabled(false);
        } catch (...) {
        }
    }
    return 0;
}
