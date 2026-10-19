import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

// A compact disclosure for the command list's permissions and activity.
Column {
    id: section

    property ExpansionScroll expansionScroll: ExpansionScroll {
        targetItem: section
        expanded: section.expanded
    }
    function reveal() { expansionScroll.follow(true); }

    property string title: ""
    property string summary: ""
    property bool expanded: false
    property ExpansionBlur expansionBlur: ExpansionBlur {
        target: body
        progress: reveal.expansion
    }
    default property alias content: body.data

    AbstractButton {
        id: toggle

        objectName: section.title + "SectionToggle"
        width: parent.width
        height: 44
        hoverEnabled: true
        Accessible.name: section.title
        Accessible.description: section.summary
        Accessible.role: Accessible.Button
        onClicked: {
            section.expanded = !section.expanded;
            if (section.expanded)
                section.expansionScroll.follow();
        }

        background: Rectangle {
            color: toggle.hovered ? Theme.hover : section.expanded ? Theme.expandedHeader : "transparent"
            border.width: toggle.visualFocus ? 1 : 0
            border.color: Theme.accent
        }
        contentItem: RowLayout {
            spacing: Theme.spaceM
            Image {
                Layout.leftMargin: Theme.spaceM
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                source: "qrc:/assets/chevron.svg"
                rotation: section.expanded ? 0 : -90
                Behavior on rotation {
                    NumberAnimation {
                        duration: Theme.animationsEnabled ? Theme.timings.controls : 0
                    }
                }
            }
            Label {
                text: section.title
                color: Theme.ink
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                Layout.rightMargin: 12
                text: section.summary
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                color: Theme.muted
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    Item {
        id: reveal
        width: parent.width
        property real expansion: section.expanded ? 1 : 0
        height: expansion * (body.implicitHeight + 24)
        clip: true
        visible: height > 0

        Behavior on expansion {
            NumberAnimation {
                duration: Theme.animationsEnabled ? Theme.timings.panels : 0
                easing.type: Easing.OutCubic
            }
        }
        ColumnLayout {
            id: body
            x: 12
            y: 12
            width: parent.width - 24
            spacing: Theme.spaceM
        }
    }

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.separator
    }
}
