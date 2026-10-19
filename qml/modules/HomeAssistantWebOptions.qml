import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: options

    required property var web
    property var draft: ({})
    property var saved: ({})
    Layout.fillWidth: true
    spacing: 0

    function update(key, value) {
        const next = Object.assign({}, draft);
        next[key] = value;
        draft = next;
    }

    function readSettings() {
        saved = Object.assign({}, web.options);
        draft = Object.assign({}, saved);
    }

    Connections {
        target: options.web
        function onConfigurationChanged() {
            // Follow toolbar changes without discarding edits awaiting Apply.
            const next = Object.assign({}, options.draft);
            const current = options.web.options;
            for (const key of Object.keys(current)) {
                if (next[key] === options.saved[key])
                    next[key] = current[key];
            }
            options.saved = Object.assign({}, current);
            options.draft = next;
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.bottomMargin: Theme.spaceM
        HintLabel {
            Layout.fillWidth: true
            text: "Picture-in-Picture Hotkey"
        }
        InputField {
            objectName: "hawebPipHotkey"
            Layout.preferredWidth: 230
            text: options.draft.pipHotkey || ""
            maximumLength: 80
            placeholderText: "Ctrl+Alt+P"
            ToolTip.text: "Show or hide picture-in-picture from any app, even with WHAT in the tray. Use a different combination from the overlay hotkey. Leave blank to disable."
            onTextEdited: options.update("pipHotkey", text.trim())
        }
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        visible: text.length > 0
        color: Theme.warning
        text: options.web.pipHotkeyError
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.bottomMargin: Theme.spaceM
        HintLabel {
            Layout.fillWidth: true
            text: "WHAT Tray Icon Click"
        }
        ChoiceBox {
            objectName: "hawebTrayAction"
            Layout.preferredWidth: 230
            readonly property var actions: ["what", "windowed", "pip", "fullscreen", "desktop"]
            model: ["Open WHAT", "Web Inside WHAT", "Toggle Picture-in-Picture", "Toggle Fullscreen", "Web on Desktop"]
            currentIndex: actions.indexOf(options.draft.trayAction || "what")
            ToolTip.text: "Choose what a left click on WHAT's main system tray icon does. A double click performs it once. Open WHAT remains in the right-click menu. Disabled or unconfigured Web falls back to Open WHAT. Apply to save."
            onActivated: options.update("trayAction", actions[currentIndex])
        }
    }

    SettingsSection {
        title: "Connection and Recovery"
        detail: "Optional cloud fallback and automatic retry"

        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "The dashboard address above is tried first. Cloud fallback is separate from WHAT's API and MQTT connections, and may need its own browser sign-in. Reload or Home tries the primary address again."
        }

        InputField {
            objectName: "hawebCloudUrl"
            Layout.fillWidth: true
            text: options.draft.cloudUrl || ""
            placeholderText: "https://your-instance.ui.nabu.casa/lovelace"
            maximumLength: 2048
            ToolTip.text: "Optional HTTPS fallback address. Used only if the primary server cannot be reached or returns a server error. Certificate and authentication failures never trigger fallback."
            onTextEdited: options.update("cloudUrl", text.trim())
        }

        OptionCheckBox {
            text: "Use Cloud When the Primary Address Is Unavailable"
            checked: options.draft.fallback === true
            ToolTip.text: "Try the saved cloud address after a network or server failure. WHAT does not transfer a login between different sites."
            onClicked: options.update("fallback", checked)
        }

        OptionCheckBox {
            text: "Recover Automatically"
            checked: options.draft.autoRecover === true
            ToolTip.text: "Stop a stalled navigation after 30 seconds. Retry temporary loading failures or a stopped browser after 2, 5 and 15 seconds, up to three attempts. One minute of stable browsing resets the retry allowance."
            onClicked: options.update("autoRecover", checked)
        }
    }

    SettingsSection {
        title: "Display and Audio"
        detail: "Monitors, toolbar, picture-in-picture and background playback"

        RowLayout {
            Layout.fillWidth: true
            HintLabel {
                Layout.fillWidth: true
                text: "PiP Window Size"
            }
            NumberInput {
                objectName: "hawebPipWindowScale"
                from: 50
                to: 200
                stepSize: 10
                unit: "%"
                value: options.draft.pipWindowScale || 100
                ToolTip.text: "100% is a 640 by 480 window. Changing this resets PiP to that percentage, adjusted for the selected aspect ratio and available screen space. You can still resize it by dragging. Dashboard zoom is separate. Apply to save."
                onValueModified: options.update("pipWindowScale", value)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            HintLabel {
                Layout.fillWidth: true
                text: "PiP Dashboard Zoom"
            }
            NumberInput {
                objectName: "hawebPipScale"
                from: 50
                to: 200
                stepSize: 10
                unit: "%"
                value: options.draft.pipScale || 100
                ToolTip.text: "Zoom only the dashboard inside PiP, without resizing the window or changing other Web views. You can also use the PiP title bar's minus and plus buttons. Apply to save."
                onValueModified: options.update("pipScale", value)
            }
        }

        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Drag the right edge of Home Assistant's desktop sidebar to resize it. Drag left to collapse it to icons, or use its own menu button. The saved width applies to Web, desktop and overlays. Narrow mobile layouts keep Home Assistant's slide-out menu."
        }

        Repeater {
            model: [{key: "desktopMonitor", label: "Desktop Monitor"},
                {key: "overlayMonitor", label: "Overlay Monitor"}]

            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true

                HintLabel {
                    Layout.fillWidth: true
                    text: modelData.label
                }

                ChoiceBox {
                    Layout.preferredWidth: 300
                    model: options.web.monitors
                    textRole: "label"
                    valueRole: "name"
                    currentIndex: Math.max(0, options.web.monitors.findIndex(item => item.name === options.draft[modelData.key]))
                    ToolTip.text: "Choose a monitor by name. Automatic uses the pointer's monitor for overlays and the last chosen desktop monitor. If a saved monitor is disconnected, another available monitor is used."
                    onActivated: options.update(modelData.key, currentValue)
                }
            }
        }

        OptionCheckBox {
            objectName: "hawebAutoHide"
            text: "Auto-Hide the Browser Toolbar"
            checked: options.draft.autoHideToolbar === true
            ToolTip.text: "Hide the toolbar after 2.5 seconds. Move the pointer to the top edge of the dashboard or press F6 to reveal it. Keyboard focus keeps it open."
            onClicked: options.update("autoHideToolbar", checked)
        }

        OptionCheckBox {
            text: "Snap Picture-in-Picture to Screen Edges"
            checked: options.draft.pipSnap === true
            ToolTip.text: "After moving or resizing, snap within 16 pixels of the monitor's usable edge. The Windows taskbar remains clear."
            onClicked: options.update("pipSnap", checked)
        }

        RowLayout {
            Layout.fillWidth: true

            HintLabel {
                Layout.fillWidth: true
                text: "Picture-in-Picture Aspect Ratio"
            }

            ChoiceBox {
                readonly property var ratios: ["free", "16:9", "4:3", "1:1"]
                model: ["Free Resize", "16:9", "4:3", "Square"]
                currentIndex: Math.max(0, ratios.indexOf(options.draft.pipAspectRatio))
                ToolTip.text: "Keep the whole PiP window at this ratio when dragging an edge or corner. Free Resize allows any shape."
                onActivated: options.update("pipAspectRatio", ratios[currentIndex])
            }
        }

        OptionCheckBox {
            text: "Mute Dashboard Audio"
            checked: options.draft.muted === true
            ToolTip.text: "Mute all sound from WHAT's embedded browser. The toolbar also provides a mute switch."
            onClicked: options.update("muted", checked)
        }

        OptionCheckBox {
            text: "Keep Audio Playing When Hidden"
            checked: options.draft.keepAudio === true
            ToolTip.text: "Keep the browser running when its page or overlay is hidden. This can use more CPU and memory. When off, WHAT mutes hidden audio and asks WebView2 to suspend the page."
            onClicked: options.update("keepAudio", checked)
        }
    }

    SettingsSection {
        title: "Dashboards by View"
        detail: "Choose a different dashboard for the desktop, fullscreen or PiP"

        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Leave blank to use the main dashboard. Enter a path such as /dashboard-desktop/home, or a full HTTP(S) address. Paths use the current primary or cloud server. Switching to a different saved dashboard navigates the shared browser."
        }

        Repeater {
            model: [{key: "desktopDashboard", label: "Desktop"},
                {key: "fullscreenDashboard", label: "Fullscreen Overlay"},
                {key: "pipDashboard", label: "Picture-in-Picture"}]

            delegate: ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: Theme.spaceS

                HintLabel { text: modelData.label }

                InputField {
                    Layout.fillWidth: true
                    text: options.draft[modelData.key] || ""
                    placeholderText: "Use the main dashboard"
                    maximumLength: 2048
                    ToolTip.text: "An optional dashboard path or address for this view. A different server may require a separate sign-in."
                    onTextEdited: options.update(modelData.key, text.trim())
                }
            }
        }
    }

    SettingsSection {
        title: "Site Permissions"
        detail: "Microphone, camera and notifications. Changes save immediately."
        onExpandedChanged: if (expanded) options.web.refreshPermissions()

        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: options.web.permissionsStatus
        }

        Repeater {
            model: options.web.permissions

            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0

                    HintLabel { text: modelData.label }
                    HintLabel {
                        Layout.fillWidth: true
                        text: modelData.origin
                        elide: Text.ElideMiddle
                        color: Theme.muted
                    }
                }

                ChoiceBox {
                    model: ["Ask", "Allow", "Block"]
                    currentIndex: modelData.state
                    enabled: !options.web.permissionsBusy && !options.web.cacheBusy
                    ToolTip.text: "Saved immediately for this site in WHAT's browser profile. This does not change WHAT integration permissions."
                    onActivated: options.web.setPermission(modelData.origin, modelData.kind, currentIndex)
                }
            }
        }

        RowLayout {
            QuietButton {
                text: "Refresh Permissions"
                enabled: !options.web.permissionsBusy && !options.web.cacheBusy
                ToolTip.text: "Read the permissions saved by this WebView2 profile, including prompts answered while browsing."
                onClicked: options.web.refreshPermissions()
            }

            QuietButton {
                text: "Reset to Ask"
                enabled: !options.web.permissionsBusy && !options.web.cacheBusy && options.web.permissions.length > 0
                ToolTip.text: "Remove all saved permission decisions in this browser profile. Sites will ask again; your login is kept."
                onClicked: options.web.resetPermissions()
            }
        }
    }

    SettingsSection {
        title: "Browser Diagnostics"
        detail: "Runtime, current host, visibility and the last loading error"

        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "WebView2: " + options.web.diagnostics.runtime
                + "\nBrowser process: " + (options.web.diagnostics.browserPid || "Not running")
                + "\nState: " + options.web.diagnostics.state + " | View: " + options.web.diagnostics.mode
                + "\nHost: " + (options.web.diagnostics.host || "Not loaded")
                + "\nAddress: " + options.web.diagnostics.route
                + "\nRecovery attempts: " + options.web.diagnostics.retries
                + (options.web.diagnostics.retryPending ? " (retry scheduled)" : "")
                + "\nLast error: " + (options.web.diagnostics.lastError || "None")
        }
    }
}
