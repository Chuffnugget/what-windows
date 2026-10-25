#include "NativeLayout.h"
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>

struct Command {
    DWORD magic, pid;
    volatile LONG state;
    DWORD mode;
    std::uint64_t object;
    float expected, target, observed;
    DWORD error;
    char sourceName[128];
};

static bool read(const void *p, void *out, SIZE_T size) {
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, size, &n) && n == size;
}

template <class T> static T at(std::uintptr_t p) {
    T v{};
    read((void *)p, &v, sizeof(v));
    return v;
}

extern "C" __declspec(dllexport) LRESULT CALLBACK VolumeHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        auto msg = reinterpret_cast<CWPSTRUCT *>(lp);
        static const UINT volumeMessage = RegisterWindowMessageW(L"BEACN.WHAT.Volume.142");
        if (msg && volumeMessage && msg->message == volumeMessage) {
            wchar_t name[100];
            swprintf(name, 100, L"Local\\BEACN.WHAT.Volume142.%lu", GetCurrentProcessId());
            HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
            if (mapping) {
                auto cmd = static_cast<Command *>(
                    MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Command)));
                if (cmd && cmd->magic == 0x42564f4c && cmd->pid == GetCurrentProcessId() &&
                    InterlockedCompareExchange(&cmd->state, 1, 0) == 0) {
                    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                    const auto obj = static_cast<std::uintptr_t>(cmd->object);
                    const auto data = at<std::uintptr_t>(obj + 0x150);
                    const auto slider = at<std::uintptr_t>(obj + 0x148);
                    const auto listenerTable = at<std::uintptr_t>(obj + 0x108);
                    char label[128]{};
                    const size_t labelSize = strnlen(cmd->sourceName, sizeof(cmd->sourceName));
                    const unsigned char handlerSignature[] = {0xf3, 0x0f, 0x11, 0x4c, 0x24,
                                                              0x10, 0x55, 0x53, 0x56, 0x57};
                    unsigned char actual[sizeof(handlerSignature)]{};
                    read((void *)(base + BeacnLayout::FaderChanged), actual, sizeof(actual));
                    const bool labelRead = labelSize > 0 && labelSize < sizeof(label) &&
                                           read((void *)at<std::uintptr_t>(data), label, labelSize + 1);
                    const float personal = at<float>(data + 0x10), audience = at<float>(data + 0x14);
                    bool valid = at<std::uintptr_t>(obj) == base + BeacnLayout::FaderTable &&
                                 at<std::uintptr_t>(listenerTable) == base + BeacnLayout::FaderChanged && slider &&
                                 labelRead && !std::memcmp(label, cmd->sourceName, labelSize + 1) &&
                                 !std::memcmp(actual, handlerSignature, sizeof(actual)) &&
                                 std::isfinite(personal) && personal >= 0 && personal <= 1 &&
                                 std::isfinite(audience) && audience >= 0 && audience <= 1 &&
                                 std::isfinite(cmd->target) && cmd->target >= 0 && cmd->target <= 1 &&
                                 cmd->mode <= 6;
                    if (cmd->mode <= 1)
                        valid = valid && personal == cmd->expected && audience == personal &&
                                at<unsigned char>(data + 0x09) == 1;
                    else
                        valid = valid && (cmd->target == 0 || cmd->target == 1) &&
                                at<unsigned char>(data + 0x18) <= 1 && at<unsigned char>(data + 0x19) <= 1 &&
                                at<std::uintptr_t>(listenerTable + 8) == base + BeacnLayout::PersonalMuteChanged &&
                                at<std::uintptr_t>(listenerTable + 16) == base + BeacnLayout::AudienceMuteChanged;
                    if (cmd->mode >= 5) {
                        const auto controls = at<std::uintptr_t>(obj + 0x1e8);
                        valid = valid && at<std::uintptr_t>(controls + 8) == base + BeacnLayout::KnobButtonChanged &&
                                at<std::uintptr_t>(base + BeacnLayout::FaderTable + 0x1a0) == base + BeacnLayout::FaderMuteStateChanged &&
                                at<unsigned char>(data + 0x58) <= 1 && at<unsigned char>(data + 0x59) <= 1;
                    }
                    if (!valid) {
                        cmd->error = 1;
                        InterlockedExchange(&cmd->state, -1);
                    } else {
                        if (cmd->mode == 1) {
                            using SetDisplay = void (*)(void *, float);
                            using Change = void (*)(void *, float, void *);
                            reinterpret_cast<SetDisplay>(base + BeacnLayout::SetFaderDisplay)((void *)slider, cmd->target);
                            reinterpret_cast<Change>(base + BeacnLayout::FaderChanged)((void *)(obj + 0x108), cmd->target,
                                                                      (void *)slider);
                        }
                        cmd->observed = at<float>(data + 0x10);
                        if (cmd->mode >= 5) {
                            // Same idempotent listener used by the physical knob/button.
                            // BEACN chooses the affected mixes from its own mute configuration.
                            using SetControl = void (*)(void *, bool, int);
                            const int selector = cmd->mode == 6 ? 1 : 0;
                            reinterpret_cast<SetControl>(base + BeacnLayout::KnobButtonChanged)((void *)(obj + 0x1e8),
                                                                          cmd->target != 0, selector);
                            cmd->observed = at<unsigned char>(data + 0x58 + selector);
                            const bool matches = cmd->observed == cmd->target;
                            cmd->error = matches ? 0 : 2;
                            InterlockedExchange(&cmd->state, matches ? 2 : -2);
                        } else if (cmd->mode >= 2) {
                            using SetMute = void (*)(void *, bool);
                            const bool mute = cmd->target != 0;
                            if (cmd->mode == 2 || cmd->mode == 4)
                                reinterpret_cast<SetMute>(base + BeacnLayout::PersonalMuteChanged)((void *)(obj + 0x108), mute);
                            if (cmd->mode == 3 || cmd->mode == 4)
                                reinterpret_cast<SetMute>(base + BeacnLayout::AudienceMuteChanged)((void *)(obj + 0x108), mute);
                            const auto hp = at<unsigned char>(data + 0x18),
                                       aud = at<unsigned char>(data + 0x19);
                            const auto expected = static_cast<unsigned char>(mute);
                            const bool matches = cmd->mode == 2   ? hp == expected
                                                 : cmd->mode == 3 ? aud == expected
                                                                  : (hp == expected && aud == expected);
                            cmd->observed = cmd->mode == 3 ? aud : hp;
                            cmd->error = matches ? 0 : 2;
                            InterlockedExchange(&cmd->state, matches ? 2 : -2);
                        } else {
                            const bool matches = std::abs(cmd->observed - cmd->target) <= 0.0001f;
                            cmd->error = matches ? 0 : 2;
                            InterlockedExchange(&cmd->state, matches ? 2 : -2);
                        }
                    }
                }
                if (cmd)
                    UnmapViewOfFile(cmd);
                CloseHandle(mapping);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

#include "GlobalHook.inl"
