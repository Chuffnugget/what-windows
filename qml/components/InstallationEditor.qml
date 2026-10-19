import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

Dialog {
    id: editor
    objectName: "installationEditor"
    required property var controller
    property Item backdropSource: null
    property string installationId: ""
    property bool hasPassword: false
    readonly property bool blocksWebView: visible && modal
    property bool saving: false
    signal saved(string id)

    Connections {
        target: editor.controller
        enabled: editor.saving

        function onInstallationSaved(id) {
            editor.saved(id);
        }
    }
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(740, parent.width - 40)
    height: Math.min(740, parent.height - 40)
    modal: true
    padding: 20
    closePolicy: Popup.CloseOnEscape
    title: installationId ? "Edit Home Assistant Installation" : "Add Home Assistant Installation"
    enter: null
    exit: null
    background: GlassSurface {}
    Overlay.modal: GlassBackdrop { sourceItem: editor.backdropSource }

    function edit(row) {
        const value = row || {};
        const mqtt = value.mqtt || {};
        installationId = value.id || "";
        nameField.text = value.name || "";
        typeField.currentIndex = value.type === "mqtt" ? 1 : 0;
        enabledField.checked = value.enabled !== false;
        controlsField.checked = value.controls !== false;
        urlField.text = value.url || "";
        hostField.text = mqtt.host || "";
        portField.value = mqtt.port || 8883;
        tlsField.checked = mqtt.tls !== false;
        localField.checked = mqtt.scope === "local";
        userField.text = mqtt.username || "";
        passwordField.text = "";
        clearPassword.checked = false;
        hasPassword = value.hasPassword === true;
        caField.text = mqtt.ca || "";
        certificateField.text = mqtt.certificate || "";
        keyField.text = mqtt.key || "";
        errorText.text = "";
        open();
    }

    function save() {
        const value = {
            id: installationId,
            name: nameField.text,
            type: typeField.currentIndex === 1 ? "mqtt" : "api",
            enabled: enabledField.checked,
            controls: controlsField.checked,
            url: urlField.text,
            mqtt: {
                host: hostField.text.trim(), port: portField.value,
                tls: tlsField.checked, scope: localField.checked ? "local" : "cloud",
                username: userField.text, ca: caField.text,
                certificate: certificateField.text, key: keyField.text
            }
        };
        // Ignore saves from other editors or connection recovery callbacks.
        saving = true;
        const succeeded = controller.saveInstallation(value, passwordField.text, clearPassword.checked);
        saving = false;
        if (succeeded)
            close();
        else
            errorText.text = controller.installationError;
    }

    header: Label {
        text: editor.title
        padding: 20
        color: Theme.ink
        font.pixelSize: Theme.fontHeading
        wrapMode: Text.WordWrap
    }

    contentItem: DragScrollView {
        id: scroll
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical: WideScrollBar {
            x: scroll.width - width
            y: scroll.topPadding
            height: scroll.availableHeight
        }
        ColumnLayout {
            width: scroll.availableWidth - 18
            spacing: Theme.spaceM
            HintLabel { text: "Installation Name" }
            InputField {
                id: nameField
                objectName: "installationName"
                Layout.fillWidth: true
                maximumLength: 80
                placeholderText: "For example: Main House"
                ToolTip.text: "A name used in WHAT to identify this Home Assistant installation. Renaming it does not change entity IDs."
            }
            RowLayout {
                Layout.fillWidth: true
                HintLabel { text: "Active Connection" }
                ChoiceBox {
                    id: typeField
                    objectName: "installationType"
                    Layout.fillWidth: true
                    model: ["Home Assistant API (Recommended)", "MQTT"]
                    ToolTip.text: "Choose the connection that carries readings and commands. Both configurations are saved. WHAT checks the new connection before switching and keeps the current connection if the check fails."
                }
                OptionCheckBox {
                    id: enabledField
                    objectName: "installationEnabled"
                    text: "Enabled"
                    ToolTip.text: "Connect this installation and accept its paired connections. Disabling it preserves its settings."
                }
            }
            HintLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: typeField.currentIndex === 0
                    ? "Recommended for most users. Connect to Home Assistant without an MQTT broker."
                    : "Use an existing MQTT broker, including one shared with Zigbee2MQTT. Connecting other services requires a compatible bridge for WHAT's protocol."
            }
            OptionCheckBox {
                id: controlsField
                text: "Allow Controls From This Installation"
                ToolTip.text: "Allow this Home Assistant installation to use controls already permitted in WHAT. Module and Local/Cloud restrictions still apply. Turn off for readings only."
            }
            HintLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: editor.installationId
                    ? "Editing keeps this installation's pairing token, PC identity and entity IDs. Use Replace Token in Pair This PC if you need a new token."
                    : "Add a separate Home Assistant server here. To update an existing pairing, cancel and select that installation in Pair This PC instead."
            }
            HintLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "Use one entry per Home Assistant installation. Configure both connections below, then choose which one is active. MQTT must already be set up in Home Assistant."
            }
            HintLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.warning
                readonly property string address: urlField.text.trim().toLowerCase().replace(/\/$/, "")
                visible: address.length > 0 && controller.installationConfiguration.some(row =>
                    row.id !== editor.installationId && (row.url || "").trim().toLowerCase().replace(/\/$/, "") === address)
                text: "Another installation already uses this Home Assistant address. Edit that entry if it is the same server."
            }
            ColumnLayout {
                Layout.fillWidth: true
                Label {
                    text: "Home Assistant API" + (typeField.currentIndex === 0 ? " - Selected" : " - Inactive")
                    color: Theme.ink
                    font.weight: Font.DemiBold
                }
                HintLabel { text: "Home Assistant HTTPS Address (Optional)" }
                InputField {
                    id: urlField
                    objectName: "installationUrl"
                    Layout.fillWidth: true
                    maximumLength: 2048
                    placeholderText: "https://your-instance.ui.nabu.casa"
                    ToolTip.text: "Leave blank for Home Assistant to connect directly to this PC. Set an HTTPS address for WHAT to connect when this installation's direct connection is unavailable."
                }
                HintLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "After saving, copy this installation's pairing details. In Home Assistant, choose WHAT API. Enter the PC address and port for local access, or leave the PC address blank for remote access using the HTTPS address above."
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceM
                Label {
                    text: "MQTT" + (typeField.currentIndex === 1 ? " - Selected" : hostField.text.trim() ? " - Configured, Inactive" : " - Not Configured")
                    color: Theme.ink
                    font.weight: Font.DemiBold
                }
                HintLabel { text: "MQTT Broker" }
                RowLayout {
                    InputField {
                        id: hostField
                        objectName: "installationMqttHost"
                        Layout.fillWidth: true
                        placeholderText: "Broker hostname"
                        ToolTip.text: "The broker used by this Home Assistant installation. Enter a hostname or IP address without a scheme or topic. Zigbee2MQTT is another client of the broker; it is not the broker itself."
                    }
                    NumberInput {
                        id: portField
                        unit: ""
                        from: 1
                        to: 65535
                        ToolTip.text: "Your MQTT broker port, commonly 8883 for TLS."
                    }
                }
                OptionCheckBox {
                    id: tlsField
                    text: "Use TLS and Verify the Broker Certificate"
                    ToolTip.text: "Required for Cloud sharing. WHAT verifies the broker's certificate and hostname."
                }
                OptionCheckBox {
                    id: localField
                    text: "Use Local Sharing Permissions"
                    ToolTip.text: "Use the Local sensor selections for a broker on a trusted network. Leave off to apply Cloud selections and restrictions."
                }
                InputField {
                    id: userField
                    Layout.fillWidth: true
                    placeholderText: "Broker username (optional)"
                    ToolTip.text: "The login configured on this MQTT broker."
                }
                InputField {
                    id: passwordField
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: editor.hasPassword ? "Password saved - leave blank to keep" : "Broker password (optional)"
                    ToolTip.text: "The password is protected by Windows and is never shown in the installation list or logs."
                }
                OptionCheckBox {
                    id: clearPassword
                    text: "Clear Saved Password"
                    visible: editor.hasPassword
                }
                SettingsSection {
                    title: "Certificates"
                    detail: "Optional CA and client authentication files"
                    InputField {
                        id: caField
                        Layout.fillWidth: true
                        placeholderText: "CA certificate path (blank uses Windows trust)"
                        ToolTip.text: "Optional CA certificate file used to verify this broker."
                    }
                    InputField {
                        id: certificateField
                        Layout.fillWidth: true
                        placeholderText: "Client certificate path"
                        ToolTip.text: "Optional client certificate if the broker requires mutual TLS."
                    }
                    InputField {
                        id: keyField
                        Layout.fillWidth: true
                        placeholderText: "Client private key path"
                        ToolTip.text: "Private key matching the client certificate. Supply both files together."
                    }
                }
                HintLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "In Home Assistant, choose MQTT when pairing WHAT. Its MQTT integration must use this same broker. A paired installation can later switch between WHAT API and MQTT without creating another entry."
                }
            }
            HintLabel {
                id: errorText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.error
                visible: text.length > 0
            }
        }
    }
    footer: Item {
        implicitHeight: 64
        RowLayout {
            anchors.fill: parent
            anchors.margins: 16
            Item { Layout.fillWidth: true }
            QuietButton { text: "Cancel"; onClicked: editor.close() }
            ActionButton {
                objectName: "saveInstallation"
                text: "Save Installation"
                ToolTip.text: "Save this installation and apply its connection settings."
                onClicked: editor.save()
            }
        }
    }
    onClosed: passwordField.text = ""
}
