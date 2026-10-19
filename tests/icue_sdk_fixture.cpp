#define ICUESDK_EXPORTS
#include "../src/third_party/icue/iCUESDK.h"
#include <cstring>
#include <atomic>
#include <windows.h>
static std::atomic_int disconnectDelay{0}, disconnectStarted{0}, disconnectFinished{0};
static std::atomic<DWORD> disconnectThread{0};
static bool connected = false, mic = true;
static int hubMode = 0;
static CorsairLedColor colors[2]{{11, 1, 2, 3, 255}, {22, 4, 5, 6, 255}};
extern "C" {
__declspec(dllexport) void IcueFixtureHub(int mode) {
    hubMode = mode;
}

CorsairError CorsairConnect(CorsairSessionStateChangedHandler callback, void *context) {
    connected = true;
    CorsairSessionStateChanged event{};
    event.state = CSS_Connected;
    callback(context, &event);
    return CE_Success;
}

__declspec(dllexport) void IcueFixtureDisconnectDelay(int delay) {
    disconnectDelay = delay;
    disconnectStarted = 0;
    disconnectFinished = 0;
}

__declspec(dllexport) int IcueFixtureDisconnectState() {
    return disconnectFinished ? 2 : disconnectStarted ? 1 : 0;
}

__declspec(dllexport) DWORD IcueFixtureDisconnectThread() {
    return disconnectThread;
}

CorsairError CorsairDisconnect() {
    disconnectThread = GetCurrentThreadId();
    disconnectStarted = 1;
    Sleep(DWORD(disconnectDelay.load()));
    connected = false;
    disconnectFinished = 1;
    return CE_Success;
}

CorsairError CorsairGetDevices(const CorsairDeviceFilter *, int, CorsairDeviceInfo *devices, int *size) {
    if (!connected)
        return CE_NotConnected;
    *size = 1;
    devices[0] = {};
    std::strcpy(devices[0].id, "test-device");
    std::strcpy(devices[0].model, "Fixture keyboard");
    devices[0].type = hubMode ? CDT_LedController : CDT_Keyboard;
    devices[0].channelCount = hubMode ? 2 : 0;
    devices[0].ledCount = hubMode ? 5 : 2;
    if (hubMode)
        std::strcpy(devices[0].model, "Fixture LINK Hub");
    return CE_Success;
}

CorsairError CorsairGetLedPositions(const char *id, int, CorsairLedPosition *positions, int *size) {
    if (std::strcmp(id, "test-device"))
        return CE_DeviceNotFound;
    if (hubMode) {
        *size = 5;
        for (int i = 0; i < 4; ++i)
            positions[i] = {unsigned((11 << 16) + i + 1), double(i), 0};
        positions[4] = {unsigned((12 << 16) + 1), 0, 1};
        return CE_Success;
    }
    *size = 2;
    positions[0] = {11, 0, 0};
    positions[1] = {22, 1, 0};
    return CE_Success;
}

CorsairError CorsairGetLedLuidForKeyName(const char *, char key, CorsairLedLuid *led) {
    if (key != 'A')
        return CE_InvalidArguments;
    *led = 11;
    return CE_Success;
}

CorsairError CorsairGetLedColors(const char *, int count, CorsairLedColor *values) {
    for (int i = 0; i < count; ++i)
        for (auto color : colors)
            if (color.id == values[i].id)
                values[i] = color;
    return CE_Success;
}

CorsairError CorsairSetLedColors(const char *, int count, const CorsairLedColor *values) {
    for (int i = 0; i < count; ++i)
        for (auto &color : colors)
            if (color.id == values[i].id)
                color = values[i];
    return CE_Success;
}

CorsairError CorsairGetDevicePropertyInfo(const char *, CorsairDevicePropertyId id, unsigned int,
                                          CorsairDataType *type, unsigned int *flags) {
    if (id != CDPI_MicEnabled && id != CDPI_BatteryLevel)
        return CE_InvalidArguments;
    *type = id == CDPI_MicEnabled ? CT_Boolean : CT_Int32;
    *flags = id == CDPI_MicEnabled ? CPF_CanRead | CPF_CanWrite : CPF_CanRead;
    return CE_Success;
}

CorsairError CorsairReadDeviceProperty(const char *, CorsairDevicePropertyId id, unsigned int channel,
                                       CorsairProperty *value) {
    if (id == CDPI_ChannelDeviceLedCountArray || id == CDPI_ChannelDeviceTypeArray) {
        if (!hubMode || channel > 1)
            return CE_InvalidArguments;
        value->type = CT_Int32_Array;
        if (channel == 0)
            value->value.int32_array = {
                new int[2]{id == CDPI_ChannelDeviceLedCountArray ? (hubMode == 2 ? 3 : 2) : 12,
                           id == CDPI_ChannelDeviceLedCountArray ? 2 : 9},
                2};
        else
            value->value.int32_array = {new int[1]{id == CDPI_ChannelDeviceLedCountArray ? 1 : 7}, 1};
        return CE_Success;
    }
    value->type = id == CDPI_MicEnabled ? CT_Boolean : CT_Int32;
    if (id == CDPI_MicEnabled)
        value->value.boolean = mic;
    else
        value->value.int32 = 87;
    return CE_Success;
}

CorsairError CorsairWriteDeviceProperty(const char *, CorsairDevicePropertyId, unsigned int,
                                        const CorsairProperty *value) {
    mic = value->value.boolean;
    return CE_Success;
}

CorsairError CorsairFreeProperty(CorsairProperty *value) {
    if (value->type == CT_Int32_Array)
        delete[] value->value.int32_array.items;
    return CE_Success;
}
}
