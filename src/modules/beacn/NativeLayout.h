#pragma once
#include <cstdint>

// BEACN 1.4.2.12. Image-relative addresses shared by the reader and UI-thread
// bridge. Do not reuse the 1.4.1 layout: settings groups and callbacks changed.
// InstalledFiles checks the executable before use; live objects and callback
// ownership are checked again immediately before each native command.
namespace BeacnLayout {
inline constexpr std::uintptr_t DestroyChoices = 0x297c20;
inline constexpr std::uintptr_t AudienceDeviceChanged = 0x408a50;
inline constexpr std::uintptr_t ListeningDeviceChanged = 0x40e610;
inline constexpr std::uintptr_t SelectListeningDevice = 0x40f9b0;
inline constexpr std::uintptr_t LightingSettingChanged = 0x417860;
inline constexpr std::uintptr_t SelectTheme = 0x41e7a0;
inline constexpr std::uintptr_t SetFaderDisplay = 0x4656c0;
inline constexpr std::uintptr_t ToggleSubmix = 0x46fcd0;
inline constexpr std::uintptr_t PreviousPage = 0x476320;
inline constexpr std::uintptr_t NextPage = 0x4763a0;
inline constexpr std::uintptr_t DefaultDeviceChanged = 0x4ae8a0;
inline constexpr std::uintptr_t FaderChanged = 0x4d0b60;
inline constexpr std::uintptr_t PersonalMuteChanged = 0x4d12c0;
inline constexpr std::uintptr_t AudienceMuteChanged = 0x4d14f0;
inline constexpr std::uintptr_t FaderMuteStateChanged = 0x4d2920;
inline constexpr std::uintptr_t KnobButtonChanged = 0x4d56d0;
inline constexpr std::uintptr_t SystemSettingChanged = 0x4db750;
inline constexpr std::uintptr_t ButtonBrightnessChanged = 0x4e2800;
inline constexpr std::uintptr_t ScreenBrightnessChanged = 0x4e2980;
inline constexpr std::uintptr_t SelectProfile = 0x51b310;
inline constexpr std::uintptr_t ProfileSelectionChanged = 0x520da0;
inline constexpr std::uintptr_t CreateString = 0x75d890;
inline constexpr std::uintptr_t DestroyString = 0x75e580;
inline constexpr std::uintptr_t AddComboItem = 0xac5880;
inline constexpr std::uintptr_t ClearCombo = 0xae0990;
inline constexpr std::uintptr_t SelectComboItem = 0xae0d30;
inline constexpr std::uintptr_t ComboText = 0xae1100;
inline constexpr std::uintptr_t SetComboText = 0xae1150;
inline constexpr std::uintptr_t SetSliderValue = 0xae7440;
inline constexpr std::uintptr_t SetSliderRange = 0xae74a0;
inline constexpr std::uintptr_t DeleteSized = 0x11a7960;
inline constexpr std::uintptr_t DimmerTable = 0x1feb380;
inline constexpr std::uintptr_t DefaultResetManagerTable = 0x1feb840;
inline constexpr std::uintptr_t ComboTable = 0x1fec6e0;
inline constexpr std::uintptr_t AudienceTable = 0x1fed990;
inline constexpr std::uintptr_t ListeningTable = 0x1fefb00;
inline constexpr std::uintptr_t LightingTable = 0x20807a0;
inline constexpr std::uintptr_t OsdTable = 0x208a4b0;
inline constexpr std::uintptr_t ThemeTable = 0x208b5c0;
inline constexpr std::uintptr_t MixerTable = 0x2f96ab0;
inline constexpr std::uintptr_t AssignmentsTable = 0x2fa4a90;
inline constexpr std::uintptr_t FaderTable = 0x2fad1a0;
inline constexpr std::uintptr_t SystemSettingsTable = 0x2fb14f8;
inline constexpr std::uintptr_t SettingsSliderTable = 0x2fb4140;
inline constexpr std::uintptr_t ButtonBrightnessTable = 0x2fb4dd0;
inline constexpr std::uintptr_t ScreenBrightnessTable = 0x2fb53e0;
inline constexpr std::uintptr_t SettingsToggleTable = 0x2fb8e90;
inline constexpr std::uintptr_t SettingsGroupTable = 0x2fb94f8;
inline constexpr std::uintptr_t ProfilesTable = 0x2fc3a28;
inline constexpr std::uintptr_t ProfileRowTable = 0x2fc7608;
inline constexpr std::uintptr_t DoubleValueType = 0x31ac6e0;
inline constexpr std::uintptr_t StringValueType = 0x31ac860;
inline constexpr std::uintptr_t ValueSourceTable = 0x31b9be0;
inline constexpr std::uintptr_t SliderTable = 0x32174a8;
} // namespace BeacnLayout
