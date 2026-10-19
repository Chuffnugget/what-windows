import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../components"

Dialog {
    id: library

    required property var manager
    property Item backdropSource: null
    signal editRequested(var definition)
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(720, parent.width - 40)
    height: Math.min(600, parent.height - 40)
    modal: true
    title: "Custom Sensors"
    header: Label {
        text: library.title
        color: Theme.ink
        font.pixelSize: Theme.fontHeading
        padding: 18
    }
    standardButtons: Dialog.Close
    footer: PromptButtons { standardButtons: library.standardButtons }
    background: GlassSurface {}
    property PopupMotion popupMotion: PopupMotion { popup: library }
    enter: popupMotion.enter
    exit: popupMotion.exit
    Overlay.modal: GlassBackdrop {
        sourceItem: library.backdropSource
    }

    contentItem: ColumnLayout {
        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Configure what to read here. Choose which readings Home Assistant receives using Local and Cloud in the sensor browser. Removing a definition keeps Home Assistant history."
        }
        RowLayout {
            ActionButton {
                text: "Add Custom Sensor"
                onClicked: {
                    library.close();
                    library.editRequested(library.manager.newDefinition("sensor"));
                }
            }
            ActionButton {
                visible: !library.manager.enabled
                text: "Enable Custom Module"
                onClicked: library.manager.enabled = true
            }
        }
        ListView {
            id: definitions
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            reuseItems: true
            model: library.manager.sensors
            ScrollBar.vertical: WideScrollBar {}
            delegate: RowLayout {
                required property var modelData
                width: definitions.width - 18
                height: 48
                Label {
                    Layout.fillWidth: true
                    text: modelData.name
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.ink
                }
                HintLabel {
                    text: modelData.outputs.length + " reading(s)" + (modelData.enabled ? "" : " - Disabled")
                }
                QuietButton {
                    text: "Edit"
                    onClicked: {
                        library.close();
                        library.editRequested(modelData);
                    }
                }
                QuietButton {
                    text: "Remove"
                    onClicked: {
                        removal.identifier = modelData.id;
                        removal.sensorName = modelData.name;
                        removal.open();
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: definitions.count === 0
                text: "No custom sensors yet"
                color: Theme.muted
            }
        }
    }

    Dialog {
        id: removal
        property string identifier
        property string sensorName
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(480, parent.width - 40)
        title: "Remove Sensor Definition?"
        header: Label {
            text: removal.title
            color: Theme.ink
            font.pixelSize: Theme.fontHeading
            padding: 18
        }
        modal: true
        standardButtons: Dialog.Yes | Dialog.Cancel
        footer: PromptButtons { standardButtons: removal.standardButtons }
        property PopupMotion popupMotion: PopupMotion { popup: removal }
        enter: popupMotion.enter
        exit: popupMotion.exit
        Overlay.modal: GlassBackdrop { sourceItem: library.backdropSource }
        background: GlassSurface {}
        contentItem: Label {
            text: "Remove " + removal.sensorName
                  + "? Its readings will become unavailable. Home Assistant history is kept."
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.ink
        }
        onAccepted: library.manager.remove(identifier)
    }
}
