import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../components"

DragScrollView {
    id: settingsScroll

    function saveConnection() {
        if (!ready || !connectionDirty || !port.acceptableInput)
            return;
        connectionDirty = false;
        controller.saveSettings(settingsScroll.status.sampleSeconds,
            settingsScroll.status.publishSeconds,
            Number(port.text),
            settingsScroll.status.license,
            settingsScroll.status.startup);
        port.text = String(settingsScroll.status.configuredPort);
    }

    // Debounce typing; do not save the initial values while controls are being populated.
    function queueConnectionSave() {
        if (ready) {
            connectionDirty = true;
            connectionSave.restart();
        }
    }

    Timer {
        id: heartbeatSave

        interval: 500
        onTriggered: controller.saveSettings(settingsScroll.status.sampleSeconds,
            publishing.value,
            settingsScroll.status.configuredPort,
            settingsScroll.status.license,
            settingsScroll.status.startup)
    }

    Timer {
        id: connectionSave

        interval: 650
        onTriggered: settingsScroll.saveConnection()
    }

    Component.onCompleted: {
        publishing.value = status.publishSeconds;
        port.text = String(status.configuredPort);
        startup.checked = status.startup;
        Qt.callLater(function () {
            settingsScroll.ready = true;
        });
    }

    function openSection(name) {
        settingsSearch.text = "";
        const sections = {
            portable: portableSection,
            connection: connectionSection,
            updates: updatesSection
        };
        const section = sections[name];
        if (!section)
            return;
        section.expanded = true;
        Qt.callLater(section.reveal);
    }

    required property var controller
    readonly property var status: controller.info
    property bool portable: status.packaged !== true
    readonly property bool mqttActive: status.running && (status.connectionTls || "").indexOf("MQTT") >= 0
    readonly property bool apiActive: status.running
        && (status.connectionTls || "").split(" + ").some(part => part.indexOf("TLS") === 0)
    readonly property bool transportConnected: apiActive || mqttActive
    property Item backdropSource: null
    required property var updater
    property string configurationMessage: ""
    property bool ready: false
    property bool connectionDirty: false
    signal notification(string message)
    signal entityCleanupRequested
    signal timingRequested
    signal installUpdateRequested
    signal releaseBrowserRequested
    signal rotateTokenRequested
    signal renewCertificateRequested
    objectName: "settingsScroll"
    clip: true
    contentWidth: availableWidth
    contentHeight: settingsContents.implicitHeight
    topPadding: settingsToolbar.implicitHeight + Theme.spaceM

    property Item toolbar: ColumnLayout {
        id: settingsToolbar
        parent: settingsScroll
        width: settingsScroll.width - Theme.scrollGutter
        spacing: Theme.spaceM
        InputField {
            id: settingsSearch

            objectName: "settingsSearch"
            Layout.fillWidth: true
            placeholderText: "Search settings - pairing, MQTT, startup, theme, animations…"
            rightPadding: 42
            onTextChanged: settingsScroll.contentItem.contentY = 0

            QuietButton {
                ToolTip.text: "Clear the current search."
                hoverEnabled: true
                ToolTip.visible: hovered
                ToolTip.delay: Theme.toolTipDelay
                ToolTip.timeout: Theme.toolTipTimeout
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "×"
                visible: settingsSearch.text.length > 0
                onClicked: settingsSearch.clear()
                Accessible.name: "Clear settings search"
            }
        }

        RowLayout {
            ActionButton {
                ToolTip.text: "Open the Home Assistant API and MQTT connection settings."
                objectName: "mqttSettingsShortcut"
                text: "Connection Settings"
                onClicked: settingsScroll.openSection("connection")
            }

            ActionButton {
                ToolTip.text: "Open update settings to check GitHub, review versions or choose a release."
                text: "Application Updates"
                onClicked: settingsScroll.openSection("updates")
            }

            Item {
                Layout.fillWidth: true
            }
        }

    }

    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    ScrollBar.vertical: WideScrollBar {
        parent: settingsScroll
        x: settingsScroll.width - width
        y: settingsScroll.topPadding
        height: settingsScroll.availableHeight
    }

    ColumnLayout {
        id: settingsContents

        width: settingsScroll.availableWidth - Theme.scrollGutter
        spacing: Theme.spaceM

        HintLabel {
            objectName: "settingsNoResults"
            text: "No matching settings"
            visible: settingsSearch.text.trim().length > 0
                && ![entityCleanupSection, portableSection, connectionSection, backupSection,
                    startupSection, appearanceSection, updatesSection, securitySection]
                    .some(section => section.searchMatch)
            Layout.fillWidth: true
        }

        SettingsSection {
            id: entityCleanupSection

            query: settingsSearch.text
            title: "Home Assistant Entities"
            keywords: "cleanup clear unavailable stale unshared obsolete remove entities"
            detail: "Clear stale, unshared or obsolete entities"

            HintLabel {
                text: "Preview obsolete entities before removing them from Home Assistant. Selected sensors are kept even when unavailable. Requires the updated integration and a local-sharing connection."
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            ChoiceBox {
                id: cleanupInstallation
                objectName: "cleanupInstallation"
                model: controller.installationConfiguration
                textRole: "name"
                valueRole: "id"
                Layout.fillWidth: true
                ToolTip.text: "Only this installation's entity registry will be inspected or cleaned. It must have an authenticated Local connection."
            }

            ActionButton {
                ToolTip.text: "Ask Home Assistant for a cleanup preview. You review the proposed removals before confirming; selected but unavailable sensors are kept."
                objectName: "previewEntityCleanup"
                text: "Clear Stale or Unshared Entities…"

                onClicked: {
                    settingsScroll.entityCleanupRequested();
                    controller.previewEntityCleanup(cleanupInstallation.currentValue || "");
                }
            }
        }

        SettingsSection {
            id: portableSection

            query: settingsSearch.text
            objectName: "portableSection"
            title: settingsScroll.portable ? "Portable Mode" : "Installation"
            detail: settingsScroll.portable ? "Limitations and installer" : "Installed and managed by Windows"

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceM

                HintLabel {
                    text: settingsScroll.portable
                        ? "You are running WHAT without Windows package identity (portable or a development build). Install WHAT using Setup.exe for all supported features."
                        : "You are running the installed version of WHAT. Installation-dependent features are available, subject to Windows permissions and compatible hardware."
                    color: settingsScroll.portable ? Theme.warning : Theme.muted
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                HintLabel {
                    visible: settingsScroll.portable
                    text: "Notification collection: portable WHAT cannot read Windows notification counts, application, time, title or contents. Sending notifications from Home Assistant to this PC still works."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                HintLabel {
                    visible: settingsScroll.portable
                    text: "Windows Dynamic Lighting: background RGB control and Windows lighting priority registration require installation. This restriction does not apply to separate RGB modules such as iCUE."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                HintLabel {
                    visible: settingsScroll.portable
                    text: "Updates: portable WHAT can check for releases automatically, but cannot install updates in place. Download and extract a new portable ZIP manually, or install WHAT for in-app MSIX updates."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                HintLabel {
                    visible: settingsScroll.portable
                    text: "Normal sensors, BEACN, Home Assistant API and MQTT remain available in portable mode. Setup.exe installs the signed MSIX and requests approval for certificate trust when needed. Quit the portable copy, install using Setup.exe, then launch WHAT from Start."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                ActionButton {
                    ToolTip.text: "Open the GitHub releases page to download the installer for this PC."
                    visible: settingsScroll.portable
                    text: "Get WHAT Installer…"
                    onClicked: updater.openReleases()
                }
            }
        }

        SettingsSection {
            id: connectionSection

            query: settingsSearch.text
            title: "Home Assistant Installations"
            keywords: "api mqtt broker host port username password tls ssl certificate remote cloud nabu casa fallback heartbeat sensor updates publish interval fast rate local sharing start stop restart"
            detail: "Named API and MQTT connections, pairing and sensor delivery"
            expanded: true

            ServiceControls {
                controller: settingsScroll.controller
                transportConnected: settingsScroll.transportConnected
                onNotification: message => settingsScroll.notification(message)
            }

            HintLabel {
                text: "Start and Stop control sharing for all installations. Sensors continue collecting while sharing is stopped."
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            InstallationList {
                controller: settingsScroll.controller
                backdropSource: settingsScroll.backdropSource
            }

            SettingsSection {
                title: "Local Pairing and Delivery"
                detail: "Shared listening port and connection heartbeat"
                query: settingsSearch.text
                keywords: "api websocket local port heartbeat"
                HintLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "Direct connections use this port, with a separate token for each installation. Copy the matching installation's pairing details above."
                }
                RowLayout {
                    HintLabel { text: "Listening Port" }
                    InputField {
                        id: port
                        objectName: "portInput"
                        implicitWidth: 130
                        maximumLength: 5
                        validator: IntValidator { bottom: 1024; top: 65535; locale: "C" }
                        onTextEdited: settingsScroll.queueConnectionSave()
                        onEditingFinished: settingsScroll.saveConnection()
                        ToolTip.text: "Shared TLS port used when Home Assistant connects to this PC. Update paired installations if you change it."
                    }
                    HintLabel { text: "Heartbeat" }
                    NumberInput {
                        id: publishing
                        objectName: "heartbeatInput"
                        from: 1
                        to: 300
                        unit: "s"
                        onValueModified: heartbeatSave.restart()
                        ToolTip.text: "How often WHAT sends a keepalive update to Home Assistant, even when readings have not changed. This does not control the Overview ECG. The ECG marks replies to separate checks sent every 5 seconds, confirming the Home Assistant integration is responding. Sensor sampling and Normal/Fast publishing use their own intervals."
                    }
                }
                HintLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "This heartbeat sends keepalive updates. The Overview ECG uses separate response checks every 5 seconds, so changing this interval does not change its beat frequency."
                }
            }

            SettingsSection {
                id: deliverySection

                query: settingsSearch.text
                title: "Sensor Updates"
                keywords: "publish interval rate fast normal milliseconds frequency"
                detail: "Normal and fast publishing intervals"

                ColumnLayout {
                    id: deliveryContents

                    Layout.fillWidth: true
                    spacing: Theme.spaceM

                    HintLabel {
                        text: "Applies to all installations. Enable Fast updates only for sensors you need to follow more closely."
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        HintLabel {
                            text: "Normal Publish Interval (ms)"
                        }

                        NumberInput {
                            id: normalPublish

                            ToolTip.text: "Interval for ordinary shared sensor updates. Currently applied: "
                                + (settingsScroll.status.publishingNormalMs || 1000) + " ms ("
                                + (settingsScroll.status.publishingNormalMs || 1000) / 1000
                                + " s). Use Apply rates to save changes."
                            unit: "ms"
                            from: 100
                            to: 60000
                            Component.onCompleted: value = settingsScroll.status.publishingNormalMs || 1000
                        }

                        HintLabel {
                            text: "Fast Updates (ms)"
                        }

                        NumberInput {
                            id: fastPublish

                            ToolTip.text: "Interval for numeric sensors with Fast updates enabled. Currently applied: "
                                + (settingsScroll.status.publishingHighMs || 100) + " ms ("
                                + (settingsScroll.status.publishingHighMs || 100) / 1000
                                + " s). Use Apply rates to save changes."
                            unit: "ms"
                            from: 50
                            to: normalPublish.value
                            Component.onCompleted: value = settingsScroll.status.publishingHighMs || 100
                        }

                        ActionButton {
                            ToolTip.text: "Save the normal and fast publishing intervals. These control how often readings are sent, not how often their source is sampled."
                            text: "Apply Rates"
                            onClicked: controller.setPublishingIntervals(normalPublish.value, fastPublish.value)
                        }
                    }
                }
            }
        }

        SettingsSection {
            id: backupSection

            query: settingsSearch.text
            title: "Configuration Backup"
            keywords: "yaml export import restore settings preferences"
            detail: "Export or restore your preferences"

            ColumnLayout {
                id: configurationContents

                Layout.fillWidth: true
                spacing: Theme.spaceM

                HintLabel {
                    text: "Export settings to YAML, or review and import a saved configuration. Passwords, pairing keys, certificate files and executable paths are excluded. Windows sign-in registration stays on this PC."
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                RowLayout {
                    ActionButton {
                        ToolTip.text: "Save a configuration backup to a file. It includes sensor names and connection addresses; keep the file private."
                        objectName: "exportConfiguration"
                        text: "Export Configuration…"
                        onClicked: controller.exportConfiguration()
                    }

                    ActionButton {
                        ToolTip.text: "Choose a backup and review its contents before scheduling an import for the next start."
                        objectName: "importConfiguration"
                        text: "Import Configuration…"
                        onClicked: controller.previewConfiguration()
                    }
                }

                HintLabel {
                    text: settingsScroll.configurationMessage
                    visible: text.length > 0
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        SettingsSection {
            id: startupSection

            query: settingsSearch.text
            title: "Startup"
            keywords: "windows sign in login minimised minimized notification tray launch"
            detail: "Windows sign-in and system tray behaviour"

            ColumnLayout {
                id: startupContents

                Layout.fillWidth: true
                spacing: Theme.spaceM

                OptionCheckBox {
                    id: startup
                    objectName: "windowsStartupSetting"

                    ToolTip.text: settingsScroll.status.startupAvailable
                        ? "Launch WHAT automatically when you sign in to Windows."
                        : "Windows startup is disabled in Debug builds. Use a Release build."
                    onClicked: controller.setStartupEnabled(checked)
                    text: "Start at Windows Sign-In"
                    enabled: settingsScroll.status.startupAvailable
                        && !settingsScroll.status.testMode && !settingsScroll.status.startupBusy
                }

                HintLabel {
                    visible: !settingsScroll.status.startupAvailable
                    text: "Windows startup is disabled in Debug builds. Use an installed or portable Release build."
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                OptionCheckBox {
                    ToolTip.text: "Start WHAT in the system tray instead of opening the main window."
                    objectName: "startMinimisedSetting"
                    text: "Start Minimised"
                    checked: settingsScroll.status.startMinimised
                    onClicked: controller.setStartMinimised(checked)
                }

                Label {
                    text: "Run in the system tray when WHAT opens."
                    color: Theme.muted
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                OptionCheckBox {
                    ToolTip.text: "Show a Windows notification confirming that WHAT started in the system tray."
                    objectName: "startupNotificationSetting"
                    text: "Show a Startup Notification When Minimised"
                    checked: settingsScroll.status.startupNotice
                    enabled: settingsScroll.status.startMinimised
                    onClicked: controller.setStartupNotice(checked)
                }
            }
        }

        SettingsSection {
            id: appearanceSection

            query: settingsSearch.text
            title: "Appearance"
            keywords: "theme colour color blue grey gray dark animation animate fade motion hover logo timing minimise minimize maximise maximize window lens refraction strength tidal ink mercury mist silk contour frost ember prism pour molten precision fresnel anamorphic twin meniscus"
            detail: "Theme, animations and interface timing"

            ColumnLayout {
                id: appearanceContents

                Layout.fillWidth: true
                spacing: Theme.spaceM

                RowLayout {
                    Label {
                        text: "Theme"
                        color: Theme.ink
                    }

                    ChoiceBox {
                        objectName: "themeSelector"
                        Accessible.name: "Application theme"
                        textRole: "label"
                        valueRole: "name"
                        model: Theme.choices
                        currentIndex: Math.max(0, Theme.choices.findIndex(theme => theme.name === controller.uiTheme))
                        onActivated: controller.uiTheme = currentValue
                        ToolTip.text: "Change the colours throughout WHAT. Your choice is applied immediately and remembered on this PC."
                    }
                }

                OptionCheckBox {
                    ToolTip.text: "Animate WHAT pages, prompts, controls, panels and the logo. Turn this off to make their state changes immediate."
                    objectName: "animationsToggle"
                    text: "Enable Animations"
                    checked: Theme.animationsEnabled
                    onClicked: controller.setAnimationsEnabled(checked)
                }

                HintLabel {
                    text: "Open and close effects, page transitions, logos, hover effects and expanding sections. Changes apply immediately and are remembered."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                OptionCheckBox {
                    ToolTip.text: "Let Windows animate minimising and restoring the WHAT window."
                    objectName: "windowAnimationsToggle"
                    text: "Use Windows Window Animations"
                    checked: settingsScroll.status.windowAnimationsEnabled !== false
                    enabled: Theme.animationsEnabled
                    onClicked: controller.setWindowAnimationsEnabled(checked)
                }

                RowLayout {
                    enabled: Theme.animationsEnabled

                    Label {
                        text: "Open / Close Effect"
                        color: Theme.ink
                    }
                    ChoiceBox {
                        objectName: "transitionStyleSelector"
                        readonly property var styles: settingsScroll.status.transitionStyles || []
                        model: styles.map(style => style.label)
                        currentIndex: styles.findIndex(style => style.id === Theme.transitionStyle)
                        onActivated: controller.setTransitionStyle(styles[currentIndex].id)
                        ToolTip.text: "Choose how WHAT's window, prompts, menus and tooltips open and close. Expanding sections, sensor rows and page changes keep their slide animations. Off disables only open/close effects. Separate colour/file dialogs, native tray menus and browser content keep their own rendering. Software rendering uses Standard."
                    }
                }

                RowLayout {
                    enabled: Theme.shaderTransitionEnabled

                    Label {
                        text: "Effect Strength"
                        color: Theme.ink
                    }
                    NumberInput {
                        objectName: "refractionStrengthInput"
                        unit: "%"
                        from: 0
                        to: 150
                        stepSize: 10
                        value: Theme.refractionStrength
                        onValueModified: controller.setRefractionStrength(value)
                        ToolTip.text: "Refraction and rim brightness as a percentage. 100 uses the default strength. Higher values make the distortion more visible without slowing the animation. Zero keeps the reveal without distortion."
                    }
                }

                ActionButton {
                    ToolTip.text: "Adjust open and close effects, page transitions, controls, panels, logos and notification timings."
                    text: "Animation Timings…"
                    onClicked: settingsScroll.timingRequested()
                }
            }
        }

        SettingsSection {
            id: updatesSection

            query: settingsSearch.text
            objectName: "updatesSection"
            title: "Application Updates"
            keywords: "version github beta release download install automatic certificate signing expired signature"
            detail: (updater.info.signingWarning || {}).title || "Release channel and update preferences"

            ColumnLayout {
                id: updateSettings
                readonly property var signingWarning: updater.info.signingWarning || ({})

                Layout.fillWidth: true
                spacing: Theme.spaceM

                WarningBanner {
                    objectName: "updateSigningWarning"
                    visible: !!updateSettings.signingWarning.title
                    title: updateSettings.signingWarning.title || ""
                    detail: updateSettings.signingWarning.detail || ""
                    critical: updateSettings.signingWarning.critical === true
                    actionText: "Open GitHub Releases"
                    inlineAction: true
                    actionHelp: "Open WHAT's official releases. Download Setup.exe for your PC and run it over the existing installation to keep your settings."
                    onActionTriggered: updater.openReleases(updateSettings.signingWarning.version || "")
                }

                HintLabel {
                    text: "Running version: " + updater.info.current
                    Layout.fillWidth: true
                }

                HintLabel {
                    visible: settingsScroll.portable
                    text: "Portable mode: update checks work, but update installation requires the installed version of WHAT."
                    color: Theme.warning
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                ActionButton {
                    ToolTip.text: "Review features that require installation and open the installer download page."
                    visible: settingsScroll.portable
                    text: "Portable Limitations and Installer…"
                    onClicked: settingsScroll.openSection("portable")
                }

                OptionCheckBox {
                    ToolTip.text: "Check for releases in the background. WHAT still asks before installing an update."
                    text: "Check GitHub for Updates Automatically"
                    checked: updater.info.automatic
                    onClicked: updater.setAutomatic(checked)
                }

                OptionCheckBox {
                    ToolTip.text: "Include beta and other prerelease builds when checking for updates."
                    objectName: "betaUpdatesToggle"
                    text: "Include Beta Updates"
                    checked: updater.info.betaUpdates
                    enabled: !updater.info.busy
                    onClicked: updater.setBetaUpdates(checked)
                }

                HintLabel {
                    text: updater.info.status
                    color: updater.info.error ? Theme.error : Theme.muted
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                HintLabel {
                    visible: !updater.info.canInstall && (!!updater.info.available || !updater.info.packaged)
                    text: updater.info.installBlockedReason
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                ProgressBar {
                    palette.dark: Theme.brand
                    palette.midlight: Theme.outline
                    objectName: "updateDownloadProgress"
                    visible: updater.info.busy
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: updater.info.progress
                    indeterminate: !updater.info.downloading || updater.info.downloadTotal <= 0
                }

                HintLabel {
                    visible: updater.info.busy && updater.info.downloading
                    text: (updater.info.downloadBytes / 1048576).toFixed(1) + " MB"
                            + (updater.info.downloadTotal > 0
                                ? " / " + (updater.info.downloadTotal / 1048576).toFixed(1)
                                    + " MB · " + Math.floor(updater.info.progress * 100) + "%"
                                : " downloaded")
                }

                RowLayout {
                    ActionButton {
                        ToolTip.text: "Check GitHub for an update on your selected release channel."
                        text: "Check Now"
                        enabled: !updater.info.busy
                        onClicked: updater.check()
                    }

                    ActionButton {
                        ToolTip.text: "Review the offered update, then download and run its installer. WHAT closes for installation."
                        objectName: "installUpdateButton"
                        text: "Download and Install…"
                        primary: true
                        enabled: !settingsScroll.portable && updater.info.canInstall
                        onClicked: settingsScroll.installUpdateRequested()
                    }

                    ActionButton {
                        ToolTip.text: "Browse available releases, including older versions, and review one before installing it."
                        text: "Choose Release…"
                        enabled: !updater.info.busy
                        onClicked: settingsScroll.releaseBrowserRequested()
                    }

                    ActionButton {
                        ToolTip.text: "Open WHAT releases on GitHub to read release notes and download files."
                        text: "Open Releases"
                        onClicked: updater.openReleases()
                    }
                }

                HintLabel {
                    text: "Automatic checks never install anything. Choose a release to upgrade or return to an older version. Beta releases are hidden unless enabled."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
        }

        HintLabel {
            text: settingsScroll.status.settingsStatus
            visible: text.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.accent
        }

        SettingsSection {
            id: securitySection

            query: settingsSearch.text
            title: "Pairing and Security"
            keywords: "token pairing replace reset renew certificate fingerprint expires expiry tls security manage restart logging home assistant"
            detail: "Pairing token and connection certificate"

            ColumnLayout {
                id: certificateSettings

                Layout.fillWidth: true
                spacing: Theme.spaceM

                RowLayout {
                    Layout.fillWidth: true
                    ToggleSwitch {
                        id: appControlPermission
                        objectName: "appControlPermission"
                        Layout.fillWidth: true
                        text: "Allow Home Assistant to Manage WHAT"
                        checked: settingsScroll.status.appControlEnabled
                        ToolTip.text: "Allow paired Home Assistant connections to change diagnostic logging and the normal publishing interval, and restart WHAT. This does not grant Windows power controls or memory dumps."
                        onToggled: controller.setAppControlEnabled(checked)
                    }
                    PermissionBadge {
                        text: appControlPermission.checked ? "Home Assistant Can Run" : "Not Allowed"
                        access: appControlPermission.checked ? PermissionBadge.Controls : PermissionBadge.Blocked
                        help: appControlPermission.ToolTip.text
                    }
                }

                HintLabel {
                    text: "Adds app controls to the WHAT PC device in Home Assistant. Version, uptime and process diagnostics are available without this permission."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                HintLabel {
                    text: settingsScroll.status.certificateWarning
                        || (settingsScroll.status.certificateExpiry
                            ? "Expires: " + settingsScroll.status.certificateExpiry
                                + " (" + settingsScroll.status.certificateDays + " days remaining)"
                            : "Preparing certificate…")
                    color: settingsScroll.status.certificateExpired ? Theme.error
                        : settingsScroll.status.certificateWarning ? Theme.warning : Theme.muted
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                HintLabel {
                    text: "Renewal changes the fingerprint. It does not change your token, PC identity or sensor selections."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                ActionButton {
                    ToolTip.text: "Generate a separate new token for every installation after confirmation. Reconfigure each Home Assistant installation using its own new Pairing details."
                    text: "Replace All Pairing Tokens…"
                    onClicked: settingsScroll.rotateTokenRequested()
                }

                ActionButton {
                    ToolTip.text: "Generate a new TLS certificate after confirmation. Home Assistant will need its new fingerprint."
                    text: settingsScroll.status.certificateBusy ? "Generating certificate…" : "Renew certificate…"
                    enabled: !settingsScroll.status.certificateBusy
                    onClicked: settingsScroll.renewCertificateRequested()
                }
            }
        }
    }
}
