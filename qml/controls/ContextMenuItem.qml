import QtQuick
import QtQuick.Controls.Basic

MenuItem {
    id: item

    implicitHeight: Theme.controlHeight
    leftPadding: Theme.spaceM
    rightPadding: Theme.spaceM

    contentItem: Text {
        text: item.text
        color: item.enabled ? Theme.ink : Theme.muted
        font.pixelSize: Theme.fontBody
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: item.enabled && item.highlighted ? Theme.hover : "transparent"
        border.width: item.visualFocus ? 1 : 0
        border.color: Theme.accent
    }
}
