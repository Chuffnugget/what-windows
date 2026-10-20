import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: timingDialog

    required property var controller

    objectName: "uiTimingDialog"
    title: "Animation Timings"
    modal: true
    anchors.centerIn: parent
    width: Math.min(510, timingDialog.parent.width - 40)
    height: Math.min(740, timingDialog.parent.height - 60)
    standardButtons: Dialog.Close

    contentItem: DragScrollView {
        id: timingScroll

        objectName: "timingScroll"
        clip: true
        contentWidth: availableWidth
        contentHeight: timingContents.implicitHeight
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ScrollBar.vertical: WideScrollBar {
            parent: timingScroll
            x: timingScroll.width - width
            y: timingScroll.topPadding
            height: timingScroll.availableHeight
        }

        ColumnLayout {
            id: timingContents

            width: timingScroll.availableWidth - 22
            spacing: Theme.spaceM

            HintLabel {
                text: "Durations are in milliseconds and save automatically. Sections and menus blur as they close and sharpen as they open. Helper text, prompts and the main window have no open or close animation. Zero makes a transition immediate. Longer page jumps transition faster. Page Changes moves each page together with its title. The animation switch still takes priority."
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: [
                    {
                        key: "hover",
                        label: "Hover Effects",
                        min: 0,
                        max: 2000
                    },

                    {
                        key: "controls",
                        label: "Controls and Chevrons",
                        min: 0,
                        max: 2000
                    },

                    {
                        key: "panels",
                        label: "Expand / Collapse",
                        min: 0,
                        max: 2000
                    },

                    {
                        key: "lists",
                        label: "List Movement",
                        min: 0,
                        max: 2000
                    },

                    {
                        key: "pages",
                        label: "Page Changes",
                        min: 0,
                        max: 2000
                    },


                    {
                        key: "dialogEnter",
                        label: "Menus Open",
                        min: 0,
                        max: 2000
                    },

                    {
                        key: "dialogExit",
                        label: "Menus Close",
                        min: 0,
                        max: 2000
                    },

                    {
                        key: "logo",
                        label: "Logo Animation",
                        min: 250,
                        max: 10000
                    },

                    {
                        key: "logoPause",
                        label: "Logo Repeat Pause",
                        min: 0,
                        max: 5000
                    },

                    {
                        key: "toast",
                        label: "Toast Messages",
                        min: 500,
                        max: 30000
                    }
                ]

                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            text: modelData.label
                            color: Theme.ink
                            Layout.fillWidth: true
                        }

                        MillisInput {
                            objectName: "timing-" + modelData.key
                            from: modelData.min
                            to: modelData.max
                            stepSize: 10
                            value: Theme.timings[modelData.key]
                            onValueModified: timingDialog.controller.setUiTiming(modelData.key, value)
                        }
                    }

                    HintLabel {
                        text: modelData.help || ""
                        visible: text.length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }
                }
            }

            ActionButton {
                ToolTip.text: "Reset every timing in this list, including any custom values."
                objectName: "resetAnimationTimings"
                text: "Reset Defaults"
                onClicked: timingDialog.controller.resetUiTimings()
            }
        }
    }
}
