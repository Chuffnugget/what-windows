import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: editor

    objectName: "commandEditor"
    required property var manager
    required property var definition
    property bool saving: true
    property string error: ""
    spacing: Theme.spaceM

    function draftValue() {
        const value = JSON.parse(JSON.stringify(definition));
        value.name = saving ? entryName.text.trim() : "One-Off Command";
        value.description = description.text.trim();
        value.enabled = enabledBox.checked;
        value.ha_enabled = saving && haPermission.checked;
        value.cloud_enabled = value.ha_enabled && cloudPermission.checked;
        value.shell = shellEditor.shell;
        value.command = shellEditor.command;
        value.timeout = timeout.value;
        value.leave_running = leaveRunning.enabled && leaveRunning.checked;
        return value;
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: editor.saving
              ? "A saved command does something when you press Run or Home Assistant triggers it. Its command text is defined on this PC." :
                "Run a command once on this PC without saving it or allowing Home Assistant to run it."
    }

    Label {
        visible: editor.saving
        text: "Name"
        color: Theme.ink
    }
    InputField {
        id: entryName
        objectName: "commandName"
        visible: editor.saving
        Layout.fillWidth: true
        text: editor.definition.name
        placeholderText: "For example, Run Backup"
        maximumLength: 100
    }

    Label {
        visible: editor.saving
        text: "What Does It Do? (Optional)"
        color: Theme.ink
    }
    InputField {
        id: description
        visible: editor.saving
        Layout.fillWidth: true
        text: editor.definition.description || ""
        placeholderText: "A short explanation for you; this stays on the PC"
        maximumLength: 500
    }

    ShellEditor {
        id: shellEditor
        definition: editor.definition
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: "Run and Test executes the real command as your Windows user. It can access files, change settings or start programs."
    }

    OptionCheckBox {
        id: enabledBox
        visible: editor.saving
        text: "Command Enabled"
        checked: editor.definition.enabled
    }

    PermissionOption {
        id: haPermission
        objectName: "commandHaPermission"
        visible: editor.saving
        text: "Allow Home Assistant to Run This Saved Command"
        checked: editor.definition.ha_enabled === true
        effective: checked && enabledBox.checked && editor.manager.enabled && editor.manager.controlEnabled
        blockedReason: "Enable this command, the Custom module and its saved-command permission to allow Home Assistant execution."
        description:
            "Home Assistant can request this saved command by its ID. The command API cannot create or edit its definition. A compromised Home Assistant instance could still trigger it."
    }

    PermissionOption {
        id: cloudPermission
        objectName: "commandCloudPermission"
        visible: editor.saving && haPermission.checked
        text: "Also Allow Cloud Connections"
        checked: editor.definition.cloud_enabled === true
        badgeText: "Cloud Home Assistant"
        effective: checked && haPermission.effective && editor.manager.cloudEnabled
        blockedReason: "This command needs Home Assistant execution permission and the Custom module's cloud permission."
        description:
            "Allows this command through cloud transports as well as local connections. The Custom module's cloud permission must also be enabled."
    }

    HintLabel {
        visible: editor.saving && haPermission.checked && (!editor.manager.enabled ||
                                                           !editor.manager.controlEnabled)
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.warning
        text: "Home Assistant execution is currently blocked by the Custom module or its saved-command permission. Enable the module in Modules > Custom and the permission here on the Commands page."
    }

    SettingsSection {
        title: "Advanced"
        detail: "Execution timeout and launched applications"

        OptionCheckBox {
            id: leaveRunning
            objectName: "commandLeaveRunning"
            text: "Leave Launched Apps Running"
            checked: editor.definition.leave_running === true
            enabled: shellEditor.shell !== "wsl"
            ToolTip.text: "Keep child applications open after the shell exits successfully. Failed, cancelled or timed-out commands still stop their children. This option is not available for WSL."
        }

        RowLayout {
            HintLabel {
                text: "Timeout (Seconds)"
            }
            NumberInput {
                id: timeout
                from: 1
                to: 60
                value: editor.definition.timeout
            }
        }
        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "The shell must finish within the timeout. By default its child applications also stop. Leave Launched Apps Running keeps them open after a successful exit; WHAT then stops monitoring them and cannot cancel them later. Launch apps with Start-Process in PowerShell or start \"\" \"app.exe\" in CMD so the shell can finish; a directly invoked program may keep it waiting until the timeout. A timeout, cancellation, failed command or permission revocation before completion still stops the command and its children."
        }
    }

    EditorResult {
        form: editor
        manager: editor.manager
        error: editor.error
    }
}
