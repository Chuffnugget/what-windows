import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../controls/Accordion.js" as Accordion
import "ModuleStatus.js" as ModuleStatus

Rectangle {
    id: card

    property ExpansionScroll expansionScroll: ExpansionScroll {
        targetItem: card
        expanded: card.expanded
    }
    function reveal() {
        expansionScroll.follow(true);
    }

    required property var controller
    property string query: ""
    property Item reorderHost: null

    function matches(text) {
        const words = query.toLowerCase().trim().split(/\s+/).filter(Boolean);
        const searchable = text.toLowerCase();
        return words.every(word => searchable.includes(word));
    }

    objectName: moduleKey + "ModuleCard"
    required property string moduleKey
    required property string heading
    required property string summary
    required property string keywords
    property string toggleHelp: "Enable or disable " + heading + " collection. Saved sensor selections are retained when the module is disabled."
    property bool collapsed: true
    readonly property bool expanded: !collapsed
    property ExpansionBlur expansionBlur: ExpansionBlur {
        target: optionList
        progress: reveal.expansion
    }
    function collapseSection() {
        collapsed = true;
    }
    onExpandedChanged: if (expanded) Accordion.closeSiblings(card)

    function toggleExpanded() {
        collapsed = !collapsed;
        if (expanded)
            expansionScroll.follow();
    }

    readonly property bool moduleOn: ModuleStatus.enabled(controller, moduleKey)
    readonly property bool sourceConnected: ModuleStatus.connected(controller, moduleKey)
    readonly property string connectionText: ModuleStatus.connectionText(controller, moduleKey)
    readonly property string issue: ModuleStatus.issue(controller, moduleKey)
    readonly property color connectionColour: ModuleStatus.statusColour(controller, moduleKey, Theme)
    default property alias options: optionList.data
    Layout.fillWidth: true
    Layout.row: reorderHost ? reorderHost.order.indexOf(moduleKey) : -1
    Layout.minimumWidth: 0
    Layout.maximumWidth: parent.width
    implicitHeight: reveal.y + reveal.height
    Layout.minimumHeight: implicitHeight
    Layout.maximumHeight: implicitHeight
    visible: card.matches(keywords)

    color: "transparent"

    Behavior on color {
        ColorAnimation {
            duration: Theme.animationsEnabled ? Theme.timings.hover : 0
        }
    }

    data: [
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.separator
        },

        Item {
            width: parent.width
            height: 72

            Rectangle {
                anchors.fill: parent

                color: moduleHover.hovered ? Theme.hover : card.expanded ? Theme.expandedHeader : "transparent"
            }

            HoverHandler {
                id: moduleHover

            }

            MouseArea {
                objectName: card.moduleKey + "Header"
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: card.toggleExpanded()
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 12
                spacing: Theme.spaceM

                ModuleReorderGrip {
                    moduleItem: card
                    reorderHost: card.reorderHost
                }

                ChevronButton {
                    objectName: card.moduleKey + "Expand"
                    expanded: card.expanded
                    onClicked: card.toggleExpanded()
                }

                Image {
                    objectName: card.moduleKey + "ModuleLogo"
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    source: {
                        switch (card.moduleKey) {
                        case "custom":
                        case "haweb":
                            return "qrc:/assets/Logo150.png";
                        case "wallpaper":
                            return "qrc:/assets/brands/wallpaper.png";
                        case "icue":
                            return "qrc:/assets/brands/corsair.svg";
                        case "hwinfo":
                            return "qrc:/assets/brands/hwinfo.png";
                        default:
                            return "qrc:/assets/brands/" + card.moduleKey + ".svg";
                        }
                    }
                    sourceSize.width: 84
                    sourceSize.height: 84
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 3

                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        text: card.heading
                        font.pixelSize: Theme.fontSection
                        font.weight: Font.DemiBold
                        color: Theme.ink
                    }

                    HintLabel {
                        text: card.summary
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                    }
                }

                RowLayout {
                    spacing: 6

                    Rectangle {
                        width: 7
                        height: 7

                        color: card.connectionColour
                    }

                    HintLabel {
                        objectName: card.moduleKey + "ConnectionStatus"
                        text: card.connectionText
                        color: card.connectionColour
                        Accessible.name: card.heading + ": " + text
                    }
                }

                ToggleSwitch {
                    objectName: card.moduleKey + "ModuleToggle"
                    enabled: !ModuleStatus.unavailable(controller, card.moduleKey)
                    ToolTip.text: ModuleStatus.unavailable(controller, card.moduleKey) || card.toggleHelp
                    checked: card.moduleOn
                    Accessible.name: "Enable " + card.heading + " module"
                    onClicked: controller.setModuleEnabled(card.moduleKey, checked)
                }
            }
        },

        WarningBanner {
            id: moduleWarning

            objectName: card.moduleKey + "ModuleWarning"
            x: 24
            y: 72 + Theme.spaceM
            width: parent.width - 48
            visible: card.issue.length > 0
            title: ModuleStatus.unavailable(controller, card.moduleKey) ? card.heading + " Unavailable on ARM64" : card.heading + " Needs Attention"
            detail: card.issue
        },

        Item {
            id: reveal

            y: moduleWarning.visible ? moduleWarning.y + moduleWarning.height + Theme.spaceM : 72
            width: parent.width
            clip: true
            property real expansion: card.expanded ? 1 : 0
            height: expansion * (optionList.implicitHeight + 30)

            // Nested disclosures supply their own animation. Follow their height
            // directly rather than restarting the parent's easing every frame.
            Behavior on expansion {
                NumberAnimation {
                    duration: Theme.animationsEnabled ? Theme.timings.panels : 0
                    easing.type: Easing.OutCubic
                }
            }

            Rectangle {
                anchors.fill: parent
                anchors.bottomMargin: 1
                color: Theme.inset
            }

            ColumnLayout {
                id: optionList

                x: 24
                y: 12
                width: parent.width - 48
                spacing: Theme.spaceM
                enabled: card.moduleOn
            }
        }
    ]
}
