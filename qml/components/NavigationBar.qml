import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

Rectangle {
    id: sidebar

    property int currentPage: 0
    property int expandedWidth: 204
    property bool collapsed: false
    property int maximumWidth: 360
    property bool ready: false
    // The idle divider occupies layout space; its larger drag hit area does not.
    readonly property int dividerWidth: 4
    readonly property real expandedExtent: Math.min(expandedWidth, maximumWidth)
    readonly property real expandedContentWidth: expandedExtent - 40 - dividerWidth
    readonly property real expansion: Math.max(0, Math.min(1,
        (width - 64) / Math.max(1, expandedExtent - 64)))
    // Hide full-size text before the narrowing sidebar would cut through it.
    // Keep the endpoint exact so the temporary shader layer is released.
    readonly property real textReveal: expansion >= 1 ? 1 : Math.max(0, (expansion - 0.55) / 0.45)
    Component.onCompleted: ready = true
    property bool logoAnimationPending: false
    onCollapsedChanged: {
        if (ready) {
            logoAnimationPending = Theme.animationsEnabled;
            Qt.callLater(playLogoAfterResize);
        }
    }

    function playLogoAfterResize() {
        if (!logoAnimationPending || widthAnimation.running || resizeGrip.dragging)
            return;
        if (Math.abs(width - (collapsed ? 64 : expandedExtent)) > 0.01)
            return;
        sidebarLogo.playOnce();
        logoAnimationPending = false;
    }

    signal layoutEdited(int expandedWidth, bool collapsed)

    function resizeTo(position) {
        widthAnimation.duration = Theme.timings.panels;
        collapsed = position < 136;
        if (!collapsed)
            expandedWidth = Math.round(Math.max(180, Math.min(maximumWidth, position)));
    }

    function toggleCollapsed() {
        widthAnimation.duration = Theme.timings.panels;
        collapsed = !collapsed;
        layoutEdited(expandedWidth, collapsed);
    }
    // Tighten navigation spacing before the status footer can overflow.
    readonly property bool compact: height < 1000
    readonly property var pageOrder: (workspacePages.model
        || []).concat(utilityPages.model
        || []).map(entry => entry.page)
    property bool healthy: false
    property string pcName: ""
    property string version: ""
    property string availableUpdate: ""
    signal navigationRequested(int page)
    signal updateRequested
    objectName: "navigationSidebar"
    width: resizeGrip.dragging ? resizeGrip.dragWidth : collapsed ? 64 : expandedExtent
    anchors.left: parent.left
    anchors.top: parent.top
    anchors.bottom: parent.bottom
    color: Theme.sidebar
    clip: true

    // Restore saved geometry immediately. Follow the pointer while dragging,
    // then animate from its released position to the compact or expanded width.
    Behavior on width {
        enabled: sidebar.ready && Theme.animationsEnabled
        NumberAnimation {
            id: widthAnimation
            // Reversals can stop one animation before starting the next. Wait
            // for the final geometry rather than playing at that interruption.
            onRunningChanged: if (!running)
                Qt.callLater(sidebar.playLogoAfterResize)
            duration: Theme.timings.panels
            // Give the text effect time at both ends of the width transition.
            easing.type: Easing.InOutCubic
        }
    }

    ColumnLayout {
        id: navigationLayout

        anchors.fill: parent
        anchors.topMargin: 20
        anchors.bottomMargin: 20
        anchors.leftMargin: 8 + 12 * sidebar.expansion
        anchors.rightMargin: anchors.leftMargin + sidebar.dividerWidth
        spacing: sidebar.height < 900 ? 2 : sidebar.compact ? 4 : 8

        Item {
            id: logoArea

            // Keep the large logo unless the screen is too short to fit it.
            readonly property real logoHeight: 44 + sidebar.expansion
                * ((sidebar.height < 700 ? 60 : Math.min(120, sidebar.expandedExtent - 84)) - 44)
            Layout.fillWidth: true
            // The compact update button gets its own row below the logo.
            // Reserve its space throughout the width animation to avoid overlap.
            Layout.preferredHeight: logoHeight
                + (updateBadge.visible ? (updateBadge.height + 8) * (1 - sidebar.expansion) : 0)
            Layout.topMargin: 8

            AnimatedLogo {
                id: sidebarLogo
                hoverAnimationEnabled: !sidebar.logoAnimationPending
                animationDuration: Theme.timings.logo
                pauseDuration: Theme.timings.logoPause
                textHovered: logoOverview.hovered || brandTitleHover.hovered || brandExpansionHover.hovered
                animationsEnabled: Theme.animationsEnabled && !widthAnimation.running && !resizeGrip.dragging
                anchors.horizontalCenter: parent.horizontalCenter
                width: 32 + sidebar.expansion * (Math.min(120, sidebar.expandedContentWidth) - 32)
                height: logoArea.logoHeight
            }

            AbstractButton {
                id: logoOverview

                objectName: "logoOverviewButton"
                anchors.fill: parent
                hoverEnabled: true
                ToolTip.text: "Overview: Return to the Overview page."
                ToolTip.visible: hovered
                ToolTip.delay: Theme.toolTipDelay
                ToolTip.timeout: Theme.toolTipTimeout
                Accessible.name: "Open Overview"
                onClicked: sidebar.navigationRequested(0)

                background: Item {}

                contentItem: Item {}
            }

            AbstractButton {
                id: updateBadge

                objectName: "updateBadge"
                x: (parent.width - width) / 2 * (1 - sidebar.expansion)
                    + (parent.width - width - 4) * sidebar.expansion
                y: logoArea.logoHeight - height + (height + 8) * (1 - sidebar.expansion)
                width: 24 + 6 * sidebar.expansion
                height: width
                padding: 4 + sidebar.expansion
                hoverEnabled: true
                visible: sidebar.availableUpdate.length > 0
                Accessible.name: "Update available: version " + sidebar.availableUpdate
                ToolTip.visible: hovered
                ToolTip.text: "Settings: Version " + sidebar.availableUpdate + " is available. Open update settings."
                onClicked: sidebar.updateRequested()

                background: Item {}

                contentItem: Image {
                    source: "qrc:/assets/update-available.svg"
                    sourceSize: Qt.size(20, 20)
                    fillMode: Image.PreserveAspectFit
                }

                scale: hovered ? 1.05 : 1

                Behavior on scale {
                    NumberAnimation {
                        duration: Theme.animationsEnabled ? Theme.timings.hover : 0
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }

        // Resize one slot, not the labels themselves. Their line height and
        // layout stay stable while the navigation rows move up or down.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: brandText.implicitHeight * sidebar.expansion
            clip: true

            ColumnLayout {
                id: brandText
                property ExpansionBlur textTransition: ExpansionBlur {
                    target: brandText
                    progress: sidebar.textReveal
                }
                anchors.horizontalCenter: parent.horizontalCenter
                width: sidebar.expandedContentWidth
                spacing: navigationLayout.spacing
                opacity: Math.min(1, sidebar.textReveal * 2)
                visible: sidebar.textReveal > 0

                Label {
                    objectName: "sidebarBrandTitle"
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    horizontalAlignment: Text.AlignHCenter
                    text: "WHAT"
                    font.pixelSize: Theme.fontHeading
                    font.weight: Font.Bold
                    color: Theme.ink

                    HoverHandler {
                        id: brandTitleHover
                    }
                }

                HintLabel {
                    objectName: "sidebarBrandExpansion"
                    visible: sidebar.height >= 800
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    textFormat: Text.PlainText
                    text: "Windows Home\nAssistant Toolkit"
                    wrapMode: Text.NoWrap
                    lineHeight: 1.3

                    HoverHandler {
                        id: brandExpansionHover
                    }
                }

                HintLabel {
                    text: "WORKSPACE"
                    font.pixelSize: Theme.fontLabel
                    font.letterSpacing: 1.8
                    Layout.topMargin: sidebar.compact ? 10 : 30
                    Layout.bottomMargin: 6
                }
            }
        }

        Repeater {
            id: workspacePages

            model: [
                {
                    title: "Overview",
                    help: "View connection status, shared sensor counts and pairing details.",
                    glyph: "◈",
                    page: 0
                },

                {
                    title: "Sensors",
                    help: "Browse readings, use local controls and choose local or cloud sharing permissions.",
                    glyph: "≡",
                    page: 1
                },

                {
                    title: "Commands",
                    help: "Create and run saved commands, test one-off commands and manage Home Assistant execution permissions.",
                    glyph: ">_",
                    page: 8
                },

                {
                    title: "Web",
                    help: "Open your Home Assistant dashboard inside WHAT, on the desktop or in a fullscreen or picture-in-picture overlay.",
                    glyph: "⌂",
                    page: 9
                },

                {
                    title: "Modules",
                    help: "Enable sources and configure discovery, polling and control permissions.",
                    glyph: "▦",
                    page: 5
                },

                {
                    title: "Specs",
                    help: "View and copy this PC's hardware and Windows specifications.",
                    glyph: "▤",
                    page: 4
                },

                {
                    title: "Taskbar",
                    help: "Manage WHAT icons and extra sensor icons on the taskbar and beside the Windows clock.",
                    glyph: "▣",
                    page: 7
                }
            ]

            delegate: NavigationButton {
                required property var modelData
                iconOnly: sidebar.collapsed
                textExpansion: sidebar.expansion
                expandedWidth: sidebar.expandedContentWidth
                collapsedWidth: 48 - sidebar.dividerWidth
                implicitHeight: sidebar.compact ? 36 : 43
                ToolTip.text: modelData.title + ": " + modelData.help
                text: modelData.title
                glyph: modelData.glyph
                selected: sidebar.currentPage === modelData.page
                onClicked: sidebar.navigationRequested(modelData.page)
            }
        }

        Item {
            Layout.fillHeight: true
        }

        Repeater {
            id: utilityPages

            model: [
                {
                    title: "About",
                    help: "Find project information, support links and acknowledgements.",
                    glyph: "ⓘ",
                    page: 6,
                    key: "aboutNavigation"
                },

                {
                    title: "Diagnostics",
                    help: "Inspect module versions, connection status and logs, or prepare a redacted bug report.",
                    glyph: "⌁",
                    page: 3,
                    key: "diagnosticsNavigation"
                },

                {
                    title: "Settings",
                    help: "Configure Home Assistant connections, updates, startup, appearance and pairing security.",
                    glyph: "⚙\uFE0E",
                    page: 2,
                    key: "settingsNavigation"
                }
            ]

            delegate: NavigationButton {
                required property var modelData
                iconOnly: sidebar.collapsed
                textExpansion: sidebar.expansion
                expandedWidth: sidebar.expandedContentWidth
                collapsedWidth: 48 - sidebar.dividerWidth
                implicitHeight: sidebar.compact ? 36 : 43
                objectName: modelData.key
                ToolTip.text: modelData.title + ": " + modelData.help
                text: modelData.title
                glyph: modelData.glyph
                selected: sidebar.currentPage === modelData.page
                onClicked: sidebar.navigationRequested(modelData.page)
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 13 + sidebar.expansion * (footerText.implicitHeight + navigationLayout.spacing)
            clip: true

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.separator
            }

            ColumnLayout {
                id: footerText
                property ExpansionBlur textTransition: ExpansionBlur {
                    target: footerText
                    progress: sidebar.textReveal
                }
                y: 13 + navigationLayout.spacing
                width: sidebar.expandedContentWidth
                spacing: navigationLayout.spacing
                opacity: Math.min(1, sidebar.textReveal * 2)
                visible: sidebar.textReveal > 0

                RowLayout {
                    objectName: "sidebarPcStatus"
                    visible: sidebar.pcName.length > 0
                    Layout.fillWidth: true

                    Rectangle {
                        width: 7
                        height: 7
                        color: sidebar.healthy ? Theme.accent : Theme.warning
                    }

                    Label {
                        text: sidebar.pcName
                        color: Theme.ink
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                    }
                }

                HintLabel {
                    objectName: "sidebarVersion"
                    text: "WHAT v" + sidebar.version
                    font.pixelSize: Theme.fontSmall
                    Layout.topMargin: 5
                }
            }
        }
    }

    // A click toggles; a drag resizes. Keep the hit area fixed while the visible
    // border grows, and save only once when the gesture finishes.
    MouseArea {
        id: resizeGrip
        objectName: "sidebarResizeGrip"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 8
        cursorShape: Qt.SizeHorCursor
        hoverEnabled: true
        activeFocusOnTab: true
        property real pressX: 0
        property real pressWidth: 0
        property real dragWidth: 0
        property bool dragging: false
        Accessible.role: Accessible.Button
        Accessible.name: sidebar.collapsed ? "Expand Navigation" : "Collapse Navigation"
        Accessible.description: "Click to collapse or expand navigation. Drag to resize, or use the arrow keys."
        Accessible.onPressAction: sidebar.toggleCollapsed()
        ToolTip.text: "Navigation: Click to collapse or expand navigation. Drag to resize. When focused, use Enter or Space to toggle, or the arrow keys to resize."
        ToolTip.visible: containsMouse && !pressed
        ToolTip.delay: Theme.toolTipDelay
        ToolTip.timeout: Theme.toolTipTimeout
        onPressed: mouse => {
            forceActiveFocus(Qt.MouseFocusReason);
            pressX = mapToItem(sidebar, mouse.x, mouse.y).x;
            pressWidth = sidebar.width;
            dragWidth = sidebar.width;
            dragging = false;
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return;
            const delta = mapToItem(sidebar, mouse.x, mouse.y).x - pressX;
            if (!dragging && Math.abs(delta) >= Qt.styleHints.startDragDistance) {
                // Set timing before changing geometry; binding evaluation order
                // must not make the release inherit a zero-duration animation.
                widthAnimation.duration = 0;
                dragging = true;
            }
            if (dragging)
                dragWidth = Math.max(64, Math.min(sidebar.maximumWidth, pressWidth + delta));
        }
        function finishResize() {
            sidebar.resizeTo(dragWidth);
            dragging = false;
            sidebar.layoutEdited(sidebar.expandedWidth, sidebar.collapsed);
        }
        onReleased: {
            if (dragging)
                finishResize();
            else
                sidebar.toggleCollapsed();
        }
        onCanceled: {
            if (dragging)
                finishResize();
        }
        Keys.onLeftPressed: {
            sidebar.resizeTo(sidebar.width <= 180 ? 64 : sidebar.width - 24);
            sidebar.layoutEdited(sidebar.expandedWidth, sidebar.collapsed);
        }
        Keys.onRightPressed: {
            sidebar.resizeTo(sidebar.collapsed ? sidebar.expandedWidth : sidebar.width + 24);
            sidebar.layoutEdited(sidebar.expandedWidth, sidebar.collapsed);
        }
        Keys.onReturnPressed: sidebar.toggleCollapsed()
        Keys.onSpacePressed: sidebar.toggleCollapsed()
        Rectangle {
            objectName: "sidebarEdge"
            anchors.right: parent.right
            width: resizeGrip.containsMouse || resizeGrip.pressed ? 8 : sidebar.dividerWidth
            height: parent.height
            color: resizeGrip.containsMouse || resizeGrip.pressed || resizeGrip.activeFocus ? Theme.accent : Theme.outline
            Behavior on width {
                NumberAnimation {
                    duration: Theme.animationsEnabled ? Theme.timings.hover : 0
                    easing.type: Easing.OutCubic
                }
            }
            Behavior on color {
                ColorAnimation { duration: Theme.animationsEnabled ? Theme.timings.hover : 0 }
            }
        }
    }
}
