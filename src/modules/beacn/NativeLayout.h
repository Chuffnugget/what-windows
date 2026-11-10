#pragma once
#include <cstdint>

// BEACN 1.4.2.19 x64. Image-relative addresses shared by the reader and UI-thread
// bridge. This build uses release STL/JUCE layouts; 1.4.2.12 offsets are incompatible.
// InstalledFiles checks the executable before use; live objects and callback
// ownership are checked again immediately before each native command.
namespace BeacnLayout {
inline constexpr std::uintptr_t DestroyChoices = 0x1d3e70;
inline constexpr std::uintptr_t AudienceDeviceChanged = 0x2dd650;
inline constexpr std::uintptr_t ListeningDeviceChanged = 0x2e1dc0;
inline constexpr std::uintptr_t SelectListeningDevice = 0x2e30e0;
inline constexpr std::uintptr_t LightingSettingChanged = 0x2e9880;
inline constexpr std::uintptr_t SelectTheme = 0x2ee490;
inline constexpr std::uintptr_t SetFaderDisplay = 0x31fc30;
inline constexpr std::uintptr_t ToggleSubmix = 0x325ad0;
inline constexpr std::uintptr_t PreviousPage = 0x32a0c0;
inline constexpr std::uintptr_t NextPage = 0x32a140;
inline constexpr std::uintptr_t DefaultDeviceChanged = 0x34e930;
inline constexpr std::uintptr_t FaderChanged = 0x366080;
inline constexpr std::uintptr_t PersonalMuteChanged = 0x3667e0;
inline constexpr std::uintptr_t AudienceMuteChanged = 0x366a10;
inline constexpr std::uintptr_t FaderMuteStateChanged = 0x367e10;
inline constexpr std::uintptr_t KnobButtonChanged = 0x36a950;
inline constexpr std::uintptr_t SystemSettingChanged = 0x3706a0;
inline constexpr std::uintptr_t ButtonBrightnessChanged = 0x376a10;
inline constexpr std::uintptr_t ScreenBrightnessChanged = 0x376b90;
inline constexpr std::uintptr_t SelectProfile = 0x3a0aa0;
inline constexpr std::uintptr_t ProfileSelectionChanged = 0x3a5bc0;
inline constexpr std::uintptr_t CreateString = 0x552de0;
inline constexpr std::uintptr_t DestroyString = 0x5536b0;
inline constexpr std::uintptr_t AddComboItem = 0x7b9f50;
inline constexpr std::uintptr_t ClearCombo = 0x7d3a00;
inline constexpr std::uintptr_t SelectComboItem = 0x7d3d40;
inline constexpr std::uintptr_t ComboText = 0x7d40d0;
inline constexpr std::uintptr_t SetComboText = 0x7d4120;
inline constexpr std::uintptr_t SetSliderValue = 0x7d9d70;
inline constexpr std::uintptr_t SetSliderRange = 0x7d9e20;
inline constexpr std::uintptr_t DimmerTable = 0x15f7020;
inline constexpr std::uintptr_t DefaultResetManagerTable = 0x15f71c0;
inline constexpr std::uintptr_t ComboTable = 0x15f76f8;
inline constexpr std::uintptr_t AudienceTable = 0x15f8060;
inline constexpr std::uintptr_t ListeningTable = 0x15f9210;
inline constexpr std::uintptr_t LightingTable = 0x1688f38;
inline constexpr std::uintptr_t OsdTable = 0x16922f0;
inline constexpr std::uintptr_t ThemeTable = 0x1692d98;
inline constexpr std::uintptr_t MixerTable = 0x2598460;
inline constexpr std::uintptr_t AssignmentsTable = 0x25a1320;
inline constexpr std::uintptr_t FaderTable = 0x25a6670;
inline constexpr std::uintptr_t SystemSettingsTable = 0x25a95c0;
inline constexpr std::uintptr_t SettingsSliderTable = 0x25aaf58;
inline constexpr std::uintptr_t ButtonBrightnessTable = 0x25ab5b0;
inline constexpr std::uintptr_t ScreenBrightnessTable = 0x25ab8b8;
inline constexpr std::uintptr_t SettingsToggleTable = 0x25adcc8;
inline constexpr std::uintptr_t SettingsGroupTable = 0x25adff8;
inline constexpr std::uintptr_t ProfilesTable = 0x25b2c98;
inline constexpr std::uintptr_t ProfileRowTable = 0x25b4e20;
inline constexpr std::uintptr_t DoubleValueType = 0x27757e0;
inline constexpr std::uintptr_t StringValueType = 0x2775960;
inline constexpr std::uintptr_t ValueSourceTable = 0x27813f0;
inline constexpr std::uintptr_t SliderTable = 0x27beac0;

// Object fields are shared so discovery, readback and command validation agree.
inline constexpr std::uintptr_t Listener = 0xf0;
inline constexpr std::uintptr_t FaderSlider = 0x130;
inline constexpr std::uintptr_t FaderData = 0x138;
inline constexpr std::uintptr_t FaderKnobListener = 0x1c0;
inline constexpr std::uintptr_t MixerData = 0x268;
inline constexpr std::uintptr_t MixerListening = 0x218;
inline constexpr std::uintptr_t MixerAudience = 0x228;
inline constexpr std::uintptr_t MixerPage = 0x5dc;
inline constexpr std::uintptr_t MixerPages = 0x5e0;
inline constexpr std::uintptr_t MixerSubmix = 0xc8;
inline constexpr std::uintptr_t MixerFadersBegin = 0x1e0;
inline constexpr std::uintptr_t MixerFadersEnd = 0x1e8;
inline constexpr std::uintptr_t MixerAssignmentListener = 0x1a0;
inline constexpr std::uintptr_t MixerDevice = 0x2f8;
inline constexpr std::uintptr_t ListeningComboA = 0x1c8;
inline constexpr std::uintptr_t ListeningComboB = 0x1d0;
inline constexpr std::uintptr_t ListeningData = 0x490;
inline constexpr std::uintptr_t ListeningSwitchListener = 0x110;
inline constexpr std::uintptr_t AudienceCombo = 0x138;
inline constexpr std::uintptr_t AudienceData = 0x178;
inline constexpr std::uintptr_t AudienceListener = 0xf8;
inline constexpr std::uintptr_t ProfilesBegin = 0x160;
inline constexpr std::uintptr_t ProfilesEnd = 0x168;
inline constexpr std::uintptr_t ProfileSelected = 0x141;
inline constexpr std::uintptr_t ProfileName = 0x148;
inline constexpr std::uintptr_t ProfileOwner = 0x210;
inline constexpr std::uintptr_t ProfilesListener = 0x118;
inline constexpr std::uintptr_t ComboChoices = 0x230;
inline constexpr std::uintptr_t ComboMenu = 0x158;
inline constexpr std::uintptr_t ComboLabel = 0x1d8;
inline constexpr std::uintptr_t LabelValueSource = 0x1d8;
inline constexpr std::uintptr_t SliderImpl = 0x240;
inline constexpr std::uintptr_t SliderValueSource = 0x50;
inline constexpr std::uintptr_t ValueType = 0x30;
inline constexpr std::uintptr_t ValuePayload = 0x38;
inline constexpr std::uintptr_t AssignmentsOwner = 0x198;
inline constexpr std::uintptr_t SettingsResetManager = 0x1a0;
inline constexpr std::uintptr_t SettingsDefaultsGroup = 0x158;
inline constexpr std::uintptr_t SettingsGroupBegin = 0x138;
inline constexpr std::uintptr_t SettingsGroupEnd = 0x140;
inline constexpr std::uintptr_t SettingId = 0x148;
inline constexpr std::uintptr_t SettingSlider = 0x128;
inline constexpr std::uintptr_t OsdTheme = 0x368;
inline constexpr std::uintptr_t OsdLighting = 0x370;
inline constexpr std::uintptr_t OsdDevice = 0x380;
inline constexpr std::uintptr_t ThemeId = 0x118;
inline constexpr std::uintptr_t ThemeOwner = 0x120;
inline constexpr std::uintptr_t LightingScreen = 0x108;
inline constexpr std::uintptr_t LightingButtons = 0x110;
inline constexpr std::uintptr_t LightingDimmer = 0x118;
inline constexpr std::uintptr_t LightingGroup = 0x128;
inline constexpr std::uintptr_t DimmerSeconds = 0x38;
inline constexpr std::uintptr_t DimmerActive = 0x40;
inline constexpr std::uintptr_t BrightnessSlider = 0x370;
inline constexpr std::uintptr_t AssignmentCombos[] = {0x130, 0x140, 0x158, 0x168};

// BEACN owns the returned strings and allocation. DestroyChoices releases both
// using BEACN's own allocator. There is no debug-iterator proxy in this build.
struct Choices {
    std::uintptr_t begin = 0, end = 0, capacity = 0;

    bool valid() const {
        if (!begin)
            return !end && !capacity;
        return begin % sizeof(std::uintptr_t) == 0 && end >= begin && capacity >= end &&
               (end - begin) % sizeof(std::uintptr_t) == 0 &&
               (capacity - begin) % sizeof(std::uintptr_t) == 0 &&
               (end - begin) / sizeof(std::uintptr_t) <= 128 &&
               (capacity - begin) / sizeof(std::uintptr_t) <= 4096;
    }
};

static_assert(sizeof(Choices) == 3 * sizeof(std::uintptr_t));
} // namespace BeacnLayout
