import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ModuleSection {
    id: section
    required property var controller
    required property var definition
    objectName: "optionalWindows_" + definition.key
    searchContext: "windows optional sensors"
    title: definition.title
    detail: definition.detail
    keywords: definition.help
    active: controller.info.sources[definition.key] === true
    onToggled: enabled => {
        const update = {};
        update[definition.key] = enabled;
        controller.saveSources(update);
        if (enabled && definition.key === "location")
            controller.optionalWindows.requestLocationAccess();
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: section.definition.help + " Disabled by default. Enable here, then choose which readings to share in Sensors."
    }
    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.warning
        text: section.active ? (section.controller.optionalWindows.issues[section.definition.key] || "") : ""
        visible: text.length > 0
    }
    RowLayout {
        visible: section.definition.key === "audio_levels"
        Layout.fillWidth: true
        HintLabel {
            text: "Sensor Update Interval"
            Layout.fillWidth: true
        }
        NumberInput {
            objectName: "audioMeterInterval"
            from: 200
            to: 2000
            stepSize: 100
            unit: "ms"
            value: section.controller.info.audioMeterInterval
            onValueModified: section.controller.saveAudioMeterInterval(value)
            ToolTip.text: "Default 500 ms. Expanded meters refresh locally at up to 30 samples per second. Home Assistant receives changes subject to this interval and the normal/fast delivery settings."
        }
    }
    ActionButton {
        visible: section.definition.key === "location"
        enabled: section.active
        text: "Request Location Access"
        ToolTip.text: "Ask Windows for location access. If access was denied previously, enable it in Windows Location Settings."
        onClicked: section.controller.optionalWindows.requestLocationAccess()
    }
    ActionButton {
        visible: !!section.definition.settings
        text: "Open Windows " + (section.definition.settingsTitle || "Privacy") + " Settings"
        onClicked: Qt.openUrlExternally("ms-settings:" + section.definition.settings)
    }
}
