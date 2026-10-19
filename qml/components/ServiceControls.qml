import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

RowLayout {
    id: service

    required property var controller
    required property bool transportConnected
    signal notification(string message)
    spacing: Theme.spaceS

    Rectangle {
        width: 8
        height: 8

        color: service.transportConnected ? Theme.accent : Theme.error
    }

    Label {
        text: controller.info.running ? "Service running" : "Service stopped"
        color: Theme.ink
        Layout.rightMargin: 10
    }

    ActionButton {
        ToolTip.text: "Start the Home Assistant API service on the saved port. MQTT is controlled separately."
        text: "Start"
        primary: !controller.info.running
        enabled: !controller.info.running

        onClicked: {
            controller.startService();
            service.notification(controller.info.server);
        }
    }

    ActionButton {
        ToolTip.text: "Stop Home Assistant API connections. Local collection and icons continue; MQTT is controlled separately."
        text: "Stop"
        enabled: controller.info.running
        onClicked: controller.stopService()
    }

    ActionButton {
        ToolTip.text: "Restart the Home Assistant API service using the saved settings. API clients will briefly disconnect."
        text: "Restart"
        enabled: controller.info.running

        onClicked: {
            controller.restartService();
            service.notification(controller.info.server);
        }
    }
}
