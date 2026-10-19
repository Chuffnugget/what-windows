import QtQuick
import QtQuick.Controls.Basic
import "../controls"

AbstractButton {
    id: button

    property bool selected: checked
    property string help: ""
    property bool subtle: false
    implicitWidth: Math.max(26, label.implicitWidth + 18)
    implicitHeight: Theme.compactHeight
    hoverEnabled: true
    Accessible.name: text
    ToolTip.text: help
    ToolTip.visible: (hovered || visualFocus) && help.length > 0
    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout

    contentItem: Text {
        id: label

        text: button.text
        color: !button.enabled ? Theme.muted
            : button.selected ? Theme.accent : button.hovered ? Theme.ink : Theme.muted
        font.pixelSize: Theme.fontCaption
        font.weight: button.selected ? Font.DemiBold : Font.Normal
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: !button.enabled
            ? (button.subtle ? "transparent" : Theme.inset)
            : button.down
            ? Theme.pressed
            : button.selected
            ? Theme.selected
            : button.hovered
            ? Theme.hover
            : button.subtle
            ? "transparent"
            : Theme.field
        border.width: button.visualFocus ? 1 : 0
        border.color: Theme.accent
    }
}
