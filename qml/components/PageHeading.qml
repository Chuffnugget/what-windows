import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

Item {
    id: root

    required property string title
    required property string subtitle
    implicitHeight: 64

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5

        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            elide: Text.ElideRight
            text: root.title
            color: Theme.ink
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }

        HintLabel {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            elide: Text.ElideRight
            text: root.subtitle
        }
    }
}
