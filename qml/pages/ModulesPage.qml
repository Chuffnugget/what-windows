import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../modules"

ColumnLayout {
    id: modulesPage
    objectName: "modulesPage"

    readonly property var order: controller.moduleOrder

    ModuleReorderArea {
        id: moduleReorder
        objectName: "modulesReorderArea"
        controller: modulesPage.controller
        scrollView: modulesScroll
        contents: moduleContents
        cards: modulesPage.moduleCards
    }

    function openModule(key) {
        moduleSearch.text = "";
        moduleCards.forEach(card => {
            card.collapsed = card.moduleKey !== key;
        });
        const card = moduleCards.find(card => card.moduleKey === key);
        if (card)
            Qt.callLater(card.reveal);
    }

    required property var controller
    readonly property var status: controller.info
    property bool portable: status.packaged !== true
    signal portableRequested
    signal wallpaperRequested
    signal hwinfoSetupRequested
    signal customSensorsRequested
    signal commandsRequested
    signal clearWebDataRequested
    readonly property var moduleCards: [beacnSettings,
        wallpaperSettings,
        icueSettings,
        nvidiaSettings,
        coretempSettings,
        hwinfoSettings,
        customSettings,
        windowsSettings,
        homeAssistantWebSettings]
    spacing: Theme.spaceM

    InputField {
        id: moduleSearch

        objectName: "moduleSearch"
        onTextChanged: moduleReorder.cancelReorder()
        Layout.fillWidth: true
        Layout.rightMargin: Theme.scrollGutter
        placeholderText: "Search modules or options - temperature, privacy, notifications…"
        rightPadding: 42

        QuietButton {
            ToolTip.text: "Clear the current search."
            hoverEnabled: true
            ToolTip.visible: hovered
            ToolTip.delay: Theme.toolTipDelay
            ToolTip.timeout: Theme.toolTipTimeout
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 34
            text: "×"
            visible: moduleSearch.text.length > 0
            onClicked: moduleSearch.clear()
            Accessible.name: "Clear module search"
        }
    }

    HintLabel {
        text: "Drag a module's grip to reorder. Changes save automatically unless an Apply button is shown. Disabling a module keeps its settings and sensor selections."
        Layout.fillWidth: true
        Layout.rightMargin: Theme.scrollGutter
        wrapMode: Text.WordWrap
    }

    DragScrollView {
        id: modulesScroll

        objectName: "modulesScroll"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentWidth: availableWidth
        contentHeight: moduleContents.implicitHeight

        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ScrollBar.vertical: WideScrollBar {
            policy: ScrollBar.AsNeeded
            parent: modulesScroll
            x: modulesScroll.width - width
            y: 0
            height: modulesScroll.availableHeight
        }

        GridLayout {
            id: moduleContents

            width: modulesScroll.availableWidth - Theme.scrollGutter
            columns: 1
            rowSpacing: 2
            columnSpacing: 0

            BeacnSettings {
                id: beacnSettings

                controller: modulesPage.controller
                reorderHost: moduleReorder
                query: moduleSearch.text
            }

            WallpaperSettings {
                id: wallpaperSettings

                controller: modulesPage.controller
                reorderHost: moduleReorder
                query: moduleSearch.text
                onControlsRequested: modulesPage.wallpaperRequested()
            }

            IcueSettings {
                id: icueSettings

                controller: modulesPage.controller
                reorderHost: moduleReorder
                query: moduleSearch.text
            }

            NvidiaSettings {
                id: nvidiaSettings

                controller: modulesPage.controller
                reorderHost: moduleReorder
                query: moduleSearch.text
            }

            CoreTempSettings {
                id: coretempSettings

                controller: modulesPage.controller
                reorderHost: moduleReorder
                query: moduleSearch.text
            }

            HwinfoSettings {
                id: hwinfoSettings

                controller: modulesPage.controller
                reorderHost: moduleReorder
                query: moduleSearch.text
                onSetupRequested: modulesPage.hwinfoSetupRequested()
            }

            CustomSettings {
                id: customSettings

                controller: modulesPage.controller
                reorderHost: moduleReorder
                query: moduleSearch.text
                onSensorsRequested: modulesPage.customSensorsRequested()
                onCommandsRequested: modulesPage.commandsRequested()
            }

            WindowsSettings {
                id: windowsSettings

                portable: modulesPage.portable
                controller: modulesPage.controller
                reorderHost: moduleReorder
                query: moduleSearch.text
                onPortableRequested: modulesPage.portableRequested()
                onCommandsRequested: modulesPage.commandsRequested()
            }

            HomeAssistantWebSettings {
                id: homeAssistantWebSettings
                controller: modulesPage.controller
                reorderHost: moduleReorder
                query: moduleSearch.text
                onClearDataRequested: modulesPage.clearWebDataRequested()
            }

            AbstractButton {
                id: requestModule

                objectName: "requestModule"
                Layout.row: modulesPage.order.length
                Layout.fillWidth: true
                implicitHeight: 72
                hoverEnabled: true
                Accessible.role: Accessible.Link
                Accessible.name: "Want WHAT to support another API? Make a feature request on GitHub."
                onClicked: Qt.openUrlExternally("https://github.com/Chuffnugget/what-windows/issues/new?title=API%20support%20request%3A%20")

                background: Rectangle {
                    color: requestModule.hovered ? Theme.hover : "transparent"
                    border.width: requestModule.visualFocus ? 1 : 0
                    border.color: Theme.accent
                }

                contentItem: RowLayout {
                    spacing: Theme.spaceM

                    Text {
                        text: "+"
                        color: Theme.accent
                        font.pixelSize: Theme.fontTitle
                        Layout.preferredWidth: 54
                        horizontalAlignment: Text.AlignHCenter
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3

                        Label {
                            text: "Want WHAT to Support Another API?"
                            color: Theme.ink
                            font.pixelSize: Theme.fontSection
                            font.weight: Font.DemiBold
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }

                        HintLabel {
                            text: "Make a feature request on GitHub ↗"
                            color: Theme.accent
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            HintLabel {
                Layout.row: modulesPage.order.length + 1
                visible: !modulesPage.moduleCards.some(card => card.visible)
                text: "No modules match your search"
                Layout.fillWidth: true
            }
        }
    }

}
