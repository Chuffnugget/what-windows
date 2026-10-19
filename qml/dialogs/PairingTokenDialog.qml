import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: rotateDialog

    required property var controller

    signal notification(string message)
    property bool allInstallations: true
    property string installationId: ""
    property string installationName: ""

    function openForInstallation(installation) {
        allInstallations = false;
        installationId = installation.id;
        installationName = installation.name;
        open();
    }

    objectName: "pairingTokenDialog"
    anchors.centerIn: parent
    title: allInstallations ? "Replace All Pairing Tokens?" : "Replace Token for " + installationName + "?"
    width: Math.min(620, rotateDialog.parent.width - 48)
    modal: true
    standardButtons: Dialog.Ok | Dialog.Cancel

    Label {
        text: rotateDialog.allInstallations
            ? "Every installation will receive a new pairing token and disconnect.\nCopy the new pairing details for each installation and reconfigure its existing WHAT entry in Home Assistant."
            : "Only " + rotateDialog.installationName + " will receive a new token and disconnect. Other installations keep their tokens.\n\nCopy the new pairing details, then use Reconfigure on the existing WHAT entry in that Home Assistant. Keep the entry to retain its devices, entities and settings. The PC identity and certificate stay the same."
        width: rotateDialog.availableWidth
        wrapMode: Text.WordWrap
        color: Theme.ink
    }

    onAccepted: {
        if (allInstallations) {
            controller.rotateToken();
        } else if (controller.rotateInstallationToken(installationId)) {
            notification("Token replaced for " + installationName + ". Copy its pairing details and reconfigure the existing Home Assistant entry.");
        } else {
            notification(controller.installationError);
        }
    }
}
