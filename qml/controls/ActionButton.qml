import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control

    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout
    ToolTip.visible: (hovered || visualFocus) && ToolTip.text.length > 0
    property bool primary: false
    property bool compact: false
    hoverEnabled: true
    implicitHeight: compact ? Theme.compactHeight : Theme.controlHeight
    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    leftPadding: compact ? Theme.spaceS : Theme.spaceM
    rightPadding: leftPadding

    background: Rectangle {
        // Keep disabled labels opaque; fading dark text on an accent fill loses contrast.
        color: !control.enabled ? Theme.inset : control.primary
            ? (control.down ? Theme.brand : control.hovered ? Theme.accentHover : Theme.accent)
            : (control.down ? Theme.pressed : control.hovered ? Theme.hover : Theme.button)
        border.color: !control.enabled ? Theme.separator
            : control.visualFocus ? Theme.accent : control.primary ? "transparent" : Theme.outline
    }

    contentItem: Text {
        text: control.text
        color: !control.enabled ? Theme.muted : control.primary ? Theme.accentText : Theme.ink
        font.pixelSize: control.compact ? Theme.fontCaption : Theme.fontBody
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
