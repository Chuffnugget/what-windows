import QtQuick
import QtQuick.Controls.Basic

Slider {
    id: slider

    property color tint: Theme.accent
    property bool compact: false
    implicitWidth: 230
    implicitHeight: compact ? Theme.compactHeight : Theme.controlHeight
    horizontalPadding: Theme.spaceS
    hoverEnabled: true
    ToolTip.visible: (hovered || visualFocus) && ToolTip.text.length > 0
    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout

    background: Rectangle {
        x: slider.leftPadding
        y: (slider.height - height) / 2
        width: slider.availableWidth
        height: 6
        color: Theme.outline

        Rectangle {
            x: slider.mirrored ? parent.width - width : 0
            width: slider.position * parent.width
            height: parent.height
            color: slider.enabled ? slider.tint : Theme.muted
        }
    }

    handle: Rectangle {
        x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
        y: (slider.height - height) / 2
        width: 12
        height: 20
        color: !slider.enabled ? Theme.muted : slider.pressed ? slider.tint : Theme.ink
        border.width: slider.visualFocus ? 2 : 0
        border.color: Theme.accent
    }
}
