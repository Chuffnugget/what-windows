import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "Accordion.js" as Accordion

Rectangle {
    id: section

    property ExpansionScroll expansionScroll: ExpansionScroll {
        targetItem: section
        expanded: section.expanded
    }
    function reveal() { expansionScroll.follow(true); }

    required property string title
    property string detail: ""
    property string keywords: ""
    property string query: ""
    readonly property bool searchMatch: query.toLowerCase().trim().split(/\s+/).filter(Boolean).every(word => (title + " " + detail + " " + keywords).toLowerCase().includes(word))
    visible: searchMatch

    property bool expanded: false
    function collapseSection() { expanded = false; }
    onExpandedChanged: if (expanded) Accordion.closeSiblings(section)
    default property alias contents: sectionBody.data
    Layout.fillWidth: true
    // Animate this disclosure only. Nested sections already animate their own
    // height, so an open parent must follow their geometry without another delay.
    property real expansion: expanded ? 1 : 0
    property ExpansionBlur expansionBlur: ExpansionBlur {
        target: sectionBody
        progress: section.expansion
    }
    implicitHeight: sectionHeader.height + expansion * (sectionBody.implicitHeight + Theme.spaceXl)
    color: "transparent"
    clip: true

    Behavior on expansion {
        NumberAnimation {
            duration: Theme.animationsEnabled ? Theme.timings.panels : 0
            easing.type: Easing.OutCubic
        }
    }

    AbstractButton {
        id: sectionHeader

        objectName: section.title + "SectionToggle"
        width: parent.width
        height: Theme.sectionHeight
        hoverEnabled: true
        Accessible.name: section.title + (section.expanded ? ", expanded" : ", collapsed")
        onClicked: {
            section.expanded = !section.expanded;
            if (section.expanded)
                section.expansionScroll.follow();
        }

        background: Rectangle {
            color: sectionHeader.hovered ? Theme.hover : section.expanded ? Theme.expandedHeader : "transparent"
            border.color: sectionHeader.visualFocus ? Theme.accent : "transparent"
        }

        contentItem: RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.spaceM
            spacing: Theme.spaceM

            Item {
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                Layout.alignment: Qt.AlignVCenter

                Image {
                    objectName: section.title + "SectionChevron"
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    source: "qrc:/assets/chevron.svg"
                    sourceSize: Qt.size(18, 18)
                    rotation: section.expanded ? 0 : -90

                    Behavior on rotation {
                        NumberAnimation {
                            duration: Theme.animationsEnabled ? Theme.timings.controls : 0
                            easing.type: Easing.OutCubic
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3

                Label {
                    text: section.title
                    color: Theme.ink
                    font.pixelSize: Theme.fontSection
                    font.weight: Font.DemiBold
                }

                HintLabel {
                    text: section.detail
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.separator
    }

    ColumnLayout {
        id: sectionBody

        visible: section.height > sectionHeader.height
        x: Theme.spaceXl
        y: sectionHeader.height + Theme.spaceM
        width: parent.width - Theme.spaceXl * 2
        spacing: Theme.spaceM
    }
}
