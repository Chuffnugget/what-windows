import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: clearWebDataDialog

    required property var controller
    objectName: "clearWebDataDialog"
    title: "Clear Home Assistant Web Login?"
    modal: true
    anchors.centerIn: parent
    standardButtons: Dialog.Ok | Dialog.Cancel
    onAccepted: clearWebDataDialog.controller.homeAssistantWeb.clearCache(true)
    contentItem: HintLabel {
        text: "This signs you out of the embedded dashboard and clears its browser data. WHAT settings, sensors and the integration pairing are kept."
        wrapMode: Text.WordWrap
    }
}
