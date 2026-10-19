#pragma once
#include <windows.h>
#include <cstdint>

// Fixed-size wire format: never share C++/Qt objects across processes or CRTs.
struct GlobalCommand {
    DWORD magic, pid;
    volatile LONG state;
    DWORD operation; // 0 lists, 1 submix, 2 page step, 3 active A/B, 4/5/6 device A/B/Audience, 7 profile;
                     // 10-13 Windows defaults; 14 automatic reset; 20 theme; 30/31 brightness; 32 timeout
                     // seconds
    std::uint64_t mixer, profiles, assignments, osd, settings;
    int value;
    DWORD error;
    char target[512];
    unsigned counts[7];
    char choices[7][128][512];
};

inline constexpr wchar_t GlobalMessage[] = L"BEACN.WHAT.Global.142";
