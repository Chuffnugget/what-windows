#pragma once
#include <windows.h>

// Load the executable's native multi-size icon directly. This preserves the
// alpha channel AND the fallback transparency mask used by Windows Shell.
inline bool setNativeTaskbarIcon(HWND window) {
    const auto dpi = GetDpiForWindow(window);
    auto load = [&](int metric) {
        const int size = GetSystemMetricsForDpi(metric, dpi ? dpi : 96);
        return static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_SHARED));
    };
    const auto large = load(SM_CXICON), small = load(SM_CXSMICON);
    if (large)
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large));
    if (small)
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    return large && small; // LR_SHARED handles belong to Windows.
}
