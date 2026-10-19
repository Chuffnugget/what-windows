import QtQuick
import QtQuick.Controls.Basic
// Attached tooltips load this module dynamically, so deployment must include it.
import QtQuick.Controls as Controls
import QtQuick.Layouts
import "controls"
import "components"
import "pages"
import "dialogs"
import "custom"

ApplicationWindow {
    id: window

    Binding {
        target: Theme
        property: "name"
        value: controller.uiTheme
    }

    readonly property var widgetColours: Theme.widgetColours
    function syncWidgetTheme() {
        controller.applyWidgetTheme(widgetColours);
    }
    onWidgetColoursChanged: Qt.callLater(syncWidgetTheme)

    Binding {
        target: Theme
        property: "animationsEnabled"
        value: window.animationsEnabled
    }

    Binding {
        target: Theme
        property: "timings"
        value: window.uiTimings
    }

    Binding {
        target: Theme
        property: "transitionStyle"
        value: window.status.transitionStyle || "refraction"
    }

    Binding {
        target: Theme
        property: "transitionMode"
        value: window.status.transitionMode ?? 7
    }

    Binding {
        target: Theme
        property: "shaderEffectsSupported"
        value: window.contentItem.GraphicsInfo.api !== GraphicsInfo.Software
    }

    Binding {
        target: Theme
        property: "refractionStrength"
        value: window.status.refractionStrength ?? 100
    }

    required property var updater
    required property var controller
    property var status: controller.info
    readonly property var uiTimings: controller.uiTimings
    readonly property bool animationsEnabled: status.animationsEnabled !== false
    readonly property bool portable: status.packaged !== true

    function openPortableSettings() {
        previewPage = 2;
        settingsPage.openSection("portable");
    }

    // Page order is shared with NavigationBar and the C++ UI tests.
    property int previewPage: 0
    // Native WebView2 content must stay hidden until every modal prompt closes.
    readonly property bool webModalOpen: quitDialog.blocksWebView
        || entityCleanupDialog.blocksWebView
        || updateNotice.blocksWebView
        || clearWebDataDialog.blocksWebView
        || configurationImport.blocksWebView
        || timingDialog.blocksWebView
        || updateDialog.blocksWebView
        || releaseBrowser.blocksWebView
        || certificateDialog.blocksWebView
        || wallpaperControls.blocksWebView
        || setupDialog.blocksWebView
        || reportDialog.blocksWebView
        || rotateDialog.blocksWebView
        || overviewPage.blocksWebView

    function openModule(key) {
        previewPage = 5;
        modulesPage.openModule(key);
    }

    function applyFilters() {
        sensorsPage.applyFilters();
    }

    Component.onCompleted: {
        syncWidgetTheme();
        controller.setBrowserVisible(browserPresented);
    }
    // Hidden and minimised pages do not need browser-model refreshes.
    readonly property bool browserPresented: visible && visibility !== Window.Minimized && previewPage === 1
    onBrowserPresentedChanged: controller.setBrowserVisible(browserPresented)
    property string feedback: ""
    readonly property bool mqttActive: status.running && (status.connectionTls || "").indexOf("MQTT") >= 0
    readonly property bool apiActive: status.running
        && (status.connectionTls || "").split(" + ").some(part => part.indexOf("TLS") === 0)
    readonly property bool transportConnected: apiActive || mqttActive

    function openConnectionSettings() {
        previewPage = 2;
        settingsPage.openSection("connection");
    }

    function openCertificateRenewal() {
        certificateDialog.renewed = false;
        certificateDialog.failure = "";
        certificateDialog.open();
    }

    DisplayRecoveryPopup {
        controller: window.controller
    }

    function openUpdateSettings() {
        previewPage = 2;
        settingsPage.openSection("updates");
    }

    Connections {
        target: controller

        function onLocalIcueFinished(ok, message) {
            if (!ok)
                window.notify(message);
        }

        function onCustomActionFinished(ok, message) {
            window.notify(message);
        }

        function onCertificateRenewalFinished(ok, message) {
            certificateDialog.renewed = ok;
            certificateDialog.failure = ok ? "" : message;
            certificateDialog.open();
        }
    }

    readonly property color ink: Theme.ink
    readonly property color muted: Theme.muted
    readonly property color accent: Theme.accent
    visible: false
    // Keep the custom frame usable on small desktops and high-DPI screens.
    readonly property int availableWindowWidth: Math.max(1, Math.min(Screen.width, Screen.desktopAvailableWidth))
    readonly property int availableWindowHeight: Math.max(1, Math.min(Screen.height, Screen.desktopAvailableHeight))
    width: Math.min(1160, availableWindowWidth)
    height: Math.min(884, availableWindowHeight)
    minimumWidth: Math.min(1000, availableWindowWidth)
    minimumHeight: Math.min(824, availableWindowHeight)
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint | Qt.WindowCloseButtonHint

    title: "WHAT · Windows Home Assistant Toolkit"
    color: Theme.canvas
    font.family: "Segoe UI"
    font.pixelSize: Theme.fontBody
    palette.text: ink
    palette.windowText: ink
    palette.buttonText: ink
    palette.window: Theme.inset
    palette.light: Theme.outline
    palette.mid: Theme.outline
    palette.dark: Theme.field
    palette.base: Theme.field
    palette.button: Theme.button
    palette.highlight: accent
    palette.placeholderText: muted
    // Qt can briefly report Windowed while minimising fullscreen. Save the settled
    // state so minimising or hiding preserves the last usable window mode.
    onVisibilityChanged: function (visibility) {
        Qt.callLater(rememberWindowMode);
    }

    function rememberWindowMode() {
        if (visibility === Window.Maximized)
            controller.setWindowMode("maximised");
        else if (visibility === Window.FullScreen)
            controller.setWindowMode("fullscreen");
        else if (visibility === Window.Windowed)
            controller.setWindowMode("normal");
    }

    onClosing: function (event) {
        event.accepted = false;
        hide();
    }

    signal windowQuitReady

    function performWindowAction(action) {
        if (action === "hide")
            hide();
        else if (action === "minimise")
            showMinimized();
        else if (action === "maximise")
            showMaximized();
        else if (action === "restore")
            showNormal();
        else if (action === "quit")
            windowQuitReady();
    }

    function toggleMaximised() {
        performWindowAction((visibility === Window.Maximized || visibility === Window.FullScreen)
            ? "restore"
            : "maximise");
    }

    header: Rectangle {
        id: titleBar

        objectName: "customTitleBar"
        height: 44
        color: window.active ? Theme.sidebar : Theme.surface

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.separator
        }

        MouseArea {
            anchors.fill: parent
            anchors.rightMargin: 144
            acceptedButtons: Qt.LeftButton
            onPressed: window.startSystemMove()
            onDoubleClicked: window.toggleMaximised()
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceM

            AnimatedLogo {
                objectName: "titleBarLogo"
                width: 20
                height: 24
                animationsEnabled: window.animationsEnabled
                animationDuration: window.uiTimings.logo
                pauseDuration: window.uiTimings.logoPause
            }

            Label {
                text: "WHAT"
                color: window.active ? window.ink : window.muted
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                height: 20
                verticalAlignment: Text.AlignVCenter
            }

            Label {
                text: "Windows Home Assistant Toolkit"
                color: window.muted
                font.pixelSize: Theme.fontSmall
                height: 20
                verticalAlignment: Text.AlignVCenter
            }
        }

        Row {
            anchors.right: parent.right
            height: parent.height
            spacing: 0

            TitleButton {
                objectName: "windowMinimise"
                glyph: "minimise"
                Accessible.name: "Minimise"
                onClicked: window.performWindowAction("minimise")
            }

            TitleButton {
                objectName: "windowMaximise"
                glyph: (window.visibility === Window.Maximized || window.visibility === Window.FullScreen)
                    ? "restore"
                    : "maximise"
                Accessible.name: (window.visibility === Window.Maximized || window.visibility === Window.FullScreen)
                    ? "Restore window"
                    : "Maximise"
                onClicked: window.toggleMaximised()
            }

            TitleButton {
                objectName: "windowClose"
                glyph: "close"
                Accessible.name: "Close to system tray"
                onClicked: window.close()
            }
        }
    }

    component TitleButton: AbstractButton {
        id: titleControl

        required property string glyph
        width: 48
        height: 44
        hoverEnabled: true
        ToolTip.visible: hovered
        ToolTip.text: Accessible.name

        background: Rectangle {
            color: titleControl.down
                ? (titleControl.glyph === "close" ? Theme.closePressed : Theme.pressed)
                : titleControl.hovered
                ? (titleControl.glyph === "close" ? Theme.closeHover : Theme.hover)
                : "transparent"
            border.width: titleControl.visualFocus ? 1 : 0
            border.color: window.accent
        }

        contentItem: Item {
            Rectangle {
                visible: titleControl.glyph === "minimise"
                width: 11
                height: 1
                color: window.ink
                anchors.centerIn: parent
            }

            Rectangle {
                visible: titleControl.glyph === "maximise"
                width: 10
                height: 10
                color: "transparent"
                border.color: window.ink
                anchors.centerIn: parent
            }

            Item {
                visible: titleControl.glyph === "restore"
                width: 13
                height: 13
                anchors.centerIn: parent

                Rectangle {
                    x: 3
                    width: 9
                    height: 9
                    color: "transparent"
                    border.color: window.ink
                }

                Rectangle {
                    y: 3
                    width: 9
                    height: 9
                    color: titleControl.hovered ? Theme.hover : Theme.sidebar
                    border.color: window.ink
                }
            }

            Item {
                visible: titleControl.glyph === "close"
                width: 12
                height: 12
                anchors.centerIn: parent

                Rectangle {
                    width: 14
                    height: 1
                    color: window.ink
                    rotation: 45
                    anchors.centerIn: parent
                }

                Rectangle {
                    width: 14
                    height: 1
                    color: window.ink
                    rotation: -45
                    anchors.centerIn: parent
                }
            }
        }
    }

    // Qt owns the interactive resize operation, retaining platform sizing behavior.
    Repeater {
        parent: window.contentItem.parent
        model: [Qt.TopEdge,
            Qt.BottomEdge,
            Qt.LeftEdge,
            Qt.RightEdge,
            Qt.TopEdge | Qt.LeftEdge,
            Qt.TopEdge | Qt.RightEdge,
            Qt.BottomEdge | Qt.LeftEdge,
            Qt.BottomEdge | Qt.RightEdge]

        delegate: MouseArea {
            required property int modelData
            objectName: "windowResizeHandle_" + modelData
            z: 1000
            visible: window.visibility !== Window.Maximized && window.visibility !== Window.FullScreen
            width: (modelData & Qt.LeftEdge) || (modelData & Qt.RightEdge) ? 6 : parent.width - 12
            height: (modelData & Qt.TopEdge) || (modelData & Qt.BottomEdge) ? 6 : parent.height - 12
            x: (modelData & Qt.RightEdge) ? parent.width - width : (modelData & Qt.LeftEdge) ? 0 : 6
            y: (modelData & Qt.BottomEdge) ? parent.height - height : (modelData & Qt.TopEdge) ? 0 : 6
            cursorShape: modelData === Qt.TopEdge
                || modelData === Qt.BottomEdge
                ? Qt.SizeVerCursor
                : modelData === Qt.LeftEdge
                || modelData === Qt.RightEdge
                ? Qt.SizeHorCursor
                : modelData === (Qt.TopEdge | Qt.LeftEdge)
                || modelData === (Qt.BottomEdge | Qt.RightEdge)
                ? Qt.SizeFDiagCursor
                : Qt.SizeBDiagCursor
            onPressed: window.startSystemResize(modelData)
        }
    }

    Connections {
        target: updater

        // A background update check must not replace the just-installed confirmation.
        function onUpdateAvailable(version) {
            if (updateNotice.completed && updateNotice.visible)
                return;
            updateNotice.completed = false;
            updateNotice.version = version;
            updateNotice.open();
        }

        function onUpdateCompleted(details) {
            updateNotice.completed = true;
            updateNotice.previousVersion = details.previous;
            updateNotice.version = details.current;
            updateNotice.notes = details.notes;
            updateNotice.open();
        }

    }

    readonly property bool quitConfirmationVisible: quitDialog.visible

    function openQuitConfirmation() {
        if (!quitDialog.visible)
            quitDialog.open();
    }

    QuitDialog {
        id: quitDialog

        backdropSource: applicationSurface
        onQuitRequested: window.windowQuitReady()
        onHideRequested: window.hide()
    }

    EntityCleanupDialog {
        id: entityCleanupDialog

        backdropSource: applicationSurface
        controller: window.controller
    }

    function showAlreadyRunning() {
        notify("WHAT is already running. Your existing window has been restored.", 20000);
    }

    UpdateNoticeDialog {
        id: updateNotice

        backdropSource: applicationSurface
        updater: window.updater
        onUpdateSettingsRequested: window.openUpdateSettings()
    }

    HomeAssistantWebHost {
        web: controller.homeAssistantWeb
        pageSlot: homeAssistantWebPage.browserSlot
        pageActive: window.previewPage === 9 && window.visible
            && window.visibility !== Window.Minimized && !mainPageStack.moving
        blocked: window.webModalOpen
        onSettingsRequested: {
            window.openModule("haweb");
            if (!window.visible || window.visibility === Window.Minimized)
                window.show();
            window.raise();
            window.requestActivate();
        }
    }

    Connections {
        target: controller.homeAssistantWeb
        function onPageRequested() {
            window.previewPage = 9;
            if (!window.visible || window.visibility === Window.Minimized)
                window.show();
            window.raise();
            window.requestActivate();
        }
        function onNotice(message) { window.notify(message); }
    }

    ClearWebDataDialog {
        id: clearWebDataDialog

        backdropSource: applicationSurface
        controller: window.controller
    }

    property string configurationMessage: ""

    Connections {
        target: controller

        function onConfigurationResult(message) {
            window.configurationMessage = message;
        }

        function onConfigurationPreview(text) {
            configurationImport.preview(text);
        }
    }

    ConfigurationImportDialog {
        id: configurationImport

        backdropSource: applicationSurface
        controller: window.controller
    }

    AnimationTimingsDialog {
        id: timingDialog

        backdropSource: applicationSurface
        controller: window.controller
    }

    UpdateConfirmationDialog {
        id: updateDialog

        backdropSource: applicationSurface
        updater: window.updater
    }

    ReleaseBrowserDialog {
        id: releaseBrowser

        backdropSource: applicationSurface
        updater: window.updater
        onInstallRequested: updateDialog.open()
    }

    CertificateDialog {
        id: certificateDialog

        backdropSource: applicationSurface
        controller: window.controller
        onNotification: message => window.notify(message)
    }

    WallpaperControlsDialog {
        id: wallpaperControls

        backdropSource: applicationSurface
        controller: window.controller
    }

    function notify(message, duration) {
        feedback = message;
        toastTimer.interval = duration === undefined ? window.uiTimings.toast : duration;
        toastTimer.restart();
    }

    function showSetup() {
        setupDialog.open();
    }

    Timer {
        id: toastTimer

        objectName: "noticeTimer"
        interval: window.uiTimings.toast
        onTriggered: window.feedback = ""
    }

    DefinitionDialog {
        id: definitionDialog

        manager: window.controller.customSources
        backdropSource: applicationSurface
    }

    SensorLibrary {
        id: sensorLibrary

        manager: window.controller.customSources
        backdropSource: applicationSurface
        onEditRequested: definition => definitionDialog.edit(definition)
    }

    // Title, warnings and content share the same page transform.
    component AppPage: Page {
        id: page

        required property string subtitle
        // Only Overview and Modules show the HWiNFO Free reminder; on every page it was noise.
        property bool hwinfoWarning: false
        property real warningRightMargin: Theme.scrollGutter
        // PageStack owns the size; wrapped content must not feed it back into
        // Page's implicit-size calculation through the header layout.
        implicitWidth: 0
        implicitHeight: 0
        padding: 0
        spacing: 18
        background: null

        header: ColumnLayout {
            spacing: 18

            PageHeading {
                objectName: "pageHeading"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                title: page.title
                subtitle: page.subtitle
            }

            WarningBanner {
                objectName: "hwinfoLicenseWarning"
                Layout.rightMargin: page.warningRightMargin
                visible: page.hwinfoWarning && window.status.hwinfoEnabled && window.status.license !== "pro"
                compact: true
                title: "HWiNFO Free: 12-Hour Sharing Limit"
                detail: "Re-enable shared memory in HWiNFO when it expires. Already using Pro? Select your edition in Modules."
                actionText: "HWiNFO Settings"
                actionHelp: "Open the HWiNFO module for edition settings, setup and licence details."
                inlineAction: true
                onActionTriggered: window.openModule("hwinfo")
            }
        }
    }

    Item {
        id: applicationSurface

        anchors.fill: parent

        NavigationBar {
            id: sidebar
            expandedWidth: controller.sidebarWidth
            collapsed: controller.sidebarCollapsed
            maximumWidth: Math.max(180, Math.min(360, window.width - 790))
            onLayoutEdited: (width, collapsed) => controller.setSidebarLayout(width, collapsed)

            currentPage: window.previewPage
            healthy: window.status.windowsHealthy
            pcName: window.status.name
            version: window.status.version
            availableUpdate: updater.info.downgrade ? "" : (updater.info.available || "")
            onNavigationRequested: page => {
                if (page === 9) {
                    controller.homeAssistantWeb.open();
                    return;
                }
                window.previewPage = page;
                if (page === 4)
                    controller.ensureSpecs();
            }

            onUpdateRequested: window.openUpdateSettings()
        }

        ColumnLayout {
            objectName: "mainContentLayout"
            anchors.left: sidebar.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 28
            anchors.topMargin: Theme.spaceL
            anchors.bottomMargin: Theme.spaceS
            spacing: Theme.spaceS

            PageStack {
                id: mainPageStack
                objectName: "pageStack"
                pageOrder: sidebar.pageOrder
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: parent.width
                Layout.minimumHeight: 0
                currentIndex: window.previewPage

                AppPage {
                    title: "Overview"
                    subtitle: "Windows Home Assistant Toolkit"
                    hwinfoWarning: true
                    warningRightMargin: Theme.scrollGutter

                    contentItem: OverviewPage {
                        id: overviewPage
                        backdropSource: applicationSurface
                        updater: window.updater

                        active: window.previewPage === 0 && window.visible && window.visibility !== Window.Minimized
                        portable: window.portable
                        controller: window.controller
                        onConnectionRequested: window.openConnectionSettings()
                        onRenewCertificateRequested: window.openCertificateRenewal()
                        onPortableRequested: window.openPortableSettings()
                        onModuleRequested: key => window.openModule(key)
                        onNavigationRequested: page => window.previewPage = page
                        onNotification: message => window.notify(message)
                    }
                }

                AppPage {
                    title: "Sensors"
                    subtitle: "Choose exactly which readings Home Assistant receives."

                    contentItem: SensorsPage {
                        id: sensorsPage

                        controller: window.controller
                        backdropSource: applicationSurface
                        onCustomSensorRequested: definitionDialog.edit(controller.customSources.newDefinition("sensor"))
                        onCustomLibraryRequested: sensorLibrary.open()
                        onCustomEditRequested: identifier => {
                            const definition = controller.customSources.definitionForReading(identifier);
                            if (definition.id)
                                definitionDialog.edit(definition);
                        }
                        onIconRequested: (sensorId, sensorName, appIcon, replaceDefault) => {
                            window.previewPage = 7;
                            taskbarPage.openForSensor(sensorId, sensorName, appIcon, replaceDefault);
                        }
                    }
                }

                AppPage {
                    title: "Settings"
                    subtitle: "Make monitoring work the way you want."

                    contentItem: SettingsPage {
                        backdropSource: applicationSurface
                        id: settingsPage

                        portable: window.portable
                        controller: window.controller
                        updater: window.updater
                        configurationMessage: window.configurationMessage
                        onNotification: message => window.notify(message)
                        onEntityCleanupRequested: entityCleanupDialog.open()
                        onTimingRequested: timingDialog.open()
                        onInstallUpdateRequested: updateDialog.open()
                        onReleaseBrowserRequested: releaseBrowser.open()
                        onRotateTokenRequested: rotateDialog.open()
                        onRenewCertificateRequested: window.openCertificateRenewal()
                    }
                }

                AppPage {
                    title: "Diagnostics"
                    subtitle: "Connection activity and source health, without your secrets."

                    contentItem: DiagnosticsPage {
                        id: diagnosticsPage

                        active: window.previewPage === 3 && window.visible && window.visibility !== Window.Minimized
                        controller: window.controller
                        onNotification: message => window.notify(message)
                        onReportRequested: reportDialog.open()
                    }
                }

                AppPage {
                    title: "Specs"
                    subtitle: "Your computer specifications, ready to copy and share."

                    contentItem: SpecsPage {
                        id: specsPage

                        controller: window.controller
                        onNotification: message => window.notify(message)
                    }
                }

                AppPage {
                    title: "Modules"
                    subtitle: "Choose your data providers and control what they collect."
                    hwinfoWarning: true

                    contentItem: ModulesPage {
                        id: modulesPage

                        portable: window.portable
                        controller: window.controller
                        onHwinfoSetupRequested: setupDialog.open()
                        onPortableRequested: window.openPortableSettings()
                        onCommandsRequested: window.previewPage = 8
                        onClearWebDataRequested: clearWebDataDialog.open()
                        onCustomSensorsRequested: {
                            window.previewPage = 1;
                            sensorLibrary.open();
                        }

                        onWallpaperRequested: {
                            window.previewPage = 1;
                            wallpaperControls.open();
                        }
                    }
                }

                AppPage {
                    title: "About WHAT"
                    subtitle: "Made by Chuffnugget. Free, open source and supported by you."

                    contentItem: AboutPage {
                        id: aboutPage

                        logoDuration: window.uiTimings.logo
                        logoPause: window.uiTimings.logoPause
                        hoverDuration: window.uiTimings.hover
                        animationsEnabled: window.animationsEnabled
                        version: window.status.version
                    }
                }

                AppPage {
                    title: "Taskbar"
                    subtitle: "System tray and app icons, just for this PC."

                    contentItem: TaskbarPage {
                        id: taskbarPage

                        active: window.previewPage === 7 && window.visible && window.visibility !== Window.Minimized
                        manager: controller.trayManager
                        colourPalette: controller.colourPalette
                        backdropSource: applicationSurface
                        animationsEnabled: window.animationsEnabled
                    }
                }

                AppPage {
                    title: "Commands"
                    subtitle: "Commands defined on this PC, with explicit Home Assistant permissions."

                    contentItem: CommandsPage {
                        controller: window.controller
                        backdropSource: applicationSurface
                        onEditRequested: (definition, saving) => definitionDialog.edit(definition, saving)
                    }
                }
                AppPage {
                    title: "Home Assistant Web"
                    subtitle: "Your Home Assistant dashboard, inside WHAT."

                    contentItem: HomeAssistantWebPage {
                        id: homeAssistantWebPage
                        web: controller.homeAssistantWeb
                        onSettingsRequested: window.openModule("haweb")
                    }
                }

            }

            Item {
                id: footerPanel
                objectName: "connectionFooter"
                Layout.fillWidth: true
                implicitHeight: 16

                RowLayout {
                    anchors.fill: parent

                    HintLabel {
                        objectName: "connectionModeFooter"
                        text: (controller.installationConfiguration.some(entry => entry.enabled && (entry.type === "api" ? !!entry.url : entry.mqtt.scope === "cloud"))
                            ? "LOCAL + CLOUD ENABLED"
                            : "LOCAL ONLY") + (window.status.connectionTransport === "cloud"
                            ? " · CLOUD CONNECTED"
                            : window.status.connectionTransport === "local"
                            ? " · LOCAL CONNECTED"
                            : window.status.connectionTransport === "hybrid"
                            ? " · LOCAL + CLOUD CONNECTED"
                            : "")
                        color: window.accent
                        font.pixelSize: Theme.fontLabel
                        font.letterSpacing: 1.2
                    }

                    HintLabel {
                        TapHandler {
                            onTapped: window.openConnectionSettings()
                        }

                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }

                        color: window.status.connectionProgress.error
                            ? Theme.error
                            : window.status.connectionProgress.busy
                            ? window.accent
                            : window.muted
                        objectName: "connectionTlsFooter"
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Link
                        Accessible.name: "Connection Settings: " + text
                        Keys.onReturnPressed: window.openConnectionSettings()
                        Keys.onEnterPressed: window.openConnectionSettings()
                        Keys.onSpacePressed: window.openConnectionSettings()
                        font.underline: activeFocus
                        text: " /  " + (window.status.connectionTls
                            || (window.status.connectionProgress.busy || window.status.connectionProgress.error ? window.status.connectionProgress.title : "Server stopped · no Home Assistant connection"))
                        font.pixelSize: Theme.fontLabel
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    Label {
                        objectName: "developerCreditFooter"
                        text: "Developed by Chuffnugget"
                        color: creditHover.hovered ? Theme.accentHover : Theme.accent
                        font.pixelSize: Theme.fontLabel
                        font.underline: creditHover.hovered
                        Accessible.role: Accessible.Link
                        Accessible.name: text
                        activeFocusOnTab: true
                        Keys.onReturnPressed: Qt.openUrlExternally("https://github.com/Chuffnugget/what-windows")

                        HoverHandler {
                            id: creditHover

                            cursorShape: Qt.PointingHandCursor
                        }

                        TapHandler {
                            onTapped: Qt.openUrlExternally("https://github.com/Chuffnugget/what-windows")
                        }

                        ToolTip.visible: creditHover.hovered
                        ToolTip.text: "github.com/Chuffnugget/what-windows"
                    }
                }
            }
        }
    }

    Rectangle {
        id: feedbackToast

        property string shownText: ""
        property real reveal: window.feedback.length > 0 ? 1 : 0
        visible: reveal > 0
        property OpenCloseLayer refraction: OpenCloseLayer {
            target: feedbackToast
            progress: feedbackToast.reveal
        }
        Behavior on reveal {
            NumberAnimation {
                duration: Theme.openCloseEnabled
                    ? (window.feedback.length > 0 ? Theme.timings.dialogEnter : Theme.timings.dialogExit) : 0
            }
        }
        Connections {
            target: window
            function onFeedbackChanged() {
                if (window.feedback)
                    feedbackToast.shownText = window.feedback;
            }
        }
        // Sits over the status footer rather than the page, so it doesn't cover
        // buttons at the bottom of a page. Click to dismiss.
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 28
        anchors.bottomMargin: 6
        width: Math.min(440, window.width - 260)
        height: toastText.implicitHeight + 24

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                toastTimer.stop();
                window.feedback = "";
            }
        }

        color: Theme.selected
        border.color: Theme.outlineStrong
        z: 10

        Label {
            id: toastText

            anchors.fill: parent
            anchors.margins: 12
            text: feedbackToast.shownText
            wrapMode: Text.WordWrap
            color: window.ink
        }
    }

    HwinfoSetupDialog {
        id: setupDialog

        backdropSource: applicationSurface
        controller: window.controller
        showLicenseReminder: window.previewPage !== 6 && window.previewPage !== 7
    }

    DiagnosticReportDialog {
        id: reportDialog

        backdropSource: applicationSurface
        controller: window.controller
        onNotification: message => window.notify(message)
    }

    PairingTokenDialog {
        id: rotateDialog

        backdropSource: applicationSurface
        controller: window.controller
    }
}
