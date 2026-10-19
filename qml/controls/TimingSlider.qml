import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: timing

    required property var input
    required property real minimum
    required property real maximum
    property real increment: 100
    property string suffix: "ms"
    property string label: "Interval"
    Layout.fillWidth: true
    Layout.minimumWidth: 160
    spacing: 0

    ValueSlider {
        id: track

        objectName: timing.input.objectName + "Slider"
        Layout.fillWidth: true
        hoverEnabled: true
        from: timing.minimum
        to: timing.maximum
        stepSize: timing.increment
        value: Math.max(from, Math.min(to, timing.input.value))
        ToolTip.text: timing.label + ": " + timing.input.value + " " + timing.suffix + ". Drag for the suggested range, or type an exact value in the number field."
        ToolTip.visible: hovered
        ToolTip.delay: Theme.toolTipDelay
        ToolTip.timeout: Theme.toolTipTimeout
        Accessible.name: timing.label

        onMoved: {
            timing.input.value = Math.round(value);
            timing.input.valueModified();
        }
    }

    HintLabel {
        text: (timing.input.value < timing.minimum
            || timing.input.value > timing.maximum
            ? "Custom value · "
            : "") + timing.minimum + "–" + timing.maximum + " " + timing.suffix + " slider"

        font.pixelSize: Theme.fontLabel
        color: track.hovered ? Theme.ink : Theme.muted
    }
}
