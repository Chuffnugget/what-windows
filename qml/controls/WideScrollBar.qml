import QtQuick
import QtQuick.Controls.Basic

ScrollBar {
    id: bar

    policy: ScrollBar.AsNeeded
    hoverEnabled: true
    implicitWidth: Theme.scrollBarWidth
    implicitHeight: Theme.scrollBarWidth
    padding: 4
    stepSize: Math.min(1, size * 0.7)
    minimumSize: Math.min(1, 36 / Math.max(1, orientation === Qt.Vertical ? height : width))

    background: Rectangle {
        color: bar.hovered || bar.pressed ? Theme.hover : "transparent"

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationsEnabled ? Theme.timings.controls : 0
            }
        }
    }

    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 6

        color: bar.pressed || bar.hovered ? Theme.accent : Theme.outlineStrong

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationsEnabled ? Theme.timings.controls : 0
            }
        }
    }
}
