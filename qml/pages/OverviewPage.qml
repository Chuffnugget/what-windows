import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../components"
import "../modules"
import "../modules/ModuleStatus.js" as ModuleStatus

DragScrollView {
    id: overviewScroll

    required property var controller
    required property var updater
    readonly property var signingWarning: updater.info.signingWarning || ({})
    required property Item backdropSource
    readonly property bool blocksWebView: pairingBody.blocksWebView
    readonly property var status: controller.info
    readonly property var progress: status.connectionProgress || ({})
    property bool active: visible
    property double now: Date.now()
    // Address visibility is only for this session, never a saved preference.
    property alias hideAddress: pairingBody.hideAddress

    function secondsSince(timestamp) {
        return Math.max(0, Math.floor((now - timestamp) / 1000));
    }

    readonly property string attemptTiming: {
        if (progress.retryAt > 0) {
            const last = progress.attemptAt > 0 ? "Last attempt " + secondsSince(progress.attemptAt) + " s ago · " : "";
            return last + "Next attempt in " + Math.max(0, Math.ceil((progress.retryAt - now) / 1000)) + " s";
        }

        if (progress.attemptAt > 0 && (progress.phase === "connecting" || progress.phase === "authenticating"))
            return "Attempt " + progress.attempt + " · " + secondsSince(progress.attemptAt) + " s elapsed";
        if (progress.busy && progress.since > 0)
            return "Waiting " + secondsSince(progress.since) + " s";
        if (progress.error && progress.attemptAt > 0)
            return "Last attempt " + secondsSince(progress.attemptAt) + " s ago";
        return "";
    }

    Timer {
        interval: 1000
        repeat: true
        triggeredOnStart: true
        running: overviewScroll.active
        onTriggered: overviewScroll.now = Date.now()
    }

    property bool portable: status.packaged !== true
    readonly property bool mqttActive: status.running && (status.connectionTls || "").indexOf("MQTT") >= 0
    readonly property bool apiActive: status.running
        && (status.connectionTls || "").split(" + ").some(part => part.indexOf("TLS") === 0)
    readonly property bool transportConnected: apiActive || mqttActive
    readonly property bool pairingFirst: (status.clients || 0) === 0
    signal connectionRequested
    signal renewCertificateRequested
    signal portableRequested
    signal moduleRequested(string key)
    signal navigationRequested(int page)
    signal notification(string message)
    objectName: "overviewScroll"
    clip: true

    contentWidth: availableWidth
    contentHeight: overviewContents.implicitHeight

    ModuleReorderArea {
        id: overviewReorder
        objectName: "overviewReorderArea"
        controller: overviewScroll.controller
        scrollView: overviewScroll
        contents: overviewModules
        cards: overviewModules.children.filter(item => typeof item.moduleKey === "string")
    }

    FastWheel {
        parent: overviewScroll.contentItem
        flick: overviewScroll.contentItem
    }

    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    ScrollBar.vertical: WideScrollBar {
        parent: overviewScroll
        x: overviewScroll.width - width
        y: overviewScroll.topPadding
        height: overviewScroll.availableHeight
    }

    ColumnLayout {
        id: overviewContents

        width: overviewScroll.availableWidth - Theme.scrollGutter
        spacing: Theme.spaceL

        WarningBanner {
            objectName: "arm64OverviewWarning"
            informational: true
            visible: overviewScroll.status.architecture === "arm64"
            title: "Native ARM64"
            detail: "BEACN, iCUE, Core Temp, NVIDIA and Wallpaper Engine are unavailable in this build. See Modules for details. Saved settings and sensor selections are kept."
        }

        WarningBanner {
            objectName: "portableOverviewWarning"
            informational: true
            visible: overviewScroll.portable
            title: "Portable Mode"
            detail: "Some Windows features and update installation need the installer."
            actionText: "Installation Details"
            inlineAction: true
            actionHelp: "See which Windows features need an installed copy of WHAT and how to install it."
            onActionTriggered: overviewScroll.portableRequested()
        }

        WarningBanner {
            objectName: "overviewUpdateSigningWarning"
            visible: !!overviewScroll.signingWarning.title
            title: overviewScroll.signingWarning.title || ""
            detail: overviewScroll.signingWarning.detail || ""
            critical: overviewScroll.signingWarning.critical === true
            actionText: "Open GitHub Releases"
            inlineAction: true
            actionHelp: "Open WHAT's official releases. Download Setup.exe for your PC and run it over the existing installation to keep your settings."
            onActionTriggered: overviewScroll.updater.openReleases(overviewScroll.signingWarning.version || "")
        }

        WarningBanner {
            objectName: "overviewCertificateWarning"
            visible: !!overviewScroll.status.certificateWarning
            title: "Home Assistant Certificate"
            detail: overviewScroll.status.certificateWarning || ""
            critical: overviewScroll.status.certificateExpired === true

            RowLayout {
                Layout.topMargin: Theme.spaceS

                ActionButton {
                    objectName: "overviewRenewCertificate"
                    text: overviewScroll.status.certificateBusy ? "Generating Certificate…" : "Renew Certificate…"
                    enabled: !overviewScroll.status.certificateBusy
                    ToolTip.text: "Generate a replacement certificate after confirmation. The PC ID, pairing token and sensor selections are kept."
                    onClicked: overviewScroll.renewCertificateRequested()
                }

                QuietButton {
                    text: "Copy Fingerprint"
                    enabled: !!overviewScroll.status.fingerprint && !overviewScroll.status.certificateBusy
                    ToolTip.text: "Copy the current fingerprint for this PC's existing WHAT entry in Home Assistant."
                    onClicked: {
                        controller.copyFingerprint();
                        overviewScroll.notification("Certificate fingerprint copied");
                    }
                }
            }
        }

        Rectangle {
            objectName: "overview-comms"
            Layout.fillWidth: true
            implicitHeight: connectionSummary.implicitHeight + 40
            color: Theme.inset

            ColumnLayout {
                id: connectionSummary

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 20
                spacing: Theme.spaceL

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceM

                    Item {
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26

                        BusyIndicator {
                            objectName: "connectionProgressSpinner"
                            anchors.fill: parent
                            padding: 0
                            palette.dark: Theme.accent
                            visible: overviewScroll.progress.busy && Theme.animationsEnabled
                            running: visible && overviewScroll.active
                        }

                        Rectangle {
                            anchors.centerIn: parent
                            width: 9
                            height: 9

                            visible: !overviewScroll.progress.busy || !Theme.animationsEnabled
                            color: overviewScroll.progress.error ? Theme.error
                                : overviewScroll.transportConnected
                                    || overviewScroll.progress.busy
                                    ? Theme.accent
                                    : Theme.muted
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs

                        Label {
                            TapHandler {
                                onTapped: overviewScroll.connectionRequested()
                            }

                            HoverHandler {
                                cursorShape: Qt.PointingHandCursor
                            }

                            objectName: "connectionSummaryStatus"
                            text: overviewScroll.progress.title || "Starting Home Assistant Connection"
                            color: overviewScroll.progress.error ? Theme.error : Theme.ink
                            font.pixelSize: Theme.fontHeading
                            font.weight: Font.DemiBold
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }

                        HintLabel {
                            text: overviewScroll.progress.detail || ""
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                    }

                    QuietButton {
                        text: "Connection Settings"
                        ToolTip.text: "Configure Home Assistant API or MQTT"
                        onClicked: overviewScroll.connectionRequested()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS
                    ConnectionHeartbeats {
                        id: connectionHeartbeat
                        objectName: "overviewHeartbeat"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        controller: overviewScroll.controller
                        active: overviewScroll.active
                    }
                    HintLabel {
                        id: responseLabel
                        objectName: "haLastCommunication"
                        Layout.preferredWidth: Math.min(responseLabelSize.advanceWidth, 320, overviewScroll.availableWidth / 3)
                        Layout.minimumWidth: 0
                        horizontalAlignment: Text.AlignRight
                        wrapMode: Text.WordWrap
                        // Reserve a compact, font-sized column that does not change
                        // with the counter or connection state. Longer messages wrap.
                        TextMetrics {
                            id: responseLabelSize
                            font: responseLabel.font
                            text: "Last Home Assistant response: 999 s ago"
                        }
                        text: controller.lastHaCommunication > 0
                            ? "Last Home Assistant response: " + overviewScroll.secondsSince(controller.lastHaCommunication) + " s ago"
                                + (overviewScroll.transportConnected ? "" : " · Disconnected")
                            : controller.haFeedbackSupported ? "Waiting for the first Home Assistant response…"
                            : overviewScroll.transportConnected ? "Update the WHAT integration to enable Home Assistant response timing."
                            : "No Home Assistant response received this session."
                        ToolTip.text: "ECG beats mark confirmed replies from the Home Assistant integration. Checks run every 5 seconds per installation. The Heartbeat setting under Local Pairing and Delivery controls outgoing keepalive updates, not these checks: sending data alone does not confirm that Home Assistant is responding."
                            + "\n\nThe counter shows the time since the most recent reply from any installation, not network latency. Each coloured trace belongs to the matching installation below and overwrites only its own history."
                            + "\n\nCurrent publishing intervals: Normal " + (status.publishingNormalMs || 1000)
                            + " ms (" + (status.publishingNormalMs || 1000) / 1000 + " s); Fast "
                            + (status.publishingHighMs || 100) + " ms (" + (status.publishingHighMs || 100) / 1000 + " s)."
                            + " Readings can be sent at these rates while this counter shows 1-3 seconds ago. Change the rates in Settings."
                            + "\n\nThe graph shows about " + Math.round(connectionHeartbeat.historyMs / 1000)
                            + " seconds per sweep. A wider window fits more history while beat spacing and sweep speed stay the same."
                            + " Beats mark confirmed replies; missing replies draw a flat line. Each trace keeps its own colour and dims after 12 seconds without a reply."
                        ToolTip.visible: commHover.hovered
                        HoverHandler { id: commHover }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: Theme.outline
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXl

                    ColumnLayout {
                        spacing: 3

                        HintLabel {
                            text: "SHARING"
                            font.pixelSize: Theme.fontLabel
                            font.letterSpacing: 1
                        }

                        Label {
                            text: overviewScroll.status.selected + " / " + overviewScroll.status.total + " sensors shared"
                            color: Theme.ink
                            font.weight: Font.DemiBold
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    QuietButton {
                        ToolTip.text: "Open the sensor browser to choose which readings can be shared over local or cloud connections."
                        text: "Choose Sensors"
                        onClicked: overviewScroll.navigationRequested(1)
                    }
                }

                Repeater {
                    model: controller.installationConfiguration
                    delegate: RowLayout {
                        required property var modelData
                        readonly property var connection: controller.installationStatus[modelData.id] || ({})
                        Layout.fillWidth: true
                        spacing: Theme.spaceM
                        Rectangle {
                            width: 6
                            height: 6
                            color: modelData.colour
                            opacity: connection.connected ? 1 : 0.45
                        }
                        Label {
                            text: modelData.name
                            color: Theme.ink
                            Layout.preferredWidth: 180
                            elide: Text.ElideRight
                        }
                        HintLabel {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: ((connection.activeType || modelData.type) === "mqtt" ? "MQTT" : "Home Assistant API")
                                + " · " + (connection.status || "Waiting for Home Assistant")
                                + (connection.connected && connection.lastCommunication > 0
                                    ? " · Last Response " + overviewScroll.secondsSince(connection.lastCommunication) + " s Ago" : "")
                        }
                    }
                }

                HintLabel {
                    objectName: "connectionAttemptTiming"
                    text: overviewScroll.attemptTiming
                    visible: text.length > 0
                    color: Theme.accent
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
        }

        Item {
            id: pairingTop
            Layout.fillWidth: true
            visible: overviewScroll.pairingFirst
            implicitHeight: pairingPanel.implicitHeight
        }

        RowLayout {
            objectName: "overviewModulesHeading"
            Layout.fillWidth: true
            Layout.topMargin: 6

            Label {
                text: "Modules"
                color: Theme.ink
                font.pixelSize: 17
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }

            QuietButton {
                ToolTip.text: "Choose which Windows and third-party sources WHAT discovers, and configure their permissions."
                text: "Manage Modules"
                onClicked: overviewScroll.navigationRequested(5)
            }
        }

        GridLayout {
            id: overviewModules
            Layout.fillWidth: true
            columns: 1
            rowSpacing: 0
            columnSpacing: 0

            Repeater {
                // Health and order changes update existing rows rather than recreating them.
                model: [
                    { key: "beacn", label: "BEACN", detail: "Audio mixing and app controls" },
                    { key: "wallpaper", label: "Wallpaper Engine", detail: "Wallpapers, playlists and profiles" },
                    { key: "windows", label: "Windows", detail: "System readings and enabled Windows controls" },
                    { key: "custom", label: "Custom", detail: "Configured sensors and saved commands" },
                    { key: "coretemp", label: "Core Temp", detail: "Processor temperatures and readings" },
                    { key: "hwinfo", label: "HWiNFO", detail: "Hardware readings from shared memory" },
                    { key: "icue", label: "Corsair iCUE", detail: "RGB SDK and Automation connections" },
                    { key: "nvidia", label: "NVIDIA", detail: "GPU readings from the NVIDIA driver" },
                    { key: "haweb", label: "Home Assistant Web", detail: "Dashboard, desktop and overlays" }
                ]

                delegate: ColumnLayout {
                    id: moduleRow

                    required property var modelData
                    readonly property string moduleKey: modelData.key
                    readonly property string heading: modelData.label
                    readonly property string issue: ModuleStatus.issue(controller, modelData.key)
                    readonly property string connectionText: ModuleStatus.connectionText(controller, modelData.key)
                    readonly property color statusColour: ModuleStatus.statusColour(controller, modelData.key, Theme)
                    objectName: "overview-" + modelData.key
                    Layout.fillWidth: true
                    Layout.row: overviewReorder.order.indexOf(moduleKey)
                    spacing: 0

                    Rectangle {
                        id: moduleHeader

                        Layout.fillWidth: true
                        implicitHeight: 68
                        color: overviewModuleHover.hovered || activeFocus ? Theme.hover : "transparent"
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: "Configure " + modelData.label
                        Accessible.onPressAction: overviewScroll.moduleRequested(modelData.key)
                        Keys.onReturnPressed: overviewScroll.moduleRequested(modelData.key)
                        Keys.onSpacePressed: overviewScroll.moduleRequested(modelData.key)

                        HoverHandler {
                            id: overviewModuleHover
                            cursorShape: Qt.PointingHandCursor
                        }

                        TapHandler {
                            enabled: overviewReorder.reorderCard === null
                            onTapped: overviewScroll.moduleRequested(modelData.key)
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: Theme.spaceM

                            ModuleReorderGrip {
                                objectName: "overview-" + moduleRow.moduleKey + "ReorderGrip"
                                moduleItem: moduleRow
                                reorderHost: overviewReorder
                            }

                            Image {
                                Layout.preferredWidth: 28
                                Layout.preferredHeight: 28
                                source: modelData.key === "custom" || modelData.key === "haweb"
                                    ? "qrc:/assets/Logo150.png"
                                    : "qrc:/assets/brands/" + (modelData.key === "wallpaper" ? "wallpaper.png"
                                        : modelData.key === "icue" ? "corsair.svg"
                                        : modelData.key === "hwinfo" ? "hwinfo.png" : modelData.key + ".svg")
                                sourceSize.width: 84
                                sourceSize.height: 84
                                fillMode: Image.PreserveAspectFit
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 3

                                Label {
                                    text: modelData.label
                                    color: Theme.ink
                                    font.weight: Font.DemiBold
                                }

                                HintLabel {
                                    text: modelData.detail
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                            }

                            Rectangle {
                                width: 6
                                height: 6
                                color: moduleRow.statusColour
                            }

                            HintLabel {
                                text: moduleRow.connectionText
                                color: moduleRow.statusColour
                                Layout.preferredWidth: 130
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                    }

                    WarningBanner {
                        objectName: "overview-" + modelData.key + "Warning"
                        visible: moduleRow.issue.length > 0
                        Layout.topMargin: visible ? Theme.spaceM : 0
                        Layout.bottomMargin: visible ? Theme.spaceM : 0
                        title: modelData.label + " Needs Attention"
                        detail: moduleRow.issue
                        actionText: "Module Settings"
                        inlineAction: true
                        actionHelp: "Open " + modelData.label + " settings to resolve this issue."
                        onActionTriggered: overviewScroll.moduleRequested(modelData.key)
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.separator
                    }
                }
            }
        }

        RowLayout {
            objectName: "overviewServiceControls"
            Layout.fillWidth: true
            Layout.topMargin: 4

            HintLabel {
                text: "Local WebSocket Server"
                Layout.fillWidth: true
            }

            ServiceControls {
                controller: overviewScroll.controller
                transportConnected: overviewScroll.transportConnected
                onNotification: message => overviewScroll.notification(message)
            }
        }

        Item {
            id: pairingBottom
            Layout.fillWidth: true
            visible: !overviewScroll.pairingFirst
            implicitHeight: pairingPanel.implicitHeight
        }

        Rectangle {
            id: pairingPanel

            // Move the existing panel so reconnecting keeps the chosen installation
            // and any open pairing details, rather than creating another form.
            parent: overviewScroll.pairingFirst ? pairingTop : pairingBottom
            width: parent.width
            objectName: "overviewPairingPanel"
            property ExpansionBlur expansionBlur: ExpansionBlur {
                target: pairingBody
                progress: pairingReveal.expansion
            }
            // Connection changes must not open or close the user's pairing details.
            property bool expanded: false
            property ExpansionScroll expansionScroll: ExpansionScroll {
                targetItem: pairingPanel
                expanded: pairingPanel.expanded
            }
            implicitHeight: pairingHeader.height + pairingReveal.height
            color: Theme.inset

            AbstractButton {
                id: pairingHeader

                objectName: "overviewPairingToggle"
                width: parent.width
                height: 58
                Accessible.name: "Pairing details, " + (pairingPanel.expanded ? "collapse" : "expand")

                onClicked: {
                    pairingPanel.expanded = !pairingPanel.expanded;
                    if (pairingPanel.expanded)
                        pairingPanel.expansionScroll.follow();
                }

                background: Rectangle {
                    color: pairingPanel.expanded ? Theme.expandedHeader : "transparent"
                }

                contentItem: RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    spacing: Theme.spaceM

                    Image {
                        source: "qrc:/assets/chevron.svg"
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 20
                        rotation: pairingPanel.expanded ? 0 : -90

                        Behavior on rotation {
                            NumberAnimation {
                                duration: Theme.animationsEnabled ? Theme.timings.panels : 0
                            }
                        }
                    }

                    Label {
                        text: "Pair This PC"
                        color: Theme.ink
                        font.weight: Font.DemiBold
                        Layout.fillWidth: true
                    }

                    HintLabel {
                        text: overviewScroll.status.name
                    }
                }
            }

            Item {
                id: pairingReveal

                objectName: "pairingReveal"
                y: pairingHeader.height
                width: parent.width
                clip: true
                enabled: pairingPanel.expanded
                property real expansion: pairingPanel.expanded ? 1 : 0
                height: expansion * (pairingBody.implicitHeight + 32)

                Behavior on expansion {
                    NumberAnimation {
                        id: pairingSlide

                        duration: Theme.animationsEnabled ? Theme.timings.panels : 0
                        easing.type: Easing.OutCubic
                    }
                }

                Connections {
                    target: Theme

                    function onAnimationsEnabledChanged() {
                        if (!Theme.animationsEnabled)
                            pairingSlide.complete();
                    }
                }

                InstallationPairing {
                    id: pairingBody

                    y: 16
                    x: 20
                    width: parent.width - 40
                    controller: overviewScroll.controller
                    backdropSource: overviewScroll.backdropSource
                    onRenewCertificateRequested: overviewScroll.renewCertificateRequested()
                    onNotification: message => overviewScroll.notification(message)
                }
            }
        }
    }
}
