import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ModuleCard {
    id: wallpaperModule

    signal controlsRequested
    moduleKey: "wallpaper"
    heading: "Wallpaper Engine"
    summary: (controller.wallpaper.enabled && controller.wallpaper.status) || "Wallpapers, playlists and profiles"
    keywords: "wallpaper engine desktop background playlist profile pause mute steam properties"

    function save(path) {
        controller.saveWallpaper(controller.wallpaper.enabled, wallpaperRemote.checked, wallpaperShare.checked, wallpaperCloud.checked, path === undefined
            ? (controller.wallpaper.override || "")
            : path);
    }

    function syncExecutable() {
        if (!wallpaperPath.activeFocus && !wallpaperPath.edited)
            wallpaperPath.text = controller.wallpaper.override || controller.wallpaper.executable || "";
    }

    Connections {
        target: controller

        function onChanged() {
            wallpaperModule.syncExecutable();
        }
    }

    HintLabel {
        text: controller.wallpaper.status || "Enable to discover Wallpaper Engine"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    HintLabel {
        text: "Wallpaper Engine must already be running. Automatic discovery checks running processes and Steam libraries."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    RowLayout {
        Layout.fillWidth: true

        InputField {
            id: wallpaperPath

            objectName: "wallpaperExecutablePath"
            Layout.fillWidth: true
            placeholderText: "Detecting Wallpaper Engine…"
            property bool edited: false
            Component.onCompleted: wallpaperModule.syncExecutable()
            onTextEdited: edited = true

            onEditingFinished: {
                if (edited) {
                    edited = false;
                    wallpaperModule.save(text);
                }
            }

            onActiveFocusChanged: if (!activeFocus && !edited)
                wallpaperModule.syncExecutable()
        }

        QuietButton {
            ToolTip.text: "Select the Wallpaper Engine executable when automatic discovery cannot find it."
            text: "Browse…"

            onClicked: {
                let path = controller.browseWallpaper(true);
                if (path) {
                    wallpaperPath.edited = false;
                    wallpaperPath.text = path;
                    wallpaperModule.save(path);
                }
            }
        }

        QuietButton {
            ToolTip.text: "Clear the custom executable path and search for Wallpaper Engine."
            text: "Auto-Detect"

            onClicked: {
                wallpaperPath.edited = false;
                wallpaperPath.focus = false;
                wallpaperModule.save("");
            }
        }
    }

    HintLabel {
        objectName: "wallpaperDetectionNotice"
        text: controller.wallpaper.busy
            ? "Checking Wallpaper Engine…"
            : controller.wallpaper.installed
            ? (controller.wallpaper.override ? "✓ Selected executable verified" : "✓ Automatically detected · executable verified")
            : "Executable not found - browse to wallpaper32.exe or wallpaper64.exe"
        color: controller.wallpaper.installed ? Theme.accent : Theme.muted
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    PermissionOption {
        help: "Allow paired Home Assistant clients to operate this module. Local controls can be used independently."
        id: wallpaperRemote

        text: "Allow Home Assistant Control"
        checked: controller.wallpaper.control_enabled === true
        onToggled: wallpaperModule.save()
    }

    PermissionOption {
        help: "Permit Wallpaper Engine commands through cloud connections. Home Assistant control must also be enabled."
        id: wallpaperCloud

        text: "Allow Wallpaper Engine over Cloud Connections"
        checked: controller.wallpaper.cloud === true
        badgeText: "Cloud Home Assistant"
        effective: checked && enabled && wallpaperRemote.checked
        blockedReason: "Wallpaper Engine must be enabled and allow Home Assistant control before Home Assistant can run cloud commands."
        onToggled: wallpaperModule.save()
    }

    PermissionOption {
        help: "Include the current wallpaper path in shared state. Paths can reveal folder or file names from this PC."
        id: wallpaperShare

        text: "Share Current Wallpaper Path with Home Assistant"
        checked: controller.wallpaper.share_path === true
        access: PermissionBadge.Readings
        badgeText: "Home Assistant Can Read"
        onToggled: wallpaperModule.save()
    }

    QuietButton {
        ToolTip.text: "Open Wallpaper Engine actions in the sensor browser to change the wallpaper or playback locally."
        text: "Open Controls in Sensor Browser"

        onClicked: {
            controller.setBrowserFilters("", "wallpaper", false, false, false);
            wallpaperModule.controlsRequested();
        }
    }
}
