import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: permissions

    required property var controller
    property bool showSaved: true
    property bool showShell: true
    Layout.fillWidth: true
    spacing: Theme.spaceM

    PermissionOption {
        objectName: "savedCommandPermission"
        visible: permissions.showSaved
        text: "Allow Home Assistant to Run Named Commands Saved in WHAT"
        checked: permissions.controller.customSources.controlEnabled
        badgeText: "Saved Commands"
        effective: checked && permissions.controller.customSources.enabled
        blockedReason: "The Custom module is disabled. Each command also needs its own Home Assistant permission."
        onToggled: allowed => permissions.controller.customSources.controlEnabled = allowed
        description:
            "Home Assistant can trigger commands you created on this PC and individually allowed. The Custom module must also be enabled. The command API cannot create or edit them. A compromised Home Assistant instance could still run those commands."
    }

    PermissionOption {
        objectName: "suppliedCommandPermission"
        visible: permissions.showShell
        enabled: permissions.controller.info.windowsEnabled
        text: "Allow Home Assistant to Run Commands Supplied in YAML"
        checked: permissions.controller.info.controlPermissions.shell === true
        access: PermissionBadge.Shell
        badgeText: "Home Assistant Shell Access"
        blockedReason: "The Windows module is disabled. Home Assistant-supplied shell commands cannot run."
        onToggled: allowed => permissions.controller.setControlPermission("shell", allowed)
        description:
            "High Risk: Home Assistant can send arbitrary PowerShell, CMD or WSL commands directly to this PC from YAML, scripts, automations or its UI. If Home Assistant is compromised, an attacker can run any command with your Windows user permissions, including reading or deleting files, running programs and changing settings. Leave this off unless you need it. Named commands saved in WHAT do not need this permission."
    }

    HintLabel {
        visible: permissions.showShell && !permissions.controller.info.windowsEnabled
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: "The Windows module is off. Enable it in Modules before allowing Home Assistant-supplied shell commands."
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: "Built-in controls such as volume, power and display changes keep their own module permissions. Receiving readings does not grant permission to use these controls."
    }
}
