import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: certificateDialog

    required property var controller
    signal notification(string message)

    objectName: "certificateDialog"
    property bool renewed: false
    property string failure: ""
    title: renewed ? "Home Assistant Certificate Renewed" : "Renew Home Assistant Certificate"
    modal: true
    anchors.centerIn: parent
    width: Math.min(640, parent.width - 48)
    standardButtons: renewed ? Dialog.Close : Dialog.Ok | Dialog.Cancel

    contentItem: ColumnLayout {
        spacing: Theme.spaceM

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: certificateDialog.renewed
                ? "Update the fingerprint in every Home Assistant installation paired with this PC. Open Settings > Devices & services > WHAT and use Reconfigure on its existing entry. Each installation keeps its own token. Keep the entries to retain their devices, entities and settings."
                : "Renew this PC's shared pairing certificate? This affects every paired installation, using WHAT API or MQTT, until you update its fingerprint in Home Assistant. Each installation's token, sensor selections and PC identity are kept. This does not change MQTT broker certificates."
            color: Theme.ink
        }

        Label {
            Layout.fillWidth: true
            visible: certificateDialog.failure.length > 0
            text: certificateDialog.failure
            color: Theme.error
            wrapMode: Text.WordWrap
        }

        RowLayout {
            visible: certificateDialog.renewed

            ActionButton {
                objectName: "renewedCertificateCopyFingerprint"
                text: "Copy New Fingerprint"
                ToolTip.text: "Copy the replacement certificate fingerprint for Home Assistant's Reconfigure form."
                onClicked: {
                    certificateDialog.controller.copyFingerprint();
                    certificateDialog.notification("New certificate fingerprint copied");
                }
            }
        }
    }

    onAccepted: if (!renewed) certificateDialog.controller.renewCertificate()
}
