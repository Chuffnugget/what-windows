import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ModuleCard {
    id: coretempSettings

    moduleKey: "coretemp"
    heading: "Core Temp"
    summary: "CPU temperatures, load, clocks and power from shared memory"
    keywords: "coretemp core temp cpu temperature load clocks power shared memory polling"

    HintLabel {
        text: coretempSettings.controller.info.coretempStatus
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    RowLayout {
        HintLabel {
            text: "Read shared memory every"
            Layout.fillWidth: true
        }

        PollingInput {
            controller: coretempSettings.controller
            moduleKey: "coretemp"
            from: 500
            to: 60000
            stepSize: 500
        }
    }

    HintLabel {
        text: "Install and run Core Temp, then enable shared memory in its settings. WHAT only reads the shared data; it does not install a driver or change Core Temp's hardware polling rate."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    HintLabel {
        text: "Recommended: run either Core Temp or HWiNFO for hardware monitoring. Running both duplicates hardware polling and may cause compatibility problems on some systems. Disabling a module here does not close the other application."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: coretempSettings.controller.info.hwinfoEnabled
            && coretempSettings.controller.info.coretempEnabled
            ? Theme.warning
            : Theme.muted
    }

    ActionButton {
        ToolTip.text: "Open the official Core Temp website for downloads and setup information."
        text: "Core Temp Website"
        onClicked: Qt.openUrlExternally("https://www.alcpu.com/CoreTemp/")
    }
}
