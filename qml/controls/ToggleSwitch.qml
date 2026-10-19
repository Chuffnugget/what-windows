import QtQuick
import QtQuick.Controls.Basic

Switch {
    id: control

    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout
    ToolTip.visible: (hovered || visualFocus) && ToolTip.text.length > 0
    hoverEnabled: true
    implicitHeight: Theme.controlHeight
    property int animationDuration: Theme.animationsEnabled ? Theme.timings.controls : 0
    property color checkedColor: Theme.accent

    indicator: Rectangle {
        implicitWidth: 44
        implicitHeight: 24
        width: implicitWidth
        height: implicitHeight
        x: 4
        y: (control.height - height) / 2

        color: control.enabled && control.checked ? control.checkedColor : Theme.outline
        border.width: control.visualFocus ? 1 : 0
        border.color: Theme.accent

        Rectangle {
            width: 18
            height: 18

            y: 3
            x: control.checked ? 23 : 3
            color: control.enabled ? Theme.ink : Theme.muted

            Behavior on x {
                NumberAnimation {
                    duration: control.animationDuration
                }
            }
        }
    }
}
