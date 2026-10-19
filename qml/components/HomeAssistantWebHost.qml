import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

Item {
    id: host
    objectName: "hawebHost"

    required property var web
    required property Item pageSlot
    property bool pageActive: false
    property bool blocked: false
    property bool transitioning: false
    readonly property bool pip: web.overlayVisible && web.overlayMode === "pip"
    readonly property bool detached: web.overlayVisible || web.desktopActive
    readonly property bool presented: web.enabled && web.url.length > 0 && (web.overlayVisible ? overlay.visible && !web.pipMinimised : web.desktopActive ? desktop.visible : pageActive && !blocked && !transitioning)
    property bool toolbarShown: true
    property bool pipToolbarCollapsed: false
    readonly property bool toolbarVisible: !web.options.autoHideToolbar || toolbarShown || !web.ready || web.loading
    readonly property bool pipToolbarVisible: web.pipMinimised || (toolbarVisible && !pipToolbarCollapsed)
    signal settingsRequested

    function revealToolbar() {
        pipToolbarCollapsed = false;
        toolbarShown = true;
        toolbarTimer.restart();
    }

    function toolbarHasFocus() {
        const view = web.overlayVisible ? overlay : web.desktopActive ? desktop : pageSlot.Window.window;
        let item = view ? view.activeFocusItem : null;
        while (item) {
            if (item === pipToolbar || item === pageToolbar)
                return true;
            item = item.parent;
        }
        return false;
    }

    onPresentedChanged: {
        web.setPresented(presented);
        if (presented)
            revealToolbar();
    }
    onBlockedChanged: {
        if (blocked)
            web.hideOverlay();
    }
    Component.onCompleted: {
        web.setOverlayWindow(overlay);
        web.setDesktopWindow(desktop);
        web.setPresented(presented);
    }
    Component.onDestruction: {
        web.setDesktopWindow(null);
        web.setOverlayWindow(null);
    }

    Timer {
        id: toolbarTimer
        interval: 2500
        repeat: true
        running: host.presented && host.web.options.autoHideToolbar && host.toolbarShown
        onTriggered: {
            if (!pipHover.hovered && !pageHover.hovered && !host.toolbarHasFocus())
                host.toolbarShown = false;
        }
    }

    Window {
        id: desktop
        objectName: "homeAssistantWebDesktop"
        title: "Home Assistant Desktop - WHAT"
        transientParent: null
        color: Theme.canvas
        flags: Qt.FramelessWindowHint | Qt.Tool
        onClosing: event => {
            event.accepted = false;
            host.settingsRequested();
        }
    }

    Window {
        id: overlay
        objectName: "homeAssistantWebOverlay"
        title: "Home Assistant Web - WHAT"
        // The hotkey must work while WHAT's main window is hidden or minimised.
        transientParent: null
        color: Theme.canvas
        flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
        onClosing: event => {
            event.accepted = false;
            host.web.hideOverlay();
        }

        Shortcut {
            sequence: "Escape"
            // Native browser focus handles Escape in the page first.
            enabled: host.web.overlayVisible && host.toolbarHasFocus()
            onActivated: host.web.hideOverlay()
        }

        // Native resizing handles DPI changes and Windows snapping. Keep the
        // grips outside the browser's HWND so the web page cannot eat the drag.
        Repeater {
            model: [Qt.TopEdge | Qt.LeftEdge, Qt.TopEdge, Qt.TopEdge | Qt.RightEdge,
                Qt.LeftEdge, Qt.RightEdge, Qt.BottomEdge | Qt.LeftEdge, Qt.BottomEdge,
                Qt.BottomEdge | Qt.RightEdge]
            delegate: MouseArea {
                required property int modelData
                readonly property bool edgeLeft: (modelData & Qt.LeftEdge) !== 0
                readonly property bool edgeRight: (modelData & Qt.RightEdge) !== 0
                readonly property bool edgeTop: (modelData & Qt.TopEdge) !== 0
                readonly property bool edgeBottom: (modelData & Qt.BottomEdge) !== 0
                width: edgeLeft || edgeRight ? 8 : overlay.width - 16
                height: edgeTop || edgeBottom ? 8 : overlay.height - 16
                x: edgeRight ? overlay.width - width : edgeLeft ? 0 : 8
                y: edgeBottom ? overlay.height - height : edgeTop ? 0 : 8
                z: 10
                enabled: host.pip && !host.web.pipMinimised
                cursorShape: (edgeLeft || edgeRight) && (edgeTop || edgeBottom) ? (edgeLeft === edgeTop ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor) : edgeLeft || edgeRight ? Qt.SizeHorCursor : Qt.SizeVerCursor
                onPressed: overlay.startSystemResize(modelData)
            }
        }
    }

    // Keep one WindowContainer. Reparenting it preserves the loaded page and
    // login when the dashboard moves between WHAT and the overlay.
    ColumnLayout {
        parent: host.web.overlayVisible ? overlay.contentItem : host.web.desktopActive ? desktop.contentItem : host.pageSlot
        anchors.fill: parent
        anchors.margins: host.pip ? 8 : host.detached ? Theme.spaceM : 0
        spacing: Theme.spaceS
        visible: host.web.enabled && host.web.url.length > 0 && (host.detached || host.pageActive)

        QuietButton {
            objectName: "hawebPipRevealToolbar"
            visible: host.pip && !host.pipToolbarVisible
            Layout.fillWidth: true
            Layout.preferredHeight: 12
            compact: true
            glyphSource: "qrc:/assets/chevron.svg"
            Accessible.name: "Show Picture-in-Picture toolbar"
            ToolTip.text: "Click to reveal the title bar and window controls."
            onClicked: host.revealToolbar()
        }

        ColumnLayout {
            id: pipToolbar
            objectName: "hawebPipToolbar"
            visible: host.pip && host.pipToolbarVisible
            Layout.fillWidth: true
            spacing: 4

            HoverHandler { id: pipHover }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceS

                AnimatedLogo {
                    objectName: "hawebPipLogo"
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 24
                    Layout.leftMargin: 4
                    animationsEnabled: Theme.animationsEnabled
                    animationDuration: Theme.timings.logo
                    pauseDuration: Theme.timings.logoPause
                    textHovered: pipTitleHover.hovered
                }

                HintLabel {
                    objectName: "hawebPipDragBar"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: Theme.compactHeight
                    text: "Home Assistant"
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    HoverHandler { id: pipTitleHover }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.SizeAllCursor
                        onPressed: overlay.startSystemMove()
                    }
                }

                QuietButton {
                    objectName: "hawebPipCollapseToolbar"
                    compact: true
                    text: "▴"
                    visible: !host.web.pipMinimised
                    Accessible.name: "Collapse Toolbar"
                    ToolTip.text: "Shrink the toolbar to a thin strip. Click the strip to reveal it again."
                    onClicked: host.pipToolbarCollapsed = true
                }

                QuietButton {
                    id: pipMinimiseButton
                    objectName: "hawebPipMinimise"
                    compact: true
                    text: host.web.pipMinimised ? "□" : "−"
                    Accessible.name: host.web.pipMinimised ? "Restore Picture-in-Picture" : "Minimise to Title Bar"
                    ToolTip.text: host.web.pipMinimised
                        ? "Restore the dashboard to its previous size."
                        : "Roll up the dashboard to just this title bar, keeping the browser session."
                    onClicked: host.web.togglePipMinimised()
                }

                QuietButton {
                    compact: true
                    text: "↗"
                    Accessible.name: "Fullscreen"
                    ToolTip.text: "Expand this dashboard to a fullscreen overlay."
                    onClicked: host.web.showOverlay("fullscreen")
                }

                QuietButton {
                    compact: true
                    text: "×"
                    Accessible.name: "Close Picture-in-Picture"
                    ToolTip.text: "Close picture-in-picture and return the dashboard to WHAT or the desktop. Escape also closes it."
                    onClicked: host.web.hideOverlay()
                }
            }

            // Keep every action visible even at the 360-pixel minimum width.
            // Rolling up the window leaves just the first title row.
            RowLayout {
                objectName: "hawebPipActions"
                Layout.fillWidth: true
                spacing: 0
                visible: !host.web.pipMinimised

                QuietButton {
                    compact: true
                    glyphSource: "qrc:/assets/chevron.svg"
                    glyphRotation: 90
                    Accessible.name: "Back"
                    ToolTip.text: "Return to the previous dashboard page."
                    enabled: host.web.canGoBack && !host.web.cacheBusy
                    onClicked: host.web.goBack()
                }
                QuietButton {
                    compact: true
                    glyphSource: "qrc:/assets/chevron.svg"
                    glyphRotation: -90
                    Accessible.name: "Forward"
                    ToolTip.text: "Move forward through the dashboard history."
                    enabled: host.web.canGoForward && !host.web.cacheBusy
                    onClicked: host.web.goForward()
                }
                QuietButton {
                    compact: true
                    text: "⌂"
                    Accessible.name: "Home"
                    ToolTip.text: "Open the saved picture-in-picture dashboard."
                    enabled: !host.web.cacheBusy
                    onClicked: host.web.goHome()
                }
                QuietButton {
                    id: pipReload
                    objectName: "hawebPipReload"
                    compact: true
                    text: "↻"
                    Accessible.name: "Reload"
                    ToolTip.text: "Reload this dashboard or retry starting the browser."
                    enabled: !host.web.cacheBusy
                    onClicked: host.web.reload()
                }
                Item { Layout.fillWidth: true }
                QuietButton {
                    objectName: "hawebPipZoomOut"
                    compact: true
                    text: "−"
                    Accessible.name: "Zoom Out"
                    ToolTip.text: "Fit more dashboard content in PiP. Its window size and other views' zoom stay the same."
                    enabled: host.web.pipScale > 50 && !host.web.cacheBusy
                    onClicked: host.web.pipScale -= 10
                }
                HintLabel {
                    Layout.preferredWidth: 40
                    horizontalAlignment: Text.AlignHCenter
                    text: host.web.pipScale + "%"
                    font.pixelSize: Theme.fontCaption
                }
                QuietButton {
                    objectName: "hawebPipZoomIn"
                    compact: true
                    text: "+"
                    Accessible.name: "Zoom In"
                    ToolTip.text: "Enlarge dashboard content in PiP. This zoom is saved separately from the main Web view."
                    enabled: host.web.pipScale < 200 && !host.web.cacheBusy
                    onClicked: host.web.pipScale += 10
                }
                Item { Layout.fillWidth: true }
                QuietButton {
                    compact: true
                    text: host.web.options.muted ? "Unmute" : "Mute"
                    ToolTip.text: "Toggle audio for WHAT's embedded browser."
                    onClicked: host.web.toggleMuted()
                }
                QuietButton {
                    compact: true
                    text: "⚙\uFE0E"
                    Accessible.name: "Web Settings"
                    ToolTip.text: "Open Web module settings, including PiP window size and zoom."
                    onClicked: {
                        host.web.hideOverlay();
                        host.settingsRequested();
                    }
                }
                QuietButton {
                    compact: true
                    text: "↗"
                    Accessible.name: "Open in Browser"
                    ToolTip.text: "Open this page in your default browser."
                    onClicked: host.web.openInBrowser()
                }
            }
        }

        RowLayout {
            id: pageToolbar
            visible: !host.pip && host.toolbarVisible
            Layout.fillWidth: true
            spacing: Theme.spaceS

            HoverHandler { id: pageHover }

            QuietButton {
                id: backButton
                glyphSource: "qrc:/assets/chevron.svg"
                glyphRotation: 90
                Accessible.name: "Back"
                enabled: !host.web.cacheBusy && host.web.canGoBack
                ToolTip.text: "Return to the previous dashboard page."
                onClicked: host.web.goBack()
            }

            QuietButton {
                glyphSource: "qrc:/assets/chevron.svg"
                glyphRotation: -90
                Accessible.name: "Forward"
                enabled: !host.web.cacheBusy && host.web.canGoForward
                ToolTip.text: "Move forward through this browser's page history."
                onClicked: host.web.goForward()
            }

            QuietButton {
                text: "Home"
                enabled: !host.web.cacheBusy
                ToolTip.text: "Open the dashboard address saved in module settings."
                onClicked: host.web.goHome()
            }

            QuietButton {
                id: reloadButton
                objectName: "hawebReload"
                text: "Reload"
                enabled: !host.web.cacheBusy
                ToolTip.text: "Reload the current page, or retry starting the browser after an error."
                onClicked: host.web.reload()
            }

            HintLabel {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                text: host.web.status
            }

            NumberInput {
                objectName: "hawebPageScale"
                from: 50
                to: 200
                stepSize: 10
                unit: "%"
                value: host.web.scale
                enabled: !host.web.cacheBusy
                Accessible.name: "Dashboard Render Scale"
                ToolTip.text: "Zoom the dashboard from 50% to 200%. Smaller values fit more on screen. Saved automatically and shared with the module's Render Scale setting."
                onValueModified: host.web.scale = value
            }

            QuietButton {
                text: host.web.options.muted ? "Unmute" : "Mute"
                ToolTip.text: "Toggle audio for WHAT's embedded browser. This setting is saved."
                onClicked: host.web.toggleMuted()
            }

            QuietButton {
                text: "Settings"
                ToolTip.text: "Change the dashboard address, scale, hotkey or browser data."
                onClicked: {
                    host.web.hideOverlay();
                    host.settingsRequested();
                }
            }

            QuietButton {
                objectName: "hawebBrowser"
                text: "Browser"
                ToolTip.text: "Open the current page in your default browser, using that browser's sign-in."
                onClicked: host.web.openInBrowser()
            }

            ActionButton {
                objectName: "hawebFullscreen"
                text: host.web.overlayVisible ? "Close Overlay" : "Fullscreen"
                ToolTip.text: "Toggle the fullscreen overlay on the current monitor. Escape also closes it."
                onClicked: host.web.overlayVisible ? host.web.hideOverlay() : host.web.showOverlay("fullscreen")
            }

            QuietButton {
                text: "PiP"
                ToolTip.text: "Open a small dashboard above your other apps. Drag its title bar to move it and its edges or corners to resize it."
                onClicked: host.web.showOverlay("pip")
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !host.pip || !host.web.pipMinimised

            Rectangle {
                anchors.fill: parent
                color: Theme.inset
            }

            WindowContainer {
                id: browser
                objectName: "hawebBrowserContainer"
                anchors.fill: parent
                window: host.web.browserWindow
                visible: host.presented && host.web.ready && !host.web.cacheBusy
                focus: true
                activeFocusOnTab: true
            }

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(560, parent.width - 48)
                visible: !host.web.ready || host.web.cacheBusy

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    visible: host.web.loading || host.web.cacheBusy
                    running: visible && host.presented
                    palette.dark: Theme.accent
                }

                HintLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    text: host.web.status
                }
            }
        }
    }

    Connections {
        target: host.web
        function onOverlayChanged() {
            host.revealToolbar();
            if (host.blocked && host.web.overlayVisible)
                host.web.hideOverlay();
        }
        function onFocusToolbarRequested() {
            host.revealToolbar();
            // Wait for the visibility binding before focusing an auto-hidden row.
            Qt.callLater(function() {
                (host.pip ? (host.web.pipMinimised ? pipMinimiseButton : pipReload) : reloadButton).forceActiveFocus(Qt.TabFocusReason);
            });
        }
        function onRevealToolbarRequested() {
            // Pointer movement can reveal an auto-hidden toolbar, but an
            // explicitly collapsed strip stays compact until clicked (or F6).
            if (!host.pipToolbarCollapsed)
                host.revealToolbar();
        }
    }
}
