import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: list
    required property var controller
    property Item backdropSource: null
    property double now: Date.now()
    spacing: 1
    Layout.fillWidth: true
    Timer {
        interval: 1000
        repeat: true
        running: list.visible
        onTriggered: list.now = Date.now()
    }
    InstallationEditor {
        id: editor
        controller: list.controller
        backdropSource: list.backdropSource
    }
    Repeater {
        model: list.controller.installationConfiguration
        delegate: Rectangle {
            id: row
            required property var modelData
            readonly property var connection: list.controller.installationStatus[modelData.id] || ({})
            Layout.fillWidth: true
            implicitHeight: contents.implicitHeight + 24
            color: Theme.surface
            RowLayout {
                id: contents
                anchors.fill: parent
                anchors.margins: 12
                spacing: Theme.spaceS
                OptionCheckBox {
                    checked: row.modelData.enabled
                    Accessible.name: "Enable " + row.modelData.name
                    ToolTip.text: "Enable or disable only this installation. Settings and pairing details are kept."
                    onClicked: list.controller.setInstallationEnabled(row.modelData.id, checked)
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label {
                        text: row.modelData.name
                        color: Theme.ink
                        font.weight: Font.DemiBold
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    HintLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: ((row.connection.activeType || row.modelData.type) === "mqtt" ? "MQTT" : "Home Assistant API")
                            + " · " + (row.connection.status || "Waiting for Home Assistant")
                            + (row.modelData.controls ? "" : " · Readings Only")
                            + (row.connection.connected && row.connection.lastCommunication > 0
                                ? " · Last Response " + Math.max(0, Math.floor((list.now - row.connection.lastCommunication) / 1000)) + " s Ago" : "")
                    }
                }
                QuietButton {
                    property bool copied: false
                    text: copied ? "Copied" : "Pairing"
                    ToolTip.text: "Copy this installation's PC address, port, pairing token and certificate fingerprint. Add WHAT in the matching Home Assistant installation using these details."
                    onClicked: {
                        list.controller.copyInstallationPairing(row.modelData.id);
                        copied = true;
                        copyFeedback.restart();
                    }
                    Timer { id: copyFeedback; interval: 1800; onTriggered: parent.copied = false }
                }
                QuietButton {
                    text: "Edit"
                    ToolTip.text: "Edit the name, transport, credentials or control permissions for this installation."
                    onClicked: editor.edit(row.modelData)
                }
                QuietButton {
                    text: "Delete"
                    ToolTip.text: "Remove this installation from WHAT and revoke its pairing token. Home Assistant entities and history are not deleted."
                    onClicked: {
                        removal.installationId = row.modelData.id;
                        removal.installationName = row.modelData.name;
                        removal.open();
                    }
                }
            }
        }
    }
    AddListEntry {
        title: "Add a Home Assistant Installation"
        detail: "Connect another Home Assistant installation using Home Assistant API or MQTT. All enabled installations can receive readings at the same time."
        buttonText: "Add Installation"
        buttonName: "addInstallation"
        onClicked: editor.edit(null)
    }
    Dialog {
        id: removal
        property string installationId: ""
        property string installationName: ""
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(560, parent.width - 40)
        modal: true
        title: "Delete Installation"
        padding: 20
        implicitHeight: removalText.implicitHeight + 144
        contentHeight: removalText.implicitHeight
        background: GlassSurface {}
        Overlay.modal: GlassBackdrop { sourceItem: list.backdropSource }
        enter: null
        exit: null
        contentItem: Label {
            id: removalText
            width: removal.availableWidth
            text: "Delete “" + removal.installationName + "” and revoke its access to this PC? Its Home Assistant entities and history will be kept."
            color: Theme.ink
            wrapMode: Text.WordWrap
        }
        header: Label {
            text: removal.title
            padding: 20
            color: Theme.ink
            font.pixelSize: Theme.fontHeading
        }
        footer: Item {
            implicitHeight: 64
            RowLayout {
                anchors.fill: parent
                anchors.margins: 16
                Item { Layout.fillWidth: true }
                QuietButton { text: "Cancel"; onClicked: removal.close() }
                ActionButton {
                    text: "Delete Installation"
                    onClicked: {
                        list.controller.deleteInstallation(removal.installationId);
                        removal.close();
                    }
                }
            }
        }
    }
}
