import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: saved

    required property var colourStore
    required property color currentColour
    property bool canChoose: true
    signal chosen(string colour)
    readonly property bool isFavourite: colourStore.favourites.indexOf(currentColour.toString()) >= 0
    spacing: 6

    RowLayout {
        Layout.fillWidth: true

        Label {
            text: "Favourites"
            color: Theme.ink
            Layout.fillWidth: true
        }

        ActionButton {
            objectName: "favouriteColour"
            compact: true
            text: saved.isFavourite ? "Remove Favourite" : "Add Favourite"
            enabled: saved.isFavourite || saved.colourStore.favourites.length < 32
            ToolTip.text: saved.isFavourite
                ? "Remove this colour from your saved favourites."
                : "Save the current colour for use anywhere in WHAT. Up to 32 favourites can be kept."
            onClicked: saved.colourStore.setFavourite(saved.currentColour, !saved.isFavourite)
        }
    }

    ColourRow {
        colours: saved.colourStore.favourites
        favourites: true
        emptyText: "Choose a colour, then add it to Favourites."
    }

    Label {
        text: "Previously Used Colours"
        color: Theme.ink
    }

    ColourRow {
        colours: saved.colourStore.recent
        emptyText: "Colours appear here after you use them."
    }

    component ColourRow: DragScrollView {
        id: row

        required property var colours
        required property string emptyText
        property bool favourites: false
        Layout.fillWidth: true
        Layout.preferredHeight: 38
        contentWidth: swatches.implicitWidth
        contentHeight: 30
        clip: true
        ScrollBar.vertical.policy: ScrollBar.AlwaysOff

        Row {
            id: swatches

            spacing: 6

            Label {
                text: row.emptyText
                color: Theme.muted
                visible: row.colours.length === 0
                height: 30
                verticalAlignment: Text.AlignVCenter
            }

            Repeater {
                model: row.colours

                delegate: AbstractButton {
                    required property string modelData
                    objectName: "savedColour_" + modelData
                    width: 30
                    height: 30
                    enabled: saved.canChoose
                    hoverEnabled: true
                    Accessible.name: "Choose " + modelData.toUpperCase()
                    ToolTip.text: "Choose " + modelData.toUpperCase() + (row.favourites
                        ? ". Right-click to remove this favourite."
                        : "")
                    ToolTip.visible: hovered
                    ToolTip.delay: Theme.toolTipDelay
                    ToolTip.timeout: Theme.toolTipTimeout
                    onClicked: saved.chosen(modelData)

                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        enabled: row.favourites
                        onTapped: removeMenu.popup()
                    }

                    Keys.onMenuPressed: if (row.favourites)
                        removeMenu.popup()

                    ContextMenu {
                        id: removeMenu

                        ContextMenuItem {
                            text: "Remove Favourite"
                            onTriggered: saved.colourStore.setFavourite(modelData, false)
                        }
                    }

                    background: Rectangle {
                        color: Theme.checkerDark

                        Rectangle {
                            width: 15
                            height: 15
                            color: Theme.checkerLight
                        }

                        Rectangle {
                            x: 15
                            y: 15
                            width: 15
                            height: 15
                            color: Theme.checkerLight
                        }

                        Rectangle {
                            anchors.fill: parent
                            color: modelData
                            border.width: 2
                            border.color: parent.parent.visualFocus ? Theme.accent : Theme.outlineStrong
                        }
                    }
                }
            }
        }
    }
}
