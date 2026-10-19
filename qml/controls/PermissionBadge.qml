import QtQuick
import QtQuick.Controls.Basic

// Describes access without changing it. Text still identifies the permission;
// amber marks cloud exposure or control, while red marks supplied shell commands.
Control {
    id: badge

    enum Access { Blocked, Readings, Controls, Shell, Cloud }
    property int access: PermissionBadge.Blocked
    property string text: "Not Allowed"
    property string help: ""
    property bool compact: false

    readonly property color foreground: access === PermissionBadge.Shell ? Theme.error
        : access === PermissionBadge.Blocked ? Theme.muted
        : access === PermissionBadge.Readings ? Theme.permissionReading : Theme.warning
    readonly property color fill: access === PermissionBadge.Shell ? Theme.errorSurface
        : access === PermissionBadge.Controls || access === PermissionBadge.Cloud ? Theme.permissionControl
        : access === PermissionBadge.Readings ? Theme.permissionRead : Theme.surface
    readonly property color outline: access === PermissionBadge.Shell ? Theme.errorOutline
        : access === PermissionBadge.Controls || access === PermissionBadge.Cloud ? Theme.warningOutline
        : access === PermissionBadge.Readings ? Theme.outline : "transparent"

    implicitWidth: label.implicitWidth + leftPadding + rightPadding
    implicitHeight: compact ? 18 : 24
    leftPadding: compact ? Theme.spaceXs : Theme.spaceS
    rightPadding: leftPadding
    hoverEnabled: true
    activeFocusOnTab: false
    Accessible.role: Accessible.StaticText
    Accessible.name: text
    Accessible.description: help
    ToolTip.text: help
    ToolTip.visible: hovered && help.length > 0
    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout

    contentItem: Text {
        id: label
        text: badge.text
        textFormat: Text.PlainText
        color: badge.foreground
        font.pixelSize: badge.compact ? Theme.fontBadge : Theme.fontSmall
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        color: badge.fill
        border.color: badge.outline
    }
}
