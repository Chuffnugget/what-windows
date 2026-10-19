import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "Accordion.js" as Accordion

Rectangle {
    id: section

    property ExpansionScroll expansionScroll: ExpansionScroll {
        targetItem: section
        expanded: section.contentExpanded
    }

    property bool animationsEnabled: Theme.animationsEnabled
    property int hoverDuration: Theme.timings.hover
    property int controlDuration: Theme.timings.controls
    property int panelDuration: Theme.timings.panels
    property string searchContext: ""
    property string title
    property string detail
    property string keywords: ""
    property string query: ""
    property bool available: true
    property string unavailableReason: ""
    signal restrictionDetailsRequested
    property bool active: false
    property bool expanded: false
    function collapseSection() { expanded = false; }
    onExpandedChanged: if (expanded) Accordion.closeSiblings(section)
    signal toggled(bool enabled)
    default property alias options: contents.data
    readonly property bool searching: query.trim().length > 0
    readonly property bool contentExpanded: expanded
    property real expansion: contentExpanded ? 1 : 0
    property ExpansionBlur expansionBlur: ExpansionBlur {
        target: contents
        progress: section.expansion
    }
    Behavior on expansion {
        NumberAnimation {
            duration: section.animationsEnabled ? section.panelDuration : 0
            easing.type: Easing.OutCubic
        }
    }
    visible: !searching || query.toLowerCase().trim().split(/\s+/).every(word => (searchContext + " " + title + " " + detail + " " + keywords).toLowerCase().indexOf(word) >= 0)
    Layout.fillWidth: true
    implicitHeight: header.height + body.height
    Layout.minimumHeight: implicitHeight
    Layout.maximumHeight: implicitHeight

    color: "transparent"
    data: [
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.separator
        },

        RowLayout {
            id: header

            width: parent.width
            height: 56
            spacing: 0

            AbstractButton {
                objectName: "submoduleExpandButton"
                padding: 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                hoverEnabled: true

                onClicked: {
                    section.expanded = !section.expanded;
                    if (section.contentExpanded)
                        section.expansionScroll.follow();
                }

                Accessible.name: section.title + (section.contentExpanded ? ", collapse" : ", expand")

                background: Rectangle {
                    color: parent.hovered ? Theme.hover : section.contentExpanded ? Theme.expandedHeader : "transparent"
                    border.color: parent.visualFocus ? Theme.accent : "transparent"

                    Behavior on color {
                        ColorAnimation {
                            duration: section.animationsEnabled ? section.hoverDuration : 0
                        }
                    }
                }

                contentItem: RowLayout {
                    spacing: Theme.spaceM

                    Item {
                        objectName: "submoduleChevronSlot"
                        Layout.minimumWidth: 20
                        Layout.preferredWidth: 20
                        Layout.maximumWidth: 20
                        Layout.minimumHeight: 20
                        Layout.preferredHeight: 20
                        Layout.maximumHeight: 20
                        Layout.leftMargin: 8

                        Image {
                            objectName: "submoduleChevron"
                            anchors.centerIn: parent
                            width: 20
                            height: 20
                            source: "qrc:/assets/chevron.svg"
                            sourceSize.width: 20
                            sourceSize.height: 20
                            fillMode: Image.PreserveAspectFit
                            rotation: section.contentExpanded ? 0 : -90

                            Behavior on rotation {
                                NumberAnimation {
                                    duration: section.animationsEnabled ? section.controlDuration : 0
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: section.title
                            color: section.available ? Theme.ink : Theme.muted
                            font.bold: true
                        }

                        Label {
                            text: section.available ? section.detail : section.unavailableReason
                            color: section.available ? Theme.muted : Theme.warning
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            QuietButton {
                objectName: "moduleRestrictionHelp"
                visible: !section.available
                text: "Why Unavailable?"
                Accessible.name: "Why is " + section.title + " unavailable?"
                ToolTip.text: section.unavailableReason + " Open the requirements and setup instructions."
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                onClicked: section.restrictionDetailsRequested()
            }

            ToggleSwitch {
                id: moduleToggle
                objectName: "submoduleToggle"

                animationDuration: section.animationsEnabled ? section.controlDuration : 0
                enabled: section.available
                ToolTip.text: section.available
                    ? (section.active ? "Disable " : "Enable ") + section.title + ". " + section.detail
                    : section.unavailableReason
                checked: section.available && section.active
                onClicked: section.toggled(checked)
                Accessible.name: "Enable " + section.title
                Layout.rightMargin: 12
            }
        },

        Item {
            id: body

            y: header.height
            width: parent.width
            clip: true
            height: section.expansion * (contents.implicitHeight + 28)

            Rectangle {
                x: 18
                y: 8
                width: 2
                height: Math.max(0, parent.height - 20)
                color: Theme.outline
            }

            ColumnLayout {
                id: contents

                enabled: section.available
                x: 40
                y: 8
                width: parent.width - 52
                spacing: Theme.spaceM
            }
        }
    ]
}
