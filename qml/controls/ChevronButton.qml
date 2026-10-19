import QtQuick
import QtQuick.Controls.Basic

AbstractButton {
    id: chevron

    property bool expanded: false
    implicitWidth: Theme.controlHeight
    implicitHeight: Theme.controlHeight
    hoverEnabled: true
    Accessible.name: expanded ? "Collapse category" : "Expand category"

    background: Rectangle {
        color: chevron.down ? Theme.pressed : chevron.hovered ? Theme.hover : "transparent"
        border.color: chevron.visualFocus ? Theme.accent : "transparent"
    }

    contentItem: Image {
        source: "qrc:/assets/chevron.svg"
        sourceSize.width: 20
        sourceSize.height: 20
        fillMode: Image.Pad
        rotation: chevron.expanded ? 0 : -90

        Behavior on rotation {
            NumberAnimation {
                duration: Theme.animationsEnabled ? Theme.timings.controls : 0
            }
        }
    }

    ToolTip.visible: hovered
    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout
    ToolTip.text: expanded ? "Hide the contents of this category." : "Show the contents of this category."
}
