import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: permission

    property alias text: toggle.text
    property alias checked: toggle.checked
    property string description: ""
    property string help: description
    property int access: PermissionBadge.Controls
    property string badgeText: "Home Assistant Can Run"
    property bool effective: checked && enabled
    property string blockedReason: "Enable the required module and permissions to allow this access."
    signal toggled(bool allowed)
    Layout.fillWidth: true
    spacing: Theme.spaceXs

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceM

        OptionCheckBox {
            id: toggle
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: Math.max(32, contentItem.implicitHeight)
            ToolTip.text: permission.help
            onClicked: permission.toggled(checked)
            contentItem: Text {
                text: toggle.text
                textFormat: Text.PlainText
                leftPadding: 30
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                color: toggle.enabled ? Theme.ink : Theme.muted
                font.pixelSize: Theme.fontBody
            }
        }
        PermissionBadge {
            objectName: permission.objectName + "Badge"
            access: permission.effective ? permission.access : PermissionBadge.Blocked
            text: permission.effective ? permission.badgeText : permission.checked ? "Blocked" : "Not Allowed"
            help: permission.checked && !permission.effective ? permission.blockedReason : permission.help
        }
    }

    HintLabel {
        visible: permission.description.length > 0
        Layout.fillWidth: true
        Layout.leftMargin: 28
        text: permission.description
        color: permission.access === PermissionBadge.Shell ? Theme.error : Theme.muted
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
    }
}
