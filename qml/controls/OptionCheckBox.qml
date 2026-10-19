import QtQuick
import QtQuick.Controls.Basic

CheckBox {
    id: c

    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout
    ToolTip.visible: (hovered || visualFocus) && ToolTip.text.length > 0
    hoverEnabled: true
    spacing: Theme.spaceM
    leftPadding: 0
    implicitHeight: Theme.controlHeight

    indicator: Rectangle {
        x: 0
        y: (c.height - height) / 2
        width: 20
        height: 20

        color: Theme.field
        border.color: c.visualFocus ? Theme.accent : Theme.outlineStrong

        Rectangle {
            objectName: "checkedRectangle"
            anchors.centerIn: parent
            width: 10
            height: c.checkState === Qt.PartiallyChecked ? 3 : 10

            color: c.enabled ? Theme.accent : Theme.muted
            visible: c.checkState !== Qt.Unchecked
        }
    }

    contentItem: Text {
        text: c.text
        leftPadding: 20 + Theme.spaceS
        verticalAlignment: Text.AlignVCenter
        color: c.enabled ? Theme.ink : Theme.muted
        font.pixelSize: Theme.fontBody
    }
}
