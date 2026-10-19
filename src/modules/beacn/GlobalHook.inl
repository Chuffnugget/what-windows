#include "GlobalBridge.h"

namespace {
using Ptr = std::uintptr_t;

template <class Fn> Fn fn(Ptr base, Ptr rva) {
    return reinterpret_cast<Fn>(base + rva);
}

bool copyString(Ptr source, char (&out)[512]) {
    for (unsigned i = 0; i < 512; ++i) {
        if (!read((void *)(source + i), out + i, 1))
            return false;
        if (!out[i])
            return true;
    }
    out[0] = 0;
    return false;
}

bool choiceList(Ptr base, Ptr combo, GlobalCommand &cmd, int index) {
    const auto callback = at<Ptr>(combo + 0x278), method = at<Ptr>(at<Ptr>(callback) + 0x10);
    if (at<Ptr>(combo) != base + BeacnLayout::ComboTable || method < base + 0x1000 ||
        method >= base + 0x1300000)
        return false;
    Ptr vector[4]{};
    reinterpret_cast<void *(*)(void *, void *)>(method)((void *)callback, vector);
    bool ok =
        vector[2] >= vector[1] && (vector[2] - vector[1]) % 8 == 0 && (vector[2] - vector[1]) / 8 <= 128;
    cmd.counts[index] = 0;
    if (ok)
        for (Ptr p = vector[1]; p < vector[2]; p += 8) {
            char label[512]{};
            if (!copyString(at<Ptr>(p), label)) {
                ok = false;
                break;
            }
            if (label[0])
                std::memcpy(cmd.choices[index][cmd.counts[index]++], label, sizeof(label));
        }
    fn<void (*)(void *)>(base, BeacnLayout::DestroyChoices)(vector);
    if (vector[0])
        fn<void (*)(void *, size_t)>(base, BeacnLayout::DeleteSized)((void *)vector[0], 16);
    return ok;
}

bool globalLists(Ptr base, Ptr mixer, GlobalCommand &cmd) {
    auto personal = at<Ptr>(mixer + 0x258), audience = at<Ptr>(mixer + 0x268);
    if (at<Ptr>(personal) != base + BeacnLayout::ListeningTable ||
        at<Ptr>(audience) != base + BeacnLayout::AudienceTable)
        return false;
    Ptr combos[] = {at<Ptr>(personal + 0x1e0), at<Ptr>(personal + 0x1e8), at<Ptr>(audience + 0x140)};
    for (int i = 0; i < 3; ++i)
        if (!choiceList(base, combos[i], cmd, i))
            return false;
    if (at<Ptr>(cmd.assignments) == base + BeacnLayout::AssignmentsTable &&
        at<Ptr>(cmd.assignments + 0x1c0) == mixer + 0x1d0) {
        const Ptr offsets[] = {0x158, 0x168, 0x180, 0x190};
        for (int i = 0; i < 4; ++i)
            if (!choiceList(base, at<Ptr>(cmd.assignments + offsets[i]), cmd, i + 3))
                return false;
    }
    return true;
}

bool settingsApply(Ptr base, GlobalCommand &c) {
    if (c.operation >= 10 && c.operation <= 13) {
        auto obj = static_cast<Ptr>(c.assignments);
        if (at<Ptr>(obj) != base + BeacnLayout::AssignmentsTable || at<Ptr>(obj + 0x1c0) != c.mixer + 0x1d0 ||
            at<Ptr>(at<Ptr>(obj + 0x108) + 8) != base + BeacnLayout::DefaultDeviceChanged)
            return false;
        const int index = c.operation - 10;
        const Ptr offsets[] = {0x158, 0x168, 0x180, 0x190};
        auto combo = at<Ptr>(obj + offsets[index]);
        if (!choiceList(base, combo, c, index + 3))
            return false;
        int matches = 0;
        for (unsigned i = 0; i < c.counts[index + 3]; ++i)
            if (!std::strcmp(c.target, c.choices[index + 3][i]))
                ++matches;
        if (matches != 1)
            return false;
        // Reapply even an unchanged label to keep native persistence in sync.
        // Populate the dynamic menu, then select an item through JUCE's native path.
        fn<void (*)(void *, int)>(base, BeacnLayout::ClearCombo)((void *)combo, 0);
        int selected = 0;
        for (unsigned i = 0; i < c.counts[index + 3]; ++i) {
            Ptr item = 0;
            fn<void *(*)(void *, const char *)>(base, BeacnLayout::CreateString)(&item,
                                                                                 c.choices[index + 3][i]);
            fn<void (*)(void *, int, void *, bool, bool)>(base, BeacnLayout::AddComboItem)(
                (void *)(combo + 0x178), i + 1, &item, true, false);
            if (!std::strcmp(c.target, c.choices[index + 3][i]))
                selected = i + 1;
        }
        fn<void (*)(void *, int, int)>(base, BeacnLayout::SelectComboItem)((void *)combo, selected, 2);
        return true;
    }
    if (c.operation == 14) {
        auto obj = static_cast<Ptr>(c.settings);
        if (c.value < 0 || c.value > 1 || at<Ptr>(obj) != base + BeacnLayout::SystemSettingsTable ||
            at<Ptr>(at<Ptr>(obj + 0x108)) != base + BeacnLayout::SystemSettingChanged)
            return false;
        auto group = at<Ptr>(obj + 0x168);
        if (at<Ptr>(group) != base + BeacnLayout::SettingsGroupTable)
            return false;
        auto begin = at<Ptr>(group + 0x168), end = at<Ptr>(group + 0x170);
        if (end < begin || (end - begin) % 8 || end - begin > 256)
            return false;
        Ptr slider = 0;
        int matches = 0;
        for (auto it = begin; it < end; it += 8) {
            auto row = at<Ptr>(it);
            if (at<Ptr>(row) == base + BeacnLayout::SettingsToggleTable && at<int>(row + 0x170) == 5) {
                slider = at<Ptr>(row + 0x150);
                ++matches;
            }
        }
        if (matches != 1 || at<Ptr>(slider) != base + BeacnLayout::SliderTable)
            return false;
        fn<void (*)(void *, double, int)>(base, BeacnLayout::SetSliderValue)((void *)slider, double(c.value),
                                                                             0);
        fn<void (*)(void *, bool, int, void *)>(base, BeacnLayout::SystemSettingChanged)(
            (void *)(obj + 0x108), c.value != 0, 5, (void *)slider);
        return true;
    }
    auto osd = static_cast<Ptr>(c.osd);
    if (at<Ptr>(osd) != base + BeacnLayout::OsdTable)
        return false;
    auto picker = at<Ptr>(osd + 0x3b8), lighting = at<Ptr>(osd + 0x3c0);
    if (c.operation == 20) {
        if (at<Ptr>(picker) != base + BeacnLayout::ThemeTable || at<Ptr>(picker + 0x148) != osd + 0x108 ||
            at<Ptr>(at<Ptr>(picker + 0x108)) != base + BeacnLayout::SelectTheme || c.value < 1 || c.value > 5)
            return false;
        fn<void (*)(void *, const int *)>(base, BeacnLayout::SelectTheme)((void *)(picker + 0x108), &c.value);
        return at<int>(picker + 0x140) == c.value;
    }
    if (at<Ptr>(lighting) != base + BeacnLayout::LightingTable)
        return false;
    if (c.operation == 30 || c.operation == 31) {
        bool screen = c.operation == 30;
        auto obj = at<Ptr>(lighting + (screen ? 0x120 : 0x128));
        auto table = screen ? BeacnLayout::ScreenBrightnessTable : BeacnLayout::ButtonBrightnessTable,
             handler = screen ? BeacnLayout::ScreenBrightnessChanged : BeacnLayout::ButtonBrightnessChanged;
        if (at<Ptr>(obj) != base + table || at<Ptr>(at<Ptr>(obj + 0x108) + 8) != base + handler ||
            c.value < (screen ? 2 : 4) || c.value > (screen ? 100 : 40))
            return false;
        fn<void (*)(void *, double, int)>(base, BeacnLayout::SetSliderValue)((void *)(obj + 0x3c8),
                                                                             double(c.value), 0);
        fn<void (*)(void *, void *)>(base, handler)((void *)(obj + 0x108), (void *)(obj + 0x3c8));
        return true;
    }
    if (c.operation == 32) {
        // The timeout is a host-side timer. Extend the native slider's range to include
        // the requested value, then use the app's normal persistence/timer handler.
        if (c.value < 1 || c.value > 86400 ||
            at<Ptr>(at<Ptr>(lighting + 0x108)) != base + BeacnLayout::LightingSettingChanged)
            return false;
        auto group = at<Ptr>(lighting + 0x140), dimmer = at<Ptr>(lighting + 0x130);
        if (at<Ptr>(group) != base + BeacnLayout::SettingsGroupTable ||
            at<Ptr>(dimmer) != base + BeacnLayout::DimmerTable)
            return false;
        auto begin = at<Ptr>(group + 0x168), end = at<Ptr>(group + 0x170);
        Ptr slider = 0;
        int matches = 0;
        if (end < begin || (end - begin) % 8 || end - begin > 256)
            return false;
        for (auto it = begin; it < end; it += 8) {
            auto row = at<Ptr>(it);
            if (at<Ptr>(row) == base + BeacnLayout::SettingsSliderTable && at<int>(row + 0x170) == 8) {
                slider = at<Ptr>(row + 0x150);
                ++matches;
            }
        }
        if (matches != 1 || at<Ptr>(slider) != base + BeacnLayout::SliderTable)
            return false;
        fn<void (*)(void *, double, double, double)>(base, BeacnLayout::SetSliderRange)(
            (void *)slider, c.value < 30 ? c.value : 30, c.value > 300 ? c.value : 300, 1.0);
        fn<void (*)(void *, double, int)>(base, BeacnLayout::SetSliderValue)((void *)slider, double(c.value),
                                                                             0);
        fn<void (*)(void *, bool, int, void *)>(base, BeacnLayout::LightingSettingChanged)(
            (void *)(lighting + 0x108), false, 8, (void *)slider);
        return at<double>(dimmer + 0x48) == c.value;
    }
    return false;
}

bool globalApply(Ptr base, GlobalCommand &c) {
    const auto mixer = static_cast<Ptr>(c.mixer);
    if (at<Ptr>(mixer) != base + BeacnLayout::MixerTable)
        return false;
    const auto data = at<Ptr>(mixer + 0x2b0), personal = at<Ptr>(mixer + 0x258),
               audience = at<Ptr>(mixer + 0x268);
    const auto listenData = at<Ptr>(personal + 0x488);
    const int mix = at<int>(data + 0xd8), page = at<int>(mixer + 0x674), pages = at<int>(mixer + 0x678);
    if (!data || mix < 0 || mix > 1 || page < 1 || pages < page || pages > 64 ||
        at<Ptr>(personal) != base + BeacnLayout::ListeningTable ||
        at<Ptr>(audience) != base + BeacnLayout::AudienceTable || !listenData ||
        at<unsigned char>(listenData + 0x10) > 1)
        return false;
    if (c.operation >= 10)
        return settingsApply(base, c);
    if (c.operation == 0)
        return globalLists(base, mixer, c);
    if (c.operation == 1 && (c.value == 0 || c.value == 1)) {
        if (mix != c.value)
            fn<void (*)(void *)>(base, BeacnLayout::ToggleSubmix)((void *)mixer);
        return at<int>(data + 0xd8) == c.value;
    }
    if (c.operation == 2 && (c.value == -1 || c.value == 1)) {
        const int expected = page + c.value;
        if (expected < 1 || expected > pages)
            return true;
        fn<void (*)(void *)>(base,
                             c.value < 0 ? BeacnLayout::PreviousPage : BeacnLayout::NextPage)((void *)mixer);
        return at<int>(mixer + 0x674) == expected;
    }
    if (c.operation == 3 && (c.value == 0 || c.value == 1)) {
        if (at<Ptr>(at<Ptr>(personal + 0x128) + 0x48) != base + BeacnLayout::SelectListeningDevice)
            return false;
        fn<void (*)(void *, bool)>(base, BeacnLayout::SelectListeningDevice)((void *)(personal + 0x128),
                                                                             c.value != 0);
        return true; // Slider notifications are queued by BEACN; confirm on delayed model readback.
    }
    if (c.operation >= 4 && c.operation <= 6) {
        if (!globalLists(base, mixer, c))
            return false;
        const unsigned index = c.operation - 4;
        int matches = 0;
        for (unsigned i = 0; i < c.counts[index]; ++i)
            if (!std::strcmp(c.target, c.choices[index][i]))
                ++matches;
        if (matches != 1)
            return false;
        auto combo = index == 2 ? at<Ptr>(audience + 0x140) : at<Ptr>(personal + 0x1e0 + index * 8);
        Ptr text = 0;
        fn<void *(*)(void *, void *)>(base, BeacnLayout::ComboText)((void *)combo, &text);
        char current[512]{};
        bool same = copyString(text, current) && !std::strcmp(current, c.target);
        fn<void (*)(void *)>(base, BeacnLayout::DestroyString)(&text);
        if (same)
            return true;
        Ptr listener = index == 2 ? audience + 0x110 : personal + 0x108;
        Ptr handler = index == 2 ? BeacnLayout::AudienceDeviceChanged : BeacnLayout::ListeningDeviceChanged;
        if (at<Ptr>(at<Ptr>(listener) + 8) != base + handler)
            return false;
        fn<void *(*)(void *, const char *)>(base, BeacnLayout::CreateString)(&text, c.target);
        fn<void (*)(void *, void *, int)>(base, BeacnLayout::SetComboText)((void *)combo, &text, 0);
        fn<void (*)(void *)>(base, BeacnLayout::DestroyString)(&text);
        fn<void (*)(void *, void *)>(base, handler)((void *)listener, (void *)combo);
        return true; // Model readback is exported separately; aliases can differ from labels.
    }
    if (c.operation == 7) {
        const Ptr owner = static_cast<Ptr>(c.profiles);
        if (at<Ptr>(owner) != base + BeacnLayout::ProfilesTable)
            return false;
        const Ptr begin = at<Ptr>(owner + 0x190), end = at<Ptr>(owner + 0x198);
        if (end < begin || (end - begin) % 16 || (end - begin) / 16 > 128)
            return false;
        Ptr chosen = 0;
        int matches = 0;
        for (Ptr it = begin; it < end; it += 16) {
            const Ptr row = at<Ptr>(it);
            char name[512]{};
            if (at<Ptr>(row) != base + BeacnLayout::ProfileRowTable || at<Ptr>(row + 0x250) != owner + 0x140)
                return false;
            if (!copyString(at<Ptr>(row + 0x178), name))
                return false;
            if (!std::strcmp(name, c.target)) {
                chosen = row;
                ++matches;
            }
        }
        if (matches != 1 || at<unsigned char>(chosen + 0x171) > 1)
            return false;
        if (at<unsigned char>(chosen + 0x171))
            return true;
        // Same selection path as clicking a profile. The app retains its own save/change handling.
        if (at<Ptr>(at<Ptr>(owner + 0x140) + 0x20) != base + BeacnLayout::ProfileSelectionChanged)
            return false;
        fn<void (*)(void *, bool, bool)>(base, BeacnLayout::SelectProfile)((void *)chosen, true, true);
        return true;
    }
    return false;
}
} // namespace

extern "C" __declspec(dllexport) LRESULT CALLBACK GlobalHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) {
        auto msg = reinterpret_cast<CWPSTRUCT *>(lp);
        if (msg && msg->message == RegisterWindowMessageW(GlobalMessage)) {
            wchar_t name[100];
            swprintf(name, 100, L"Local\\BEACN.WHAT.Global142.%lu", GetCurrentProcessId());
            HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
            if (mapping) {
                auto cmd = static_cast<GlobalCommand *>(
                    MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GlobalCommand)));
                if (cmd && cmd->magic == 0x42474c42 && cmd->pid == GetCurrentProcessId() &&
                    std::memchr(cmd->target, 0, sizeof(cmd->target)) &&
                    InterlockedCompareExchange(&cmd->state, 1, 0) == 0) {
                    const bool ok = globalApply(reinterpret_cast<Ptr>(GetModuleHandleW(nullptr)), *cmd);
                    cmd->error = ok ? 0 : 1;
                    InterlockedExchange(&cmd->state, ok ? 2 : -1);
                }
                if (cmd)
                    UnmapViewOfFile(cmd);
                CloseHandle(mapping);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
