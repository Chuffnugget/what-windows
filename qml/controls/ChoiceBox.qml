import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: choice

    property bool compact: false
    implicitWidth: 190
    implicitHeight: compact ? Theme.compactHeight : Theme.controlHeight
    leftPadding: compact ? Theme.spaceS : Theme.spaceM
    rightPadding: 28
    hoverEnabled: true
    onVisibleChanged: if (!visible) choicePopup.contentsReady = false
    font.pixelSize: compact ? Theme.fontCaption : Theme.fontBody
    ToolTip.visible: (hovered || visualFocus) && ToolTip.text.length > 0
    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout

    contentItem: Text {
        text: choice.displayText
        font: choice.font
        color: choice.enabled ? Theme.ink : Theme.muted
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Image {
        x: choice.width - 24
        y: (choice.height - height) / 2
        source: "qrc:/assets/chevron.svg"
        sourceSize.width: 16
        sourceSize.height: 16
        opacity: choice.enabled ? 1 : 0.4
    }

    background: Rectangle {
        color: choice.hovered ? Theme.hover : Theme.field
        border.color: choice.visualFocus ? Theme.accent : Theme.outline
    }

    delegate: ItemDelegate {
        required property int index
        width: choiceList.width - (choiceBar.visible ? choiceBar.width : 0)
        height: choice.compact ? Theme.compactHeight : Theme.controlHeight
        highlighted: choice.highlightedIndex === index
        padding: Theme.spaceS

        contentItem: Text {
            text: choice.textAt(index)
            font: choice.font
            color: index === choice.currentIndex ? Theme.accent : Theme.ink
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            color: parent.highlighted ? Theme.hover : Theme.surface
        }
    }

    popup: Popup {
        id: choicePopup

        property real reveal: 0
        property bool contentsReady: false
        onAboutToShow: contentsReady = true
        property ExpansionBlur refraction: ExpansionBlur {
            target: choiceList
            progress: choicePopup.reveal
        }
        y: choice.height + Theme.spaceXs
        width: choice.width
        padding: Theme.spaceXs
        implicitHeight: Math.min(300, choiceList.contentHeight + padding * 2)
        height: implicitHeight * reveal
        clip: true

        enter: Transition {
            NumberAnimation {
                target: choicePopup
                property: "reveal"
                to: 1
                duration: Theme.animationsEnabled ? Theme.timings.panels : 0
                easing.type: Easing.InOutCubic
            }
        }

        exit: Transition {
            NumberAnimation {
                target: choicePopup
                property: "reveal"
                to: 0
                duration: Theme.animationsEnabled ? Theme.timings.panels : 0
                easing.type: Easing.InOutCubic
            }
        }

        background: Rectangle {
            color: Theme.surface
            border.color: Theme.outline
        }

        contentItem: Item {
            // Keep the list at its full size while the popup reveals it. Resizing
            // the list on every frame would repeatedly scroll the selected row.
            ListView {
                id: choiceList

                width: parent.width
                height: Math.max(0, choicePopup.implicitHeight - choicePopup.padding * 2)
                clip: true
                // Populate on first use, then keep the visible rows for reopening.
                // Hiding the owning control releases them. Rebuilding the list on
                // every open added a GUI-thread stall to the first slide frame.
                model: choicePopup.contentsReady ? choice.delegateModel : null
                currentIndex: choice.highlightedIndex
                highlightMoveDuration: 0
                ScrollBar.vertical: WideScrollBar {
                    id: choiceBar
                }
            }
        }
    }
}
