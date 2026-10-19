import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: root

    property Item backdropSource: null

    property bool animationsEnabled: true
    property int hoverDuration: 140
    required property var controller
    readonly property var state: controller.displays
    Layout.fillWidth: true

    RowLayout {
        Layout.fillWidth: true

        Label {
            text: "DISPLAY LAYOUT"
            color: Theme.muted
            font.pixelSize: Theme.fontLabel
            font.letterSpacing: 1
        }

        ChoiceBox {
            id: layouts

            Layout.preferredWidth: 190
            model: root.state.layouts || []
            textRole: "name"
            valueRole: "id"
            currentIndex: (root.state.layouts || []).findIndex(l => l.id === root.state.active_layout)
            displayText: currentIndex >= 0 ? currentText : "Current arrangement"
            enabled: root.state.available && !root.state.busy && !root.state.pending && count > 0
            onActivated: root.controller.localDisplayCommand({
                operation: "restore",
                layout_id: currentValue
            })
            ToolTip.visible: hovered
            ToolTip.text: "Restore a named arrangement of all displays"
        }

        ActionButton {
            ToolTip.text: "Save, restore or delete named monitor arrangements for this PC."
            text: "Manage Layouts"
            onClicked: manager.open()
        }

        Item {
            Layout.fillWidth: true
        }

        ActionButton {
            ToolTip.text: "Read the connected monitors and their current layout, resolution and refresh rate from Windows. This does not change your display settings."
            text: "Rescan Monitors"
            objectName: "rescanMonitors"
            enabled: !root.state.busy
            onClicked: root.controller.refreshDisplays()
        }
    }

    Label {
        Layout.fillWidth: true
        text: root.state.status || ""
        color: Theme.muted
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.fontSmall
    }

    Popup {
        id: manager
        property PopupMotion popupMotion: PopupMotion { popup: manager }
        enter: popupMotion.enter
        exit: popupMotion.exit

        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(620, parent.width - 40)
        padding: 22
        modal: true
        focus: true

        Overlay.modal: GlassBackdrop {
            sourceItem: root.backdropSource
        }

        background: GlassSurface {}

        contentItem: ColumnLayout {
            spacing: Theme.spaceM

            Label {
                text: "Saved Display Layouts"
                color: Theme.ink
                font.pixelSize: Theme.fontHeading
                font.bold: true
            }

            Label {
                Layout.fillWidth: true
                text: "Arrange your monitors here or in Windows Display settings, then save the current arrangement. Home Assistant can restore these layouts by name."
                color: Theme.muted
                wrapMode: Text.WordWrap
            }

            RowLayout {
                InputField {
                    id: layoutName

                    ToolTip.text: "Name this monitor arrangement so you or Home Assistant can restore it later."
                    hoverEnabled: true
                    ToolTip.visible: hovered
                    ToolTip.delay: Theme.toolTipDelay
                    ToolTip.timeout: Theme.toolTipTimeout
                    Layout.fillWidth: true
                    placeholderText: "Layout name, e.g. Desk or TV"
                    maximumLength: 64
                }

                ActionButton {
                    ToolTip.text: "Save the current monitor arrangement under the name entered here."
                    text: "Save Current"
                    enabled: !root.state.busy && !root.state.pending && layoutName.text.trim().length > 0
                    onClicked: root.controller.localDisplayCommand({
                        operation: "save",
                        name: layoutName.text
                    })
                }
            }

            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(240, contentHeight)
                clip: true
                model: root.state.layouts || []

                ScrollBar.vertical: WideScrollBar {
                }

                delegate: RowLayout {
                    required property var modelData
                    width: ListView.view.width - 18
                    height: 48

                    Label {
                        text: modelData.name + (modelData.available ? "" : " - display missing")
                        color: Theme.ink
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }

                    ActionButton {
                        ToolTip.text: "Apply this saved monitor arrangement. WHAT will ask you to keep it or restore the previous layout."
                        text: "Restore"
                        enabled: modelData.available && !root.state.busy && !root.state.pending
                        onClicked: root.controller.localDisplayCommand({
                            operation: "restore",
                            layout_id: modelData.id
                        })
                    }

                    ActionButton {
                        ToolTip.text: "Remove this saved layout from WHAT. Your current monitor arrangement stays in place."
                        text: "Delete"
                        enabled: !root.state.busy && !root.state.pending
                        onClicked: root.controller.localDisplayCommand({
                            operation: "delete",
                            layout_id: modelData.id
                        })
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: root.state.status || ""
                color: Theme.accent
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: "Layouts preserve desktop membership, mirror groups, primary desktop, positions and source sizes. Windows chooses compatible output timings; exact refresh rate, HDR and scaling are not saved. Moving a cable to another port can change monitor identity."
                color: Theme.muted
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSmall
            }

            ActionButton {
                text: "Close"
                Layout.alignment: Qt.AlignRight
                onClicked: manager.close()
            }
        }
    }
}
