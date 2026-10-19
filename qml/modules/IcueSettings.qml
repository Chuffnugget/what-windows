import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ModuleCard {
    id: icueModule

    moduleKey: "icue"
    heading: "Corsair iCUE"
    summary: "Direct RGB, profiles and device presets"
    keywords: "icue corsair rgb led lighting keyboard mouse headset fan cooling memory battery equalizer profile dpi sdk automation link effect colour speed direction rotation random"

    HintLabel {
        text: "In iCUE, enable Settings → SDK. When 'SDK: New Client Access Confirmation' appears, approve WHAT. This is separate from each submodule's Home Assistant control permission."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    ModuleSection {
        searchContext: "icue corsair"
        objectName: "icueDirectSubmodule"
        title: "Direct RGB Control"
        detail: "Public SDK · colours, brightness and device properties"
        query: icueModule.query
        active: icueModule.controller.info.icueDirectEnabled === true
        onToggled: enabled => controller.setIcueOption("direct", enabled)

        HintLabel {
            text: icueModule.controller.info.icueStatus || "Disabled"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        PermissionOption {
            help: "Allow paired Home Assistant clients to change RGB colours and supported headset properties through the iCUE SDK."
            text: "Allow Home Assistant to Control RGB and Headset Settings"
            checked: icueModule.controller.info.icueControl === true
            onToggled: allowed => controller.setIcueControl(allowed)
        }

        HintLabel {
            text: "One Home Assistant light per device, with optional individual LED entities. Disabling direct RGB releases WHAT's colour overrides."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "icue corsair"
        objectName: "icueAutomationSubmodule"
        keywords: "lighting link effect colour color speed direction rotation random cooling eq dpi profiles presets"
        title: "Profiles & Device Controls"
        detail: "Experimental Automation API · profiles, presets and Lighting Link"
        query: icueModule.query
        active: icueModule.controller.info.icueAutomationEnabled === true
        onToggled: enabled => controller.setIcueOption("automation", enabled)

        HintLabel {
            text: icueModule.controller.info.icueAutomationStatus || "Disabled"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        HintLabel {
            text: icueModule.controller.info.icueAutomationTelemetryStatus || ""
            visible: icueModule.controller.info.icueAutomationEnabled && text.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        PermissionOption {
            help: "Allow paired Home Assistant clients to change iCUE profiles, device presets and Lighting Link settings."
            text: "Allow Home Assistant to Control Profiles, Presets and Lighting Link"
            checked: icueModule.controller.info.icueAutomationControl === true
            onToggled: allowed => controller.setIcueOption("automationControl", allowed)
        }

        HintLabel {
            text: "Controls appear in the sensor browser and as Home Assistant entities. Lighting Link is one global override; Stop override returns control to profile lighting. Selecting a complete profile clears WHAT's RGB overrides first and can also change cooling and other profile settings. iCUE runs all effects itself."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        HintLabel {
            text: "Only presets reported by each device are offered. Cooling, EQ and DPI controls apply a preset; the API does not confirm their current preset. This undocumented API may change with iCUE updates."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }
}
