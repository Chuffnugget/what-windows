import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../dialogs"

ColumnLayout {
    id: pairing

    required property var controller
    required property Item backdropSource
    property bool hideAddress: true
    property string selectedInstallationId: ""
    readonly property var installations: controller.installationConfiguration
    readonly property var selectedInstallation: installations.find(row => row.id === selectedInstallationId) || null
    readonly property var status: controller.info
    readonly property bool blocksWebView: editor.blocksWebView || tokenDialog.blocksWebView
    signal renewCertificateRequested()
    signal notification(string message)
    spacing: Theme.spaceM

    // Model refreshes must not move the selection to another installation's token.
    function syncSelection() {
        if (!selectedInstallation)
            selectedInstallationId = installations.length ? installations[0].id : "";
        installationChoice.currentIndex = installationChoice.indexOfValue(selectedInstallationId);
    }

    onInstallationsChanged: Qt.callLater(syncSelection)
    Component.onCompleted: syncSelection()

    InstallationEditor {
        id: editor
        objectName: "pairingInstallationEditor"
        controller: pairing.controller
        backdropSource: pairing.backdropSource
        onSaved: id => {
            pairing.selectedInstallationId = id;
            pairing.syncSelection();
        }
    }

    PairingTokenDialog {
        id: tokenDialog
        objectName: "selectedInstallationTokenDialog"
        controller: pairing.controller
        backdropSource: pairing.backdropSource
        onNotification: message => pairing.notification(message)
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: "Choose an existing installation to pair again or update its details. Use Add for another Home Assistant server."
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceM

        HintLabel { text: "Installation" }

        ChoiceBox {
            id: installationChoice
            objectName: "pairingInstallation"
            Layout.fillWidth: true
            model: pairing.installations
            textRole: "name"
            valueRole: "id"
            enabled: count > 0
            displayText: count ? currentText : "No Installations Yet"
            ToolTip.text: "Choose the Home Assistant server you want to pair. Each installation has its own token."
            onActivated: pairing.selectedInstallationId = currentValue
        }

        ActionButton {
            objectName: "pairingAddInstallation"
            text: "Add"
            ToolTip.text: "Add another Home Assistant installation and select it for pairing."
            onClicked: editor.edit(null)
        }

        QuietButton {
            objectName: "pairingEditInstallation"
            text: "Edit"
            enabled: pairing.selectedInstallation !== null
            ToolTip.text: "Edit the selected installation's name, connection and permissions. Its token and identity are kept."
            onClicked: editor.edit(pairing.selectedInstallation)
        }
    }

    HintLabel {
        objectName: "pairingInstructions"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: !pairing.selectedInstallation
            ? "Add an installation to create its pairing token."
            : pairing.selectedInstallation.type === "mqtt"
                ? "Choose MQTT in Home Assistant's WHAT setup. WHAT and Home Assistant's MQTT integration must use the same broker. Copy the PC device ID, PC name, token and fingerprint below. No PC address or port is needed."
                : "Choose WHAT API in Home Assistant's WHAT setup. Copy the pairing details below. For local access, enter this PC's address and port and allow inbound TCP on that port for WHAT on your Private network in Windows Firewall. A Public network or a blocked port can prevent local pairing; do not open the port on your router. For remote access, leave the PC address blank and set this Home Assistant's HTTPS address using Edit."
    }

    HintLabel {
        Layout.fillWidth: true
        visible: pairing.selectedInstallation !== null && !pairing.selectedInstallation.enabled
        text: "This installation is disabled. Enable it using Edit before connecting Home Assistant."
        color: Theme.warning
        wrapMode: Text.WordWrap
    }

    RowLayout {
        visible: pairing.selectedInstallation !== null && pairing.selectedInstallation.type === "api"
        Layout.fillWidth: true
        spacing: Theme.spaceXl

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceXs

            HintLabel {
                text: "LOCAL ADDRESS"
                font.pixelSize: Theme.fontLabel
                font.letterSpacing: 1
            }

            Label {
                text: pairing.hideAddress
                    ? "•••.•••.•••.•••"
                    : (pairing.status.addresses || "No active IPv4 address")
                color: Theme.ink
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
            }
        }

        ColumnLayout {
            spacing: Theme.spaceXs

            HintLabel {
                text: "PORT"
                font.pixelSize: Theme.fontLabel
                font.letterSpacing: 1
            }

            Label {
                text: pairing.hideAddress ? "•••••" : pairing.status.port
                color: Theme.ink
            }
        }

        OptionCheckBox {
            ToolTip.text: "Hide the address and port on this page. They start hidden whenever WHAT opens; this choice is not saved."
            objectName: "hidePairingAddress"
            text: "Hide Address"
            checked: pairing.hideAddress
            onClicked: pairing.hideAddress = checked
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spaceM

        ActionButton {
            objectName: "pairingCopyDetails"
            text: "Copy Pairing Details"
            enabled: pairing.selectedInstallation !== null
            primary: true
            ToolTip.text: "Copy this installation's token, PC device ID, PC name, fingerprint and local address. Only paste these details into a trusted Home Assistant."
            onClicked: {
                controller.copyInstallationPairing(pairing.selectedInstallationId);
                pairing.notification("Pairing details copied");
            }
        }

        QuietButton {
            text: "Copy Token"
            enabled: pairing.selectedInstallation !== null
            ToolTip.text: "Copy the existing token for the selected installation without replacing it."
            onClicked: {
                controller.copyInstallationToken(pairing.selectedInstallationId);
                pairing.notification("Pairing token copied");
            }
        }

        QuietButton {
            text: "Copy Fingerprint"
            ToolTip.text: "Copy this PC's certificate fingerprint. It is shared by all installations."
            onClicked: {
                controller.copyFingerprint();
                pairing.notification("Certificate fingerprint copied");
            }
        }
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        visible: pairing.selectedInstallation !== null
        text: "Already paired? Use Reconfigure on the existing WHAT entry in Home Assistant to keep its devices and entities. You only need to replace a token or renew the certificate when those credentials need changing."
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spaceM

        QuietButton {
            objectName: "pairingReplaceToken"
            text: "Replace Token…"
            enabled: pairing.selectedInstallation !== null
            ToolTip.text: "Replace only the selected installation's token after confirmation. Other installations keep their tokens."
            onClicked: tokenDialog.openForInstallation(pairing.selectedInstallation)
        }

        QuietButton {
            objectName: "pairingRenewCertificate"
            text: "Renew Shared Certificate…"
            ToolTip.text: "Renew this PC's shared pairing certificate after confirmation. Every paired Home Assistant must then use its new fingerprint. MQTT broker certificates are separate."
            onClicked: pairing.renewCertificateRequested()
        }
    }
}
