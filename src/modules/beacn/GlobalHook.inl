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
    const auto callback = at<Ptr>(combo + BeacnLayout::ComboChoices),
               method = at<Ptr>(at<Ptr>(callback) + 0x10);
    if (at<Ptr>(combo) != base + BeacnLayout::ComboTable || method < base + 0x1000 ||
        method >= base + 0x1300000)
        return false;
    BeacnLayout::Choices vector;
    reinterpret_cast<void *(*)(void *, void *)>(method)((void *)callback, &vector);
    // A malformed return must not reach native iteration/destruction.
    if (!vector.valid())
        return false;
    bool ok = true;
    cmd.counts[index] = 0;
    for (Ptr p = vector.begin; p < vector.end; p += 8) {
        char label[512]{};
        if (!copyString(at<Ptr>(p), label)) {
            ok = false;
            break;
        }
        if (label[0])
            std::memcpy(cmd.choices[index][cmd.counts[index]++], label, sizeof(label));
    }
    fn<void (*)(void *)>(base, BeacnLayout::DestroyChoices)(&vector);
    return ok;
}

bool globalLists(Ptr base, Ptr mixer, GlobalCommand &cmd) {
    auto personal = at<Ptr>(mixer + BeacnLayout::MixerListening),
         audience = at<Ptr>(mixer + BeacnLayout::MixerAudience);
    if (at<Ptr>(personal) != base + BeacnLayout::ListeningTable ||
        at<Ptr>(audience) != base + BeacnLayout::AudienceTable)
        return false;
    Ptr combos[] = {at<Ptr>(personal + BeacnLayout::ListeningComboA),
                    at<Ptr>(personal + BeacnLayout::ListeningComboB),
                    at<Ptr>(audience + BeacnLayout::AudienceCombo)};
    for (int i = 0; i < 3; ++i)
        if (!choiceList(base, combos[i], cmd, i))
            return false;
    if (at<Ptr>(cmd.assignments) == base + BeacnLayout::AssignmentsTable &&
        at<Ptr>(cmd.assignments + BeacnLayout::AssignmentsOwner) ==
            mixer + BeacnLayout::MixerAssignmentListener) {
        const auto &offsets = BeacnLayout::AssignmentCombos;
        for (int i = 0; i < 4; ++i)
            if (!choiceList(base, at<Ptr>(cmd.assignments + offsets[i]), cmd, i + 3))
                return false;
    }
    return true;
}

bool settingsApply(Ptr base, GlobalCommand &c) {
    if (c.operation >= 10 && c.operation <= 13) {
        auto obj = static_cast<Ptr>(c.assignments);
        if (at<Ptr>(obj) != base + BeacnLayout::AssignmentsTable ||
            at<Ptr>(obj + BeacnLayout::AssignmentsOwner) != c.mixer + BeacnLayout::MixerAssignmentListener ||
            at<Ptr>(at<Ptr>(obj + BeacnLayout::Listener) + 8) != base + BeacnLayout::DefaultDeviceChanged)
            return false;
        const int index = c.operation - 10;
        const auto &offsets = BeacnLayout::AssignmentCombos;
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
                (void *)(combo + BeacnLayout::ComboMenu), i + 1, &item, true, false);
            if (!std::strcmp(c.target, c.choices[index + 3][i]))
                selected = i + 1;
        }
        fn<void (*)(void *, int, int)>(base, BeacnLayout::SelectComboItem)((void *)combo, selected, 2);
        return true;
    }
    if (c.operation == 14) {
        auto obj = static_cast<Ptr>(c.settings);
        if (c.value < 0 || c.value > 1 || at<Ptr>(obj) != base + BeacnLayout::SystemSettingsTable ||
            at<Ptr>(at<Ptr>(obj + BeacnLayout::Listener)) != base + BeacnLayout::SystemSettingChanged)
            return false;
        auto group = at<Ptr>(obj + BeacnLayout::SettingsDefaultsGroup);
        if (at<Ptr>(group) != base + BeacnLayout::SettingsGroupTable)
            return false;
        auto begin = at<Ptr>(group + BeacnLayout::SettingsGroupBegin),
             end = at<Ptr>(group + BeacnLayout::SettingsGroupEnd);
        if (end < begin || (end - begin) % 8 || end - begin > 256)
            return false;
        Ptr slider = 0;
        int matches = 0;
        for (auto it = begin; it < end; it += 8) {
            auto row = at<Ptr>(it);
            if (at<Ptr>(row) == base + BeacnLayout::SettingsToggleTable &&
                at<int>(row + BeacnLayout::SettingId) == 5) {
                slider = at<Ptr>(row + BeacnLayout::SettingSlider);
                ++matches;
            }
        }
        if (matches != 1 || at<Ptr>(slider) != base + BeacnLayout::SliderTable)
            return false;
        fn<void (*)(void *, double, int)>(base, BeacnLayout::SetSliderValue)((void *)slider, double(c.value),
                                                                             0);
        fn<void (*)(void *, bool, int, void *)>(base, BeacnLayout::SystemSettingChanged)(
            (void *)(obj + BeacnLayout::Listener), c.value != 0, 5, (void *)slider);
        return true;
    }
    auto osd = static_cast<Ptr>(c.osd);
    if (at<Ptr>(osd) != base + BeacnLayout::OsdTable)
        return false;
    auto picker = at<Ptr>(osd + BeacnLayout::OsdTheme), lighting = at<Ptr>(osd + BeacnLayout::OsdLighting);
    if (c.operation == 20) {
        if (at<Ptr>(picker) != base + BeacnLayout::ThemeTable ||
            at<Ptr>(picker + BeacnLayout::ThemeOwner) != osd + BeacnLayout::Listener ||
            at<Ptr>(at<Ptr>(picker + BeacnLayout::Listener)) != base + BeacnLayout::SelectTheme ||
            c.value < 1 || c.value > 5)
            return false;
        fn<void (*)(void *, const int *)>(base, BeacnLayout::SelectTheme)(
            (void *)(picker + BeacnLayout::Listener), &c.value);
        return at<int>(picker + BeacnLayout::ThemeId) == c.value;
    }
    if (at<Ptr>(lighting) != base + BeacnLayout::LightingTable)
        return false;
    if (c.operation == 30 || c.operation == 31) {
        bool screen = c.operation == 30;
        auto obj = at<Ptr>(lighting + (screen ? BeacnLayout::LightingScreen : BeacnLayout::LightingButtons));
        auto table = screen ? BeacnLayout::ScreenBrightnessTable : BeacnLayout::ButtonBrightnessTable,
             handler = screen ? BeacnLayout::ScreenBrightnessChanged : BeacnLayout::ButtonBrightnessChanged;
        if (at<Ptr>(obj) != base + table ||
            at<Ptr>(at<Ptr>(obj + BeacnLayout::Listener) + 8) != base + handler ||
            c.value < (screen ? 2 : 4) || c.value > (screen ? 100 : 40))
            return false;
        fn<void (*)(void *, double, int)>(base, BeacnLayout::SetSliderValue)(
            (void *)(obj + BeacnLayout::BrightnessSlider), double(c.value), 0);
        fn<void (*)(void *, void *)>(base, handler)((void *)(obj + BeacnLayout::Listener),
                                                    (void *)(obj + BeacnLayout::BrightnessSlider));
        return true;
    }
    if (c.operation == 32) {
        // The timeout is a host-side timer. Extend the native slider's range to include
        // the requested value, then use the app's normal persistence/timer handler.
        if (c.value < 1 || c.value > 86400 ||
            at<Ptr>(at<Ptr>(lighting + BeacnLayout::Listener)) != base + BeacnLayout::LightingSettingChanged)
            return false;
        auto group = at<Ptr>(lighting + BeacnLayout::LightingGroup),
             dimmer = at<Ptr>(lighting + BeacnLayout::LightingDimmer);
        if (at<Ptr>(group) != base + BeacnLayout::SettingsGroupTable ||
            at<Ptr>(dimmer) != base + BeacnLayout::DimmerTable)
            return false;
        auto begin = at<Ptr>(group + BeacnLayout::SettingsGroupBegin),
             end = at<Ptr>(group + BeacnLayout::SettingsGroupEnd);
        Ptr slider = 0;
        int matches = 0;
        if (end < begin || (end - begin) % 8 || end - begin > 256)
            return false;
        for (auto it = begin; it < end; it += 8) {
            auto row = at<Ptr>(it);
            if (at<Ptr>(row) == base + BeacnLayout::SettingsSliderTable &&
                at<int>(row + BeacnLayout::SettingId) == 8) {
                slider = at<Ptr>(row + BeacnLayout::SettingSlider);
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
            (void *)(lighting + BeacnLayout::Listener), false, 8, (void *)slider);
        return at<double>(dimmer + BeacnLayout::DimmerSeconds) == c.value;
    }
    return false;
}

bool globalApply(Ptr base, GlobalCommand &c) {
    const auto mixer = static_cast<Ptr>(c.mixer);
    if (at<Ptr>(mixer) != base + BeacnLayout::MixerTable)
        return false;
    const auto data = at<Ptr>(mixer + BeacnLayout::MixerData),
               personal = at<Ptr>(mixer + BeacnLayout::MixerListening),
               audience = at<Ptr>(mixer + BeacnLayout::MixerAudience);
    const auto listenData = at<Ptr>(personal + BeacnLayout::ListeningData);
    const int mix = at<int>(data + BeacnLayout::MixerSubmix), page = at<int>(mixer + BeacnLayout::MixerPage),
              pages = at<int>(mixer + BeacnLayout::MixerPages);
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
        return at<int>(data + BeacnLayout::MixerSubmix) == c.value;
    }
    if (c.operation == 2 && (c.value == -1 || c.value == 1)) {
        const int expected = page + c.value;
        if (expected < 1 || expected > pages)
            return true;
        fn<void (*)(void *)>(base,
                             c.value < 0 ? BeacnLayout::PreviousPage : BeacnLayout::NextPage)((void *)mixer);
        return at<int>(mixer + BeacnLayout::MixerPage) == expected;
    }
    if (c.operation == 3 && (c.value == 0 || c.value == 1)) {
        if (at<Ptr>(at<Ptr>(personal + BeacnLayout::ListeningSwitchListener) + 0x48) !=
            base + BeacnLayout::SelectListeningDevice)
            return false;
        fn<void (*)(void *, bool)>(base, BeacnLayout::SelectListeningDevice)(
            (void *)(personal + BeacnLayout::ListeningSwitchListener), c.value != 0);
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
        auto combo = index == 2 ? at<Ptr>(audience + BeacnLayout::AudienceCombo)
                                : at<Ptr>(personal + BeacnLayout::ListeningComboA + index * 8);
        Ptr text = 0;
        fn<void *(*)(void *, void *)>(base, BeacnLayout::ComboText)((void *)combo, &text);
        char current[512]{};
        bool same = copyString(text, current) && !std::strcmp(current, c.target);
        fn<void (*)(void *)>(base, BeacnLayout::DestroyString)(&text);
        if (same)
            return true;
        Ptr listener =
            index == 2 ? audience + BeacnLayout::AudienceListener : personal + BeacnLayout::Listener;
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
        const Ptr begin = at<Ptr>(owner + BeacnLayout::ProfilesBegin),
                  end = at<Ptr>(owner + BeacnLayout::ProfilesEnd);
        if (end < begin || (end - begin) % 16 || (end - begin) / 16 > 128)
            return false;
        Ptr chosen = 0;
        int matches = 0;
        for (Ptr it = begin; it < end; it += 16) {
            const Ptr row = at<Ptr>(it);
            char name[512]{};
            if (at<Ptr>(row) != base + BeacnLayout::ProfileRowTable ||
                at<Ptr>(row + BeacnLayout::ProfileOwner) != owner + BeacnLayout::ProfilesListener)
                return false;
            if (!copyString(at<Ptr>(row + BeacnLayout::ProfileName), name))
                return false;
            if (!std::strcmp(name, c.target)) {
                chosen = row;
                ++matches;
            }
        }
        if (matches != 1 || at<unsigned char>(chosen + BeacnLayout::ProfileSelected) > 1)
            return false;
        if (at<unsigned char>(chosen + BeacnLayout::ProfileSelected))
            return true;
        // Same selection path as clicking a profile. The app retains its own save/change handling.
        if (at<Ptr>(at<Ptr>(owner + BeacnLayout::ProfilesListener) + 0x20) !=
            base + BeacnLayout::ProfileSelectionChanged)
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
            swprintf(name, 100, L"Local\\BEACN.WHAT.Global14219.%lu", GetCurrentProcessId());
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
