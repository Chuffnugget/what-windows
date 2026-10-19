import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ModuleCard {
    id: beacnModuleCard

    moduleKey: "beacn"
    heading: "BEACN"
    // The status pill already says "Disabled", so only show live status while enabled.
    summary: ((controller.beacn.enabled && controller.beacn.status) || "Audio mixing, devices, profiles and app controls").split("\n")[0]
    keywords: "beacn mixer audio volume mute audience personal profile theme brightness default devices"
    readonly property var installation: controller.beacn.installation || ({})

    function syncPaths() {
        if (!beacnExe.activeFocus && !beacnExe.edited)
            beacnExe.text = controller.beacn.exe_override || controller.beacn.exe_path || "";
        if (!beacnDll.activeFocus && !beacnDll.edited)
            beacnDll.text = controller.beacn.dll_override || controller.beacn.dll_path || "";
    }

    Connections {
        target: controller

        function onChanged() {
            beacnModuleCard.syncPaths();
        }
    }

    HintLabel {
        text: "Keep the official BEACN app running. Readings and controls are in Sensors; remote controls appear in Home Assistant when allowed below."
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    HintLabel {
        objectName: "beacnVersions"
        text: "BEACN App: " + (beacnModuleCard.installation.appVersion || "Not detected")
            + " · Driver: " + (beacnModuleCard.installation.driverVersion || "Not detected")
            + " · Driver API: " + (controller.beacn.api_version || "Not connected")
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    HintLabel {
        text: "BEACN Application"
    }

    RowLayout {
        Layout.fillWidth: true

        InputField {
            id: beacnExe

            objectName: "beacnExecutablePath"
            Layout.fillWidth: true
            placeholderText: "Automatically discover BEACN.exe"
            property bool edited: false
            Component.onCompleted: beacnModuleCard.syncPaths()
            onTextEdited: edited = true
            onEditingFinished: if (edited) {
                edited = false;
                controller.saveBeacnPaths(text, controller.beacn.dll_override || "");
            }

            onActiveFocusChanged: if (!activeFocus && !edited)
                beacnModuleCard.syncPaths()
        }

        QuietButton {
            ToolTip.text: "Select the installed BEACN application executable. Leave the path empty to use automatic discovery."
            text: "Browse…"
            onClicked: controller.browseBeacn(true)
        }
    }

    HintLabel {
        text: "BEACN Driver API"
    }

    RowLayout {
        Layout.fillWidth: true

        InputField {
            id: beacnDll

            objectName: "beacnDriverPath"
            Layout.fillWidth: true
            placeholderText: "Automatically discover BEACNUsbAudioapi_x64.dll"
            property bool edited: false
            Component.onCompleted: beacnModuleCard.syncPaths()
            onTextEdited: edited = true
            onEditingFinished: if (edited) {
                edited = false;
                controller.saveBeacnPaths(controller.beacn.exe_override || "", text);
            }

            onActiveFocusChanged: if (!activeFocus && !edited)
                beacnModuleCard.syncPaths()
        }

        QuietButton {
            ToolTip.text: "Select the installed BEACN 64-bit audio API DLL. Leave the path empty to use automatic discovery."
            text: "Browse…"
            onClicked: controller.browseBeacn(false)
        }
    }

    RowLayout {
        QuietButton {
            ToolTip.text: "Clear the custom paths and search for the installed BEACN app and driver."
            text: "Auto-detect"

            onClicked: {
                beacnExe.edited = false;
                beacnDll.edited = false;
                beacnExe.focus = false;
                beacnDll.focus = false;
                controller.saveBeacnPaths("", "");
            }
        }

        QuietButton {
            ToolTip.text: "Disconnect and reconnect the BEACN bridge using the configured application and driver paths."
            text: "Reconnect"
            onClicked: controller.reconnectBeacn()
        }
    }

    HintLabel {
        objectName: "beacnVerification"
        text: controller.beacn.discovering
            ? "Discovering and verifying BEACN…"
            : (controller.beacn.installation || {}).status
            || controller.beacn.status
            || "Enable BEACN to discover its installation"
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    PermissionOption {
        help: "Allow paired Home Assistant clients to operate this module. Local controls can be used independently."
        text: "Allow Home Assistant Control"
        checked: controller.beacn.control_enabled === true
        onToggled: allowed => controller.saveBeacn(true, allowed, controller.beacn.cloud === true)
    }

    PermissionOption {
        help: "Permit BEACN commands through cloud connections. Home Assistant control must also be enabled."
        text: "Allow BEACN over Cloud Connections"
        checked: controller.beacn.cloud === true
        badgeText: "Cloud Home Assistant"
        access: PermissionBadge.Cloud
        effective: checked && enabled && controller.beacn.control_enabled === true
        blockedReason: "BEACN must be enabled and allow Home Assistant control before Home Assistant can run cloud commands."
        onToggled: allowed => controller.saveBeacn(true, controller.beacn.control_enabled === true, allowed)
    }

    HintLabel {
        text: controller.beacn.recording_status || ""
        visible: text.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    HintLabel {
        text: controller.beacn.log_path || ""
        visible: text.length > 0
        Layout.fillWidth: true
        elide: Text.ElideMiddle
    }
}
