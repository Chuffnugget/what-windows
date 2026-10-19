import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: updateDialog

    required property var updater

    objectName: "installUpdatePrompt"
    title: updateDialog.updater.info.downgrade ? "Install Older WHAT Version" : "Install WHAT Update"
    modal: true
    anchors.centerIn: parent
    property string confirmedVersion: ""
    onOpened: confirmedVersion = updateDialog.updater.info.available
    width: Math.min(520, updateDialog.parent.width - 40)
    standardButtons: Dialog.Ok | Dialog.Cancel

    Label {
        width: parent.width
        wrapMode: Text.WordWrap
        text: "Install WHAT " + updateDialog.confirmedVersion + " and restart?\nHome Assistant will briefly disconnect." + (updateDialog.updater.info.downgrade
            ? "\nThis is an older release. Back up your WHAT profile first: older versions may not understand newer settings."
            : "\nSettings and pairing are retained.") + (updateDialog.confirmedVersion.indexOf("-beta.") >= 0
            ? "\nBeta software may contain unfinished features or regressions."
            : "")
        color: Theme.ink
    }

    onAccepted: updateDialog.updater.install(confirmedVersion)
}
