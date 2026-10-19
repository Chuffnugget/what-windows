import QtQuick
import QtQuick.Controls.Basic

SpinBox {
    id: number

    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout
    ToolTip.visible: (hovered || visualFocus) && ToolTip.text.length > 0
    hoverEnabled: true
    editable: true
    implicitWidth: 150
    implicitHeight: Theme.controlHeight
    font.pixelSize: Theme.fontBody
    palette.text: enabled ? Theme.ink : Theme.muted
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.accentText
    property string unit: "s"
    textFromValue: function (value, locale) {
        return String(value) + (number.unit ? " " + number.unit : "");
    }

    valueFromText: function (text, locale) {
        return parseInt(text, 10);
    }

    validator: RegularExpressionValidator {
        regularExpression: new RegExp("[0-9]{1,5}" + (number.unit ? "(\\s*" + number.unit + ")?" : ""))
    }

    background: Rectangle {
        color: Theme.field
        border.color: number.visualFocus ? Theme.accent : Theme.outline
    }

    up.indicator: Rectangle {
        x: number.width - width - Theme.spaceXs
        y: Theme.spaceXs
        width: 32
        height: number.height - Theme.spaceS

        color: number.up.hovered ? Theme.hover : Theme.button

        Label {
            anchors.centerIn: parent
            text: "+"
            color: number.enabled ? Theme.ink : Theme.muted
            font.pixelSize: Theme.fontHeading
        }
    }

    down.indicator: Rectangle {
        x: Theme.spaceXs
        y: Theme.spaceXs
        width: 32
        height: number.height - Theme.spaceS

        color: number.down.hovered ? Theme.hover : Theme.button

        Label {
            anchors.centerIn: parent
            text: "−"
            color: number.enabled ? Theme.ink : Theme.muted
            font.pixelSize: Theme.fontHeading
        }
    }
}
