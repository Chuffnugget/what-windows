# Corsair iCUE

The optional Corsair iCUE module adds RGB lighting, individual LEDs and supported
headset properties. Release scripts prepare the pinned Corsair SDK automatically.
Profiles & device controls uses a separate, experimental Automation API.

Under **Modules > Corsair iCUE**, independently enable **Direct RGB control** and/or
**Profiles & device controls**. Each has a separate Home Assistant control checkbox.
The Automation submodule and its Home Assistant permission default to off.
Enable iCUE Settings > SDK and approve **SDK: New Client
Access Confirmation** for WHAT. Reading profiles does not establish write access.

Automation controls appear in the sensor browser and as Home Assistant
dropdowns: the complete iCUE profile, and existing cooling, EQ
and DPI presets for supported devices. Profile selection clears WHAT's colour
overrides and verifies the active profile. Cooling/EQ/DPI selectors say **Apply**:
iCUE does not expose confirmed current-preset feedback through these queries.
Presets are not edited; iCUE renders all effects itself. No firmware/device-memory
writes or custom RGB effect engine are implemented. The module does not expose
arbitrary internal methods, library macros, Mural commands or DPI numeric values.

Disabling a submodule removes its browser rows and makes corresponding HA entities
unavailable while preserving their identities. Inventories refresh every 15 seconds;
the active profile is checked on each iCUE poll (normally two seconds), and options
are revalidated before commands. This undocumented interface was tested with iCUE
5.49.34 and may change in future versions. API errors appear in Diagnostics.

An optional read-only C++ probe can be built with CMake target
`icue_automation_probe`; it prints the actual Automation metadata without switching
profiles or changing presets.


### Lighting Link

Under Modules > Corsair iCUE, enable Profiles & device controls. The sensor browser
includes an effect selector, parameter selectors, colour swatches/pickers and Stop
override. Changes apply automatically. iCUE renders the animations; WHAT does
not generate effects or edit saved profiles. Starting/changing or stopping Lighting
Link releases WHAT's direct RGB overrides first.

Enable the submodule's Home Assistant control permission for remote controls.
The Home Assistant device exposes an effect light, a second-colour light,
parameter selectors and a stop button. Both lights represent the same global effect:
turning either off stops the entire Lighting Link override and returns control to
profile lighting. This does not mean all LEDs become black. The second colour and
parameter entities are unavailable when the chosen effect does not support them.
Brightness adjusts a colour's RGB intensity; there is no invented global brightness
control for rainbow effects. Choosing a colour on the main light without explicitly
choosing an effect switches a colourless effect to Static colour.

Speed 0 to 2, direction 0 to 3 and rotation 0 to 1 are tested numeric API codes. Their human
labels have not been verified, so the UI keeps the codes. Colours use #RRGGBB in
HA/QML and are converted to iCUE's signed packed ARGB values internally. Current
state comes from nested get-active.effect replies. Missing readings are unavailable;
ResourceNotFound means no active Automation Lighting Link effect, not that all
profile lighting is off. Read-only sensors do not require HA control permission.

Parameter edits include the effect identity and are rejected if the effect changed
before execution. The iCUE worker reads fresh state before writing, sends a complete
validated payload and verifies readback. Writes are never automatically retried.
Stop is explicit: closing WHAT does not automatically deactivate an iCUE effect.

DPI current stage/sensitivity remain unsupported: the verified interface only lists
available stages. No additional DPI writes were added in this release.

