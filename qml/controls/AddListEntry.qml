import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: entry

    property string title: ""
    property string detail: ""
    property string buttonText: "Add"
    property string buttonName: ""
    property bool canAdd: true
    property bool primary: false
    property bool compact: false
    signal clicked
    Layout.fillWidth: true
    implicitHeight: contents.implicitHeight + Theme.spaceXl
    color: Theme.surface

    RowLayout {
        id: contents
        anchors.fill: parent
        anchors.margins: Theme.spaceM
        spacing: Theme.spaceM

        ColumnLayout {
            Layout.fillWidth: true
            Label {
                visible: entry.title.length > 0
                Layout.fillWidth: true
                text: entry.title
                color: Theme.ink
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
            HintLabel {
                Layout.fillWidth: true
                text: entry.detail
                wrapMode: Text.WordWrap
            }
        }
        ActionButton {
            objectName: entry.buttonName
            text: entry.buttonText
            enabled: entry.canAdd
            primary: entry.primary
            compact: entry.compact
            ToolTip.text: entry.detail
            onClicked: entry.clicked()
        }
    }
}
