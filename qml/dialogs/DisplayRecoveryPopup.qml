import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

Popup {
    id: displayRecovery

    required property var controller

    enter: null
    exit: null

    parent: Overlay.overlay
    x: Math.max(16, (parent.width - width) / 2)
    y: 12
    width: Math.min(720, parent.width - 32)
    visible: displayRecovery.controller.displays.pending === true
    closePolicy: Popup.NoAutoClose
    padding: 14

    background: Rectangle {
        color: Theme.selected

        border.color: Theme.accent
        border.width: 2
    }

    contentItem: RowLayout {
        Label {
            text: displayRecovery.controller.displays.status
            color: Theme.ink
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ActionButton {
            ToolTip.text: "Accept this monitor arrangement and stop the automatic rollback timer."
            text: "Keep Layout"
            enabled: !displayRecovery.controller.displays.busy
            onClicked: displayRecovery.controller.localDisplayCommand({
                operation: "keep"
            })
        }

        ActionButton {
            ToolTip.text: "Return to the monitor arrangement used before the pending display change."
            text: "Restore Previous"
            enabled: !displayRecovery.controller.displays.busy
            onClicked: displayRecovery.controller.localDisplayCommand({
                operation: "revert"
            })
        }
    }
}
