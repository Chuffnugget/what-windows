import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../custom"

ModuleCard {
    id: card

    moduleKey: "custom"
    heading: "Custom Sensors and Commands"
    summary: "Local definitions, reading access and saved-command permissions"
    keywords: "custom sensors commands scripts powershell cmd wsl process service file counter wmi permissions cloud sharing"
    signal sensorsRequested
    signal commandsRequested

    Label {
        text: "Share Readings with Home Assistant"
        color: Theme.ink
    }
    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: "Select Local or Cloud beside each reading in Sensors. Home Assistant receives only those results. This does not allow it to edit definitions or run commands. Locally configured sensor scripts still execute with your Windows permissions."
    }

    CommandPermissions {
        controller: card.controller
        showShell: false
    }

    PermissionOption {
        text: "Allow Custom Readings and Commands Over Cloud Connections"
        checked: card.controller.customSources.cloudEnabled
        access: PermissionBadge.Readings
        badgeText: "Cloud Route"
        help: "The cloud route is enabled for eligible readings and commands. Individual sharing and execution permissions still apply."
        onToggled: allowed => card.controller.customSources.cloudEnabled = allowed
        description:
            "This enables the cloud route only. Each reading still needs Cloud selected in Sensors, and each saved command needs its own Home Assistant and cloud permissions."
    }

    RowLayout {
        ActionButton {
            text: "Manage Custom Sensors"
            ToolTip.text: "Open the sensor browser's custom sensor editor and saved definitions."
            onClicked: card.sensorsRequested()
        }
        ActionButton {
            text: "Open Commands"
            ToolTip.text: "Create, test and run saved commands or use the one-off command console."
            onClicked: card.commandsRequested()
        }
    }
}
