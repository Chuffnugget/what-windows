import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../modules/ModuleStatus.js" as ModuleStatus

GlassDialog {
    id: wallpaperControls

    required property var controller

    objectName: "wallpaperControlsDialog"
    title: "Wallpaper Engine Controls"
    anchors.centerIn: parent
    width: Math.min(720, wallpaperControls.parent.width - 40)
    height: Math.min(650, wallpaperControls.parent.height - 60)
    standardButtons: Dialog.Close

    DragScrollView {
        id: wallpaperControlsScroll

        anchors.fill: parent
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        rightPadding: 22

        ScrollBar.vertical: WideScrollBar {
            parent: wallpaperControlsScroll
            x: wallpaperControlsScroll.width - width
            height: wallpaperControlsScroll.availableHeight
        }

        ColumnLayout {
            width: wallpaperControlsScroll.availableWidth
            spacing: Theme.spaceM

            HintLabel {
                text: ModuleStatus.connectionText(wallpaperControls.controller, "wallpaper") + " · " + (wallpaperControls.controller.wallpaper.status
                    || "")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            HintLabel {
                text: wallpaperControls.controller.wallpaper.current_wallpaper || "Current wallpaper unavailable"
                Layout.fillWidth: true
                elide: Text.ElideMiddle
            }

            RowLayout {
                Repeater {
                    model: [
                        {
                            name: "Play",
                            op: "play"
                        },

                        {
                            name: "Pause",
                            op: "pause"
                        },

                        {
                            name: "Stop",
                            op: "stop"
                        },

                        {
                            name: "Mute",
                            op: "mute"
                        },

                        {
                            name: "Unmute",
                            op: "unmute"
                        }
                    ]

                    QuietButton {
                        required property var modelData
                        text: modelData.name
                        enabled: wallpaperControls.controller.wallpaper.running === true && !wallpaperControls.controller.wallpaper.busy
                        onClicked: wallpaperControls.controller.wallpaperCommand({
                            operation: modelData.op
                        })
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.separator
            }

            RowLayout {
                Layout.fillWidth: true

                ChoiceBox {
                    id: wallpaperOperation

                    ToolTip.text: "Choose the action to send to Wallpaper Engine when you click Apply."
                    hoverEnabled: true
                    ToolTip.visible: hovered
                    ToolTip.delay: Theme.toolTipDelay
                    ToolTip.timeout: Theme.toolTipTimeout
                    Layout.fillWidth: true
                    textRole: "name"
                    valueRole: "op"
                    model: [
                        {
                            name: "Open wallpaper",
                            op: "openWallpaper"
                        },

                        {
                            name: "Open playlist",
                            op: "openPlaylist"
                        },

                        {
                            name: "Open display profile",
                            op: "openProfile"
                        },

                        {
                            name: "Next wallpaper",
                            op: "nextWallpaper"
                        },

                        {
                            name: "Remove wallpaper",
                            op: "closeWallpaper"
                        },

                        {
                            name: "Apply wallpaper properties",
                            op: "applyProperties"
                        },

                        {
                            name: "Show desktop icons",
                            op: "showIcons"
                        },

                        {
                            name: "Hide desktop icons",
                            op: "hideIcons"
                        },

                        {
                            name: "Refresh current wallpaper",
                            op: "getWallpaper"
                        }
                    ]
                }

                QuietButton {
                    text: "Refresh Status"
                    enabled: wallpaperControls.controller.wallpaper.enabled === true && !wallpaperControls.controller.wallpaper.busy
                    onClicked: wallpaperControls.controller.refreshWallpaper()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: ["openWallpaper",
                    "openPlaylist",
                    "openProfile"].indexOf(wallpaperOperation.currentValue) >= 0

                InputField {
                    id: wallpaperValue

                    Layout.fillWidth: true
                    placeholderText: wallpaperOperation.currentValue === "openWallpaper"
                        ? "Wallpaper file on this PC"
                        : "Saved name in Wallpaper Engine"
                }

                QuietButton {
                    ToolTip.text: "Choose the wallpaper file to open with Wallpaper Engine."
                    text: "Browse…"
                    visible: wallpaperOperation.currentValue === "openWallpaper"

                    onClicked: {
                        let path = wallpaperControls.controller.browseWallpaper(false);
                        if (path)
                            wallpaperValue.text = path;
                    }
                }
            }

            RowLayout {
                visible: ["openWallpaper",
                    "openPlaylist",
                    "nextWallpaper",
                    "closeWallpaper",
                    "applyProperties",
                    "getWallpaper"].indexOf(wallpaperOperation.currentValue) >= 0

                HintLabel {
                    text: "Apply to"
                }

                ChoiceBox {
                    id: wallpaperMonitor

                    objectName: "wallpaperMonitorChoice"
                    Layout.preferredWidth: 250
                    textRole: "name"
                    valueRole: "monitor"
                    model: {
                        let all = ["nextWallpaper",
                            "closeWallpaper",
                            "applyProperties"].indexOf(wallpaperOperation.currentValue) >= 0;
                        let choices = [
                            {
                                name: all ? "All monitors" : "Default monitor",
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
                }

                HintLabel {
                    text: "Monitor order follows Wallpaper Engine."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }

            OptionCheckBox {
                id: wallpaperAdvanced

                text: "Advanced Options"
            }

            HintLabel {
                visible: wallpaperAdvanced.checked
                text: "Advanced options · JSON object for location, preset, pop-out window settings or properties from Wallpaper Engine’s Share JSON. Leave {} for defaults."
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            TextArea {
                id: wallpaperOptions

                visible: wallpaperAdvanced.checked
                Layout.fillWidth: true
                implicitHeight: 90
                text: "{}"
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                color: Theme.ink

                background: Rectangle {
                    color: Theme.inset

                    border.color: Theme.outline
                }
            }

            HintLabel {
                id: wallpaperError

                visible: text.length > 0
                color: Theme.warning
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            QuietButton {
                ToolTip.text: "Send the selected action and options to Wallpaper Engine on this PC."
                text: "Apply"
                enabled: wallpaperControls.controller.wallpaper.running === true && !wallpaperControls.controller.wallpaper.busy

                onClicked: {
                    try {
                        let options = JSON.parse(wallpaperOptions.text);
                        if (!options || Array.isArray(options) || typeof options !== "object")
                            throw new Error("Enter a JSON object");
                        options.operation = wallpaperOperation.currentValue;
                        if (options.operation === "openWallpaper")
                            options.file = wallpaperValue.text;
                        if (options.operation === "openPlaylist")
                            options.playlist = wallpaperValue.text;
                        if (options.operation === "openProfile")
                            options.profile = wallpaperValue.text;
                        if (wallpaperMonitor.visible && wallpaperMonitor.currentValue >= 0)
                            options.monitor = wallpaperMonitor.currentValue;
                        wallpaperError.text = "";
                        wallpaperControls.controller.wallpaperCommand(options);
                    } catch (error) {
                        wallpaperError.text = error.message;
                    }
                }
            }

            HintLabel {
                text: "Commands are dispatched once. Wallpaper Engine cannot confirm pause/mute state through its CLI."
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }
    }
}
