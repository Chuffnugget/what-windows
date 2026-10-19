import QtQuick
import QtQuick.Controls.Basic

Button {
    id: quiet

    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout
    property bool compact: false
    // Optional image shown instead of the text, such as the shared chevron.
    property url glyphSource: ""
    property real glyphRotation: 0
    implicitHeight: compact ? Theme.compactHeight : Theme.controlHeight
    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    horizontalPadding: compact ? Theme.spaceS : Theme.spaceM
    hoverEnabled: true
    ToolTip.visible: (hovered || visualFocus) && ToolTip.text.length > 0

    background: Rectangle {
        color: !quiet.enabled ? "transparent"
            : quiet.down ? Theme.pressed : quiet.hovered ? Theme.hover : quiet.checked ? Theme.selected : "transparent"
        border.color: quiet.visualFocus ? Theme.accent : "transparent"
    }

    contentItem: Item {
        readonly property bool hasGlyph: quiet.glyphSource.toString().length > 0

        implicitWidth: hasGlyph ? 18 : label.implicitWidth
        implicitHeight: label.implicitHeight

        Text {
            id: label

            anchors.fill: parent
            visible: !parent.hasGlyph
            text: quiet.text
            color: !quiet.enabled ? Theme.muted : quiet.checked || quiet.hovered ? Theme.accent : Theme.secondary
            font.pixelSize: quiet.compact ? Theme.fontCaption : Theme.fontBody
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        Image {
            anchors.centerIn: parent
            visible: parent.hasGlyph
            source: quiet.glyphSource
            width: Math.min(18, quiet.availableHeight)
            height: width
            sourceSize: Qt.size(18, 18)
            rotation: quiet.glyphRotation
            opacity: quiet.enabled ? 1 : 0.4
        }
    }
}
