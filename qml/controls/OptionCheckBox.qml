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
    // Configure the indicator without forcing deferred creation from a cached row.
    property bool indicatorCentered: false
    property bool indicatorVisible: true

    indicator: Rectangle {
        x: c.indicatorCentered ? (c.width - width) / 2 : 0
        y: (c.height - height) / 2
        width: 20
        height: 20
        visible: c.indicatorVisible

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
