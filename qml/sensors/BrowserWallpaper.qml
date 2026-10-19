import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: wallpaper

    objectName: "inlineWallpaperControls"
    required property var controller
    spacing: 6
    readonly property bool ready: controller.wallpaper.running === true && !controller.wallpaper.busy

    Flow {
        Layout.fillWidth: true
        spacing: 5

        Repeater {
            model: ["Play", "Pause", "Stop", "Mute", "Unmute"]

            BrowserButton {
                required property string modelData
                text: modelData
                enabled: wallpaper.ready
                onClicked: wallpaper.controller.wallpaperCommand({
                    operation: modelData.toLowerCase()
                })
            }
        }

        BrowserButton {
            text: "Refresh Status"
            enabled: !wallpaper.controller.wallpaper.busy
            onClicked: wallpaper.controller.refreshWallpaper()
        }
    }

    RowLayout {
        Layout.fillWidth: true

        BrowserChoice {
            id: operation

            Layout.fillWidth: true
            textRole: "name"
            valueRole: "op"
            model: [
                {
                    name: "Open Wallpaper",
                    op: "openWallpaper"
                },

                {
                    name: "Open Playlist",
                    op: "openPlaylist"
                },

                {
                    name: "Open Display Profile",
                    op: "openProfile"
                },

                {
                    name: "Next Wallpaper",
                    op: "nextWallpaper"
                },

                {
                    name: "Remove Wallpaper",
                    op: "closeWallpaper"
                },

                {
                    name: "Apply Properties",
                    op: "applyProperties"
                },

                {
                    name: "Show Desktop Icons",
                    op: "showIcons"
                },

                {
                    name: "Hide Desktop Icons",
                    op: "hideIcons"
                },

                {
                    name: "Read Current Wallpaper",
                    op: "getWallpaper"
                }
            ]
        }

        BrowserChoice {
            id: monitor

            implicitWidth: 150
            visible: ["openWallpaper",
                "openPlaylist",
                "nextWallpaper",
                "closeWallpaper",
                "applyProperties",
                "getWallpaper"].indexOf(operation.currentValue) >= 0
            textRole: "name"
            valueRole: "monitor"
            model: {
                const all = ["nextWallpaper", "closeWallpaper", "applyProperties"].indexOf(operation.currentValue) >= 0;
                const choices = [
                    {
                        name: all ? "All Monitors" : "Default Monitor",
                        monitor: -1
                    }
                ];
                for (let i = 0; i < Qt.application.screens.length; ++i)
                    choices.push({
                        name: "Monitor " + (i + 1),
                        monitor: i
                    });
                return choices;
            }

            ToolTip.text: "Monitor order follows Wallpaper Engine."
        }

        BrowserButton {
            text: "Apply"
            selected: true
            enabled: wallpaper.ready

            onClicked: {
                try {
                    let args = JSON.parse(options.text);
                    if (!args || Array.isArray(args) || typeof args !== "object")
                        throw new Error("Advanced options must be a JSON object");
                    args.operation = operation.currentValue;
                    if (args.operation === "openWallpaper")
                        args.file = input.text;
                    if (args.operation === "openPlaylist")
                        args.playlist = input.text;
                    if (args.operation === "openProfile")
                        args.profile = input.text;
                    if (monitor.visible && monitor.currentValue >= 0)
                        args.monitor = monitor.currentValue;
                    result.text = "";
                    wallpaper.controller.wallpaperCommand(args);
                } catch (error) {
                    result.text = error.message;
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: ["openWallpaper", "openPlaylist", "openProfile"].indexOf(operation.currentValue) >= 0

        InputField {
            compact: true
            id: input

            Layout.fillWidth: true
                placeholderText: operation.currentValue === "openWallpaper"
                ? "Wallpaper file on this PC"
                : "Saved name in Wallpaper Engine"
            placeholderTextColor: Theme.muted
            color: Theme.ink
            font.pixelSize: Theme.fontCaption
            selectByMouse: true

        }

        BrowserButton {
            text: "Browse…"
            visible: operation.currentValue === "openWallpaper"

            onClicked: {
                const path = wallpaper.controller.browseWallpaper(false);
                if (path)
                    input.text = path;
            }
        }
    }

    BrowserButton {
        id: advanced

        text: "Advanced Options"
        checkable: true
        help: "Extra Wallpaper Engine options as a JSON object."
    }

    TextArea {
        id: options

        visible: advanced.checked
        Layout.fillWidth: true
        implicitHeight: 70
        text: "{}"
        color: Theme.ink
        font.pixelSize: Theme.fontCaption
        selectByMouse: true
        wrapMode: TextEdit.Wrap

        background: Rectangle {
            color: Theme.field

            border.color: Theme.outline
        }
    }

    Text {
        id: result

        visible: text.length > 0
        color: Theme.warning
        font.pixelSize: Theme.fontCaption
        Layout.fillWidth: true
        wrapMode: Text.Wrap
    }

    Text {
        text: wallpaper.controller.wallpaper.status || ""
        color: Theme.muted
        font.pixelSize: Theme.fontSmall
        Layout.fillWidth: true
        wrapMode: Text.Wrap
    }
}
