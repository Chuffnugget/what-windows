import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Button {
    id: button

    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout
    ToolTip.visible: hovered && ToolTip.text.length > 0
    property string glyph: ""
    property bool selected: false
    property bool iconOnly: false
    property real textExpansion: iconOnly ? 0 : 1
    property real expandedWidth: width
    property real collapsedWidth: width
    // Keep the endpoint exact so the temporary shader layer is released.
    readonly property real textReveal: textExpansion >= 1 ? 1 : Math.max(0, (textExpansion - 0.55) / 0.45)
    Accessible.name: text
    Layout.fillWidth: true
    implicitHeight: 44
    hoverEnabled: true

    background: Rectangle {
        color: button.selected ? Theme.selected : button.hovered ? Theme.hover : "transparent"
        border.color: button.visualFocus ? Theme.accent : "transparent"

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationsEnabled ? Theme.timings.hover : 0
            }
        }
    }

    contentItem: Item {
        clip: true

        TextMetrics {
            id: glyphMetrics
            font: navigationGlyph.font
            text: navigationGlyph.text
        }

        FontMetrics {
            id: glyphFont
            font: navigationGlyph.font
        }

        Label {
            id: navigationGlyph
            objectName: "navigationIcon"
            // Interpolate between the two endpoints. Using the animated parent
            // width here makes the icon overshoot sideways halfway through.
            x: Math.max(0, (button.collapsedWidth - button.leftPadding - button.rightPadding - width) / 2)
                * (1 - button.textExpansion)
            width: 22
            height: parent.height
            text: button.glyph
            color: button.selected ? Theme.accent : Theme.muted
            font.family: "Segoe UI Symbol"
            font.pixelSize: 18
            // Centre the ink on both axes. Font bearings and baseline spacing
            // differ between these symbols, even when their text boxes align.
            leftPadding: (width - glyphMetrics.tightBoundingRect.width) / 2
                - glyphMetrics.tightBoundingRect.x
            topPadding: (height - glyphMetrics.tightBoundingRect.height) / 2
                - glyphFont.ascent - glyphMetrics.tightBoundingRect.y
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignTop
        }

        Label {
            id: title
            objectName: "navigationLabel"
            property ExpansionBlur textTransition: ExpansionBlur {
                target: title
                progress: button.textReveal
            }
            x: navigationGlyph.x + navigationGlyph.width + Theme.spaceM
            // Clip a stable text layout instead of re-eliding it every frame.
            width: Math.max(0, button.expandedWidth - button.leftPadding - button.rightPadding - navigationGlyph.width - Theme.spaceM)
            height: parent.height
            opacity: Math.min(1, button.textReveal * 2)
            visible: button.textReveal > 0
            text: button.text
            color: button.selected ? Theme.ink : Theme.muted
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
    }
}
