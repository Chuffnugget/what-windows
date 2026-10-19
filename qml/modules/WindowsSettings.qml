import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ModuleCard {
    id: windowsSettings

    property var lightingDrafts: ({})
    property bool portable: controller.info.packaged !== true
    signal portableRequested
    signal commandsRequested
    moduleKey: "windows"
    heading: "Windows"
    summary: "System sensors and optional personal data"
    keywords: "windows security defender antivirus firewall updates display displays topology layout primary mirror extend disconnect system storage disk memory audio network power polling interval user session idle lock remote control reboot shutdown hibernate monitor sleep url web browser plans foreground application window titles privacy microphone events errors messages media track notifications contents speech tts speak announcements replies actions shell powershell pwsh cmd wsl command commands terminal output stream debugging debug"

    RowLayout {
        Layout.fillWidth: true

        HintLabel {
            text: "Windows Polling Interval"
            Layout.fillWidth: true
        }

        TimingSlider {
            input: winPolling
            minimum: 1000
            maximum: 10000
            increment: 500
            label: "Windows Polling Interval"
        }

        PollingInput {
            id: winPolling

            controller: windowsSettings.controller
            moduleKey: "windows"
            from: 500
            to: 60000
            stepSize: 500
        }
    }

    HintLabel {
        text: "System, memory, storage, output audio, network traffic and power are included while enabled. Wi-Fi details can be disabled below."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    HintLabel {
        text: "Optional Windows submodules. Remote permissions are off until you enable them; existing choices are retained."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    ModuleSection {
        objectName: "wifiSubmodule"
        searchContext: "windows"
        title: "Wi-Fi Details"
        detail: "Network name and signal strength - Windows location permission required"
        keywords: "wifi wi-fi network ssid signal location privacy permission denied"
        query: windowsSettings.query
        active: windowsSettings.controller.info.sources.wifi === true
        onToggled: enabled => controller.saveSources({wifi: enabled})

        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Windows protects Wi-Fi details with its location permission. WHAT uses this for the network name and signal strength, not GPS coordinates. Declining only makes these readings unavailable; other network readings and Home Assistant connections still work."
        }

        HintLabel {
            objectName: "wifiPermissionWarning"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.warning
            text: controller.info.wifiIssue || ""
            visible: text.length > 0
        }

        ActionButton {
            objectName: "wifiLocationSettings"
            text: "Open Windows Location Settings"
            ToolTip.text: "Allow location access for WHAT in Windows. After a denial, Windows does not show the one-time prompt again. Readings resume automatically after permission is restored and Wi-Fi Details is enabled."
            onClicked: Qt.openUrlExternally("ms-settings:privacy-location")
        }
    }

    ModuleSection {
        searchContext: "windows"
        objectName: "commandsSubmodule"
        title: "Allow Home Assistant to Send Shell Commands"
        detail: "PowerShell, CMD and WSL supplied by Home Assistant - runs as your Windows user"
        keywords: "shell terminal command output stream debug powershell pwsh cmd wsl permissions"
        query: windowsSettings.query
        active: windowsSettings.controller.info.controlPermissions.shell === true
        onToggled: allowed => controller.setControlPermission("shell", allowed)

        PermissionBadge {
            text: !windowsSettings.controller.info.controlPermissions.shell ? "Not Allowed"
                : windowsSettings.controller.info.windowsEnabled ? "Home Assistant Shell Access" : "Blocked"
            access: windowsSettings.controller.info.windowsEnabled && windowsSettings.controller.info.controlPermissions.shell
                ? PermissionBadge.Shell : PermissionBadge.Blocked
            help: "Home Assistant can supply scripts that run as your Windows user. This is broader access than triggering saved commands."
        }

        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.error
            text: "Home Assistant can supply commands from scripts, automations or its UI. If Home Assistant is compromised, this could allow access to files, settings and programs on this PC. Disabling this permission cancels active Home Assistant shell commands. Commands can also request that launched apps stay open after completion; WHAT no longer monitors or stops those apps."
        }
        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Saved commands have a separate permission under Custom. They are defined on this PC and triggered by ID; you do not need this broader shell permission to use them."
        }
        ActionButton {
            text: "Open Commands and Activity"
            onClicked: windowsSettings.commandsRequested()
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Media"
        detail: "Playback state, track information and optional controls"
        keywords: "audio volume mute seek next previous track artist privacy"
        query: windowsSettings.query
        active: windowsSettings.controller.info.sources.media === true
        onToggled: enabled => controller.saveSources({
                media: enabled
            })

        ControlOption {
            controller: windowsSettings.controller
            query: windowsSettings.query
            permission: "media"
            text: "Allow Home Assistant Playback, Seeking, Volume and Mute"
            enabled: windowsSettings.controller.info.sources.media
        }

        SourceOption {
            controller: windowsSettings.controller
            query: windowsSettings.query
            key: "media_titles"
            text: "Include Track Title and Artist"
            enabled: windowsSettings.controller.info.sources.media
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Power Actions"
        detail: "Sleep, hibernate, restart, shutdown and lock"
        keywords: "remote control power"
        query: windowsSettings.query
        active: windowsSettings.controller.info.controlPermissions.power === true
        onToggled: enabled => controller.setControlPermission("power", enabled)

        HintLabel {
            text: "When enabled, paired Home Assistant clients may use these controls. Changes save automatically."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Speech and Audio from Home Assistant"
        detail: "Speak with a Windows voice or play Home Assistant announcements"
        keywords: "tts text speech speak announcements audio media player voices"
        query: windowsSettings.query
        active: controller.info.controlPermissions.announcements === true
        onToggled: enabled => controller.setControlPermission("announcements", enabled)

        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Home Assistant gets a separate WHAT Announcements media player for TTS and audio URLs. The what.speak action uses installed Windows voices. Speech runs on this PC and uses its default audio output."
        }

        Repeater {
            model: controller.announcements.voices
            delegate: HintLabel {
                required property var modelData
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
                text: modelData.name + " (" + modelData.language + ") - " + modelData.id
            }
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Power Modes"
        detail: "Switch Windows power plans and power modes"
        keywords: "remote control power_plan"
        query: windowsSettings.query
        active: windowsSettings.controller.info.controlPermissions.power_plan === true
        onToggled: enabled => controller.setControlPermission("power_plan", enabled)

        HintLabel {
            text: "When enabled, paired Home Assistant clients may use these controls. Changes save automatically."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Displays"
        detail: "Extend, mirror, disconnect and restore named monitor layouts"
        keywords: "display primary topology layout monitor"
        query: windowsSettings.query
        active: controller.displays.enabled === true
        onToggled: enabled => controller.setDisplaysEnabled(enabled)

        PermissionOption {
            help: "Allow paired Home Assistant clients to enable, disable, mirror and rearrange monitors. Local controls remain available separately."
            text: "Allow Home Assistant to Change Display Layouts"
            checked: controller.displays.control_enabled === true
            onToggled: allowed => controller.setDisplayControl(allowed)
        }

        HintLabel {
            text: "Controls and saved layouts appear in the sensor browser. Display changes apply immediately with no confirmation or automatic rollback. Disconnect removes a screen from the desktop; it does not turn its power off. See docs/DISPLAYS.md for setup and Home Assistant automation examples."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "windows"
        objectName: "dynamicLightingModule"
        // Availability is determined by Windows package identity.
        available: !windowsSettings.portable
        unavailableReason: "Portable mode - install WHAT to use this feature"
        onRestrictionDetailsRequested: windowsSettings.portableRequested()
        title: "Dynamic Lighting"
        detail: "Windows-native RGB lighting for compatible devices"
        keywords: "rgb lighting colour color keyboard mouse lamp array"
        query: windowsSettings.query
        active: controller.dynamicLighting.enabled === true
        onToggled: enabled => controller.setDynamicLightingOption("enabled", enabled)

        PermissionOption {
            help: "Allow paired Home Assistant clients to send Dynamic Lighting commands to this PC."
            text: "Allow Home Assistant to Control Lighting"
            checked: controller.dynamicLighting.control_enabled === true
            onToggled: allowed => controller.setDynamicLightingOption("control", allowed)
        }

        PermissionOption {
            help: "Permit Dynamic Lighting commands through cloud connections as well as local ones."
            text: "Allow Lighting Control over Cloud Connections"
            checked: controller.dynamicLighting.cloud === true
            badgeText: "Cloud Home Assistant"
            enabled: controller.dynamicLighting.control_enabled
            onToggled: allowed => controller.setDynamicLightingOption("cloud", allowed)
        }

        HintLabel {
            text: controller.dynamicLighting.status || "Disabled"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        HintLabel {
            visible: !!controller.dynamicLighting.feedback
            text: controller.dynamicLighting.feedback || ""
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        HintLabel {
            text: "For background control, install WHAT and prioritise WHAT under Windows Settings > Personalisation > Dynamic Lighting > Background light control. Only compatible LampArray devices appear. Avoid controlling the same device through iCUE at the same time."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        RowLayout {
            ActionButton {
                ToolTip.text: "Open Windows Dynamic Lighting settings to manage devices and application priority."
                text: "Windows Lighting Settings"
                onClicked: Qt.openUrlExternally("ms-settings:personalization-lighting")
            }

            ActionButton {
                ToolTip.text: "Ask the Windows lighting helper for the current devices and their availability."
                text: "Refresh Devices"
                enabled: controller.dynamicLighting.enabled
                onClicked: controller.refreshDynamicLighting()
            }
        }

        Repeater {
            model: controller.dynamicLighting.devices || []

            delegate: ColumnLayout {
                id: lightingDevice

                objectName: "lightingDevice_" + modelData.id
                required property var modelData
                property color chosen: windowsSettings.lightingDrafts[modelData.id]
                    ? windowsSettings.lightingDrafts[modelData.id].colour
                    : Theme.accent

                function remember() {
                    windowsSettings.lightingDrafts[modelData.id] = {
                        colour: chosen,
                        brightness: lampBrightness.value
                    };
                }

                Layout.fillWidth: true

                Label {
                    text: lightingDevice.modelData.name + " · " + lightingDevice.modelData.lamp_count + " LEDs"
                    color: Theme.ink
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                HintLabel {
                    visible: !lightingDevice.modelData.available
                    text: "Windows has not granted WHAT control of this device."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    enabled: lightingDevice.modelData.available === true

                    ActionButton {
                        ToolTip.text: "Choose a colour for this lighting device. Use Apply to send it to the device."
                        text: "Choose Colour…"

                        onClicked: {
                            const colour = controller.colourPalette.choose(lightingDevice.chosen, "Lighting Colour");
                            if (colour.valid) {
                                lightingDevice.chosen = colour;
                                lightingDevice.remember();
                            }
                        }
                    }

                    Rectangle {
                        width: 26
                        height: 26

                        color: lightingDevice.chosen
                    }

                    ValueSlider {
                        id: lampBrightness

                        from: 0
                        to: 100
                        value: windowsSettings.lightingDrafts[lightingDevice.modelData.id]
                            ? windowsSettings.lightingDrafts[lightingDevice.modelData.id].brightness
                            : 100
                        stepSize: 1
                        onMoved: lightingDevice.remember()
                        Layout.fillWidth: true
                        Accessible.name: "Lighting brightness"
                    }

                    HintLabel {
                        text: Math.round(lampBrightness.value) + "%"
                    }

                    ActionButton {
                        ToolTip.text: "Send the selected colour and brightness to this lighting device."
                        text: "Apply"
                        onClicked: controller.applyDynamicLighting({
                            operation: "set",
                            device: lightingDevice.modelData.id,
                            rgb: [Math.round(lightingDevice.chosen.r * 255),
                                Math.round(lightingDevice.chosen.g * 255),
                                Math.round(lightingDevice.chosen.b * 255)],
                            brightness: lampBrightness.value
                        })
                    }

                    ActionButton {
                        ToolTip.text: "Turn off this Dynamic Lighting device by setting its brightness to zero."
                        text: "Off"
                        onClicked: controller.applyDynamicLighting({
                            operation: "set",
                            device: lightingDevice.modelData.id,
                            rgb: [0, 0, 0],
                            brightness: 0
                        })
                    }
                }
            }
        }

        HintLabel {
            text: "Displayed light state is the last colour WHAT successfully applied, not a readback of another app's lighting. Disabling this submodule releases control back to Windows."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Monitors"
        detail: "Allow putting monitors to sleep"
        keywords: "remote control monitor"
        query: windowsSettings.query
        active: windowsSettings.controller.info.controlPermissions.monitor === true
        onToggled: enabled => controller.setControlPermission("monitor", enabled)

        HintLabel {
            text: "When enabled, paired Home Assistant clients may use these controls. Changes save automatically."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Web URLs"
        detail: "Allow opening HTTP/HTTPS URLs"
        keywords: "remote control urls"
        query: windowsSettings.query
        active: windowsSettings.controller.info.controlPermissions.urls === true
        onToggled: enabled => controller.setControlPermission("urls", enabled)

        HintLabel {
            text: "When enabled, paired Home Assistant clients may use these controls. Changes save automatically."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Session"
        detail: "Windows user, idle time, lock and remote session"
        keywords: "session  privacy"
        query: windowsSettings.query
        active: windowsSettings.controller.info.sources.session === true
        onToggled: enabled => controller.saveSources({
                session: enabled
            })

        HintLabel {
            text: "Disable this submodule to stop collecting and displaying its readings."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Foreground Application"
        detail: "Active application and optional window titles"
        keywords: "foreground window_titles privacy"
        query: windowsSettings.query
        active: windowsSettings.controller.info.sources.foreground === true
        onToggled: enabled => controller.saveSources({
                foreground: enabled
            })

        SourceOption {
            controller: windowsSettings.controller
            query: windowsSettings.query
            key: "window_titles"
            text: "Include Window Titles"
            enabled: windowsSettings.controller.info.sources.foreground
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Microphone"
        detail: "Device, volume and mute; no recording"
        keywords: "microphone  privacy"
        query: windowsSettings.query
        active: windowsSettings.controller.info.sources.microphone === true
        onToggled: enabled => controller.saveSources({
                microphone: enabled
            })

        HintLabel {
            text: "Disable this submodule to stop collecting and displaying its readings."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Windows Security"
        detail: "Antivirus, firewall and automatic-update health"
        keywords: "security defender antivirus firewall updates"
        query: windowsSettings.query
        active: windowsSettings.controller.info.sources.security === true
        onToggled: enabled => controller.saveSources({
                security: enabled
            })

        HintLabel {
            text: "Read-only Windows Security Center status, refreshed once a minute. Includes Microsoft Defender or your registered antivirus. Select these readings in the sensor browser to share them with Home Assistant."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        ActionButton {
            ToolTip.text: "Open the Windows Security application to inspect protection settings and alerts."
            text: "Open Windows Security"
            onClicked: Qt.openUrlExternally("windowsdefender:")
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Windows Events"
        detail: "Errors and critical events"
        keywords: "events event_messages privacy"
        query: windowsSettings.query
        active: windowsSettings.controller.info.sources.events === true
        onToggled: enabled => controller.saveSources({
                events: enabled
            })

        SourceOption {
            controller: windowsSettings.controller
            query: windowsSettings.query
            key: "event_messages"
            text: "Include Event Messages"
            enabled: windowsSettings.controller.info.sources.events
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Notifications from Home Assistant"
        detail: "Send a title and message to this PC"
        keywords: "notifications send title message warning error"
        query: windowsSettings.query
        active: windowsSettings.controller.info.controlPermissions.notifications === true
        onToggled: enabled => controller.setControlPermission("notifications", enabled)

        HintLabel {
            text: "Allows what.send_notification. Short text notifications work in portable and installed builds. Install WHAT for images, longer messages, action buttons and replies. Notification-reading access is separate. Windows notification settings and Do not disturb control whether messages appear."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "windows"
        title: "Keyboard and Mouse from Home Assistant"
        detail: "Send shortcuts, type text, move the pointer and click on the active desktop"
        keywords: "input keyboard mouse shortcuts typing click"
        query: windowsSettings.query
        active: windowsSettings.controller.info.controlPermissions.input === true
        onToggled: enabled => controller.setControlPermission("input", enabled)

        HintLabel {
            text: "Allows full control of the active desktop, including shortcuts such as Windows+R. Pointer coordinates use physical pixels across all monitors, not scaled UI coordinates. Requires an unlocked desktop; elevated windows may reject input. Keyboard text is omitted from local logs, but can appear in Home Assistant automation traces."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    ModuleSection {
        searchContext: "windows"
        // Availability is determined by Windows package identity.
        available: !windowsSettings.portable
        unavailableReason: "Portable mode - install WHAT to use this feature"
        onRestrictionDetailsRequested: windowsSettings.portableRequested()
        title: "Notification Collection"
        detail: "Count, application and time"
        keywords: "notifications notification_contents privacy"
        query: windowsSettings.query
        active: windowsSettings.controller.info.sources.notifications === true
        onToggled: enabled => controller.saveSources({
                notifications: enabled
            })

        SourceOption {
            controller: windowsSettings.controller
            query: windowsSettings.query
            key: "notification_contents"
            text: "Include Notification Contents"
            enabled: windowsSettings.controller.info.sources.notifications
        }

        ActionButton {
            ToolTip.text: "Ask Windows to allow WHAT to read notifications. Windows requires the installed version of WHAT for this permission."
            text: windowsSettings.controller.info.packaged
                ? "Request notification access…"
                : "Installation required for notification access"
            enabled: windowsSettings.controller.info.packaged
            onClicked: controller.requestNotificationAccess()
        }
    }

    SourceOption {
        controller: windowsSettings.controller
        query: windowsSettings.query
        key: "auto_drives"
        text: "Automatically Share New Drives' Connected State and Usage"
    }
}
