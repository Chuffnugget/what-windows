import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: field

    property bool compact: false
    // TextField inherits TextInput, so it needs Control's keyboard-focus rule.
    readonly property bool visualFocus: activeFocus && (focusReason === Qt.TabFocusReason
        || focusReason === Qt.BacktabFocusReason || focusReason === Qt.ShortcutFocusReason)
    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout
    ToolTip.visible: (hovered || visualFocus) && ToolTip.text.length > 0
    hoverEnabled: true
    color: enabled ? Theme.ink : Theme.muted
    leftPadding: compact ? Theme.spaceS : Theme.spaceM
    rightPadding: leftPadding
    topPadding: Theme.spaceXs
    bottomPadding: topPadding
    implicitHeight: compact ? Theme.compactHeight : Theme.controlHeight
    font.pixelSize: compact ? Theme.fontCaption : Theme.fontBody
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentText
    selectByMouse: true
    placeholderTextColor: Theme.muted

    background: Rectangle {
        color: field.enabled ? Theme.field : Theme.inset
        border.color: field.visualFocus ? Theme.accent : Theme.outline
    }
}
