import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../components"

GlassDialog {
    id: updateNotice

    required property var updater
    signal updateSettingsRequested()

    objectName: "updateAvailablePrompt"
    property string version: ""
    property bool completed: false
    property string previousVersion: ""
    property string notes: ""
    title: completed ? "Update Complete" : "Update Available"

    onClosed: {
        if (completed)
            updateNotice.updater.dismissCompletedUpdate();
    }

    modal: true
    anchors.centerIn: parent
    width: Math.min(540, updateNotice.parent.width - 48)
    padding: 30

    header: Item {
        implicitHeight: 0
    }

    closePolicy: Popup.CloseOnEscape

    Overlay.modal: GlassBackdrop {
        sourceItem: updateNotice.backdropSource
    }

    background: GlassSurface {
        Rectangle {
            x: 24
            y: 1
            width: parent.width - 48
            height: 1
            color: Theme.glassEdge
        }
    }

    contentItem: ColumnLayout {
        spacing: 22

        RowLayout {
            spacing: Theme.spaceL

            AnimatedLogo {
                objectName: "updatePromptLogo"
                Layout.preferredWidth: 52
                Layout.preferredHeight: 62
                animationsEnabled: Theme.animationsEnabled
                animationDuration: Theme.timings.logo
                pauseDuration: Theme.timings.logoPause
            }

            ColumnLayout {
                spacing: Theme.spaceXs
                Layout.fillWidth: true

                Label {
                    text: "WHAT"
                    color: Theme.ink
                    font.pixelSize: Theme.fontHeading
                    font.weight: Font.DemiBold
                }

                HintLabel {
                    text: "Windows Home Assistant Toolkit"
                    color: Theme.secondary
                }
            }

            Rectangle {
                implicitWidth: channelLabel.implicitWidth + 20
                implicitHeight: 28

                color: Theme.selected
                border.color: Theme.outline

                Label {
                    id: channelLabel

                    anchors.centerIn: parent
                    text: updateNotice.version.indexOf("-beta.") >= 0 ? "BETA" : "UPDATE"
                    color: Theme.accent
                    font.pixelSize: Theme.fontLabel
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.outlineStrong
        }

        ColumnLayout {
            spacing: Theme.spaceM
            Layout.fillWidth: true

            Label {
                text: updateNotice.completed ? "Update Complete" : "A new version is ready"
                color: Theme.ink
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            HintLabel {
                text: updateNotice.completed
                    ? "Successfully updated to WHAT " + updateNotice.version + "."
                    : "Take a look at what’s changed before you update."
                color: Theme.secondary
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: versionRow.implicitHeight + 28

            color: Theme.inset
            border.color: Theme.outline

            RowLayout {
                id: versionRow

                anchors.fill: parent
                anchors.margins: 14
                spacing: Theme.spaceM

                ColumnLayout {
                    spacing: Theme.spaceXs
                    Layout.fillWidth: true

                    HintLabel {
                        text: updateNotice.completed ? "PREVIOUS" : "RUNNING"
                        font.pixelSize: Theme.fontBadge
                        font.letterSpacing: 1
                    }

                    Label {
                        text: updateNotice.completed ? updateNotice.previousVersion : updateNotice.updater.info.current
                        color: Theme.secondary
                        font.pixelSize: Theme.fontSection
                    }
                }

                Label {
                    text: "→"
                    color: Theme.muted
                    font.pixelSize: Theme.fontHeading
                }

                ColumnLayout {
                    spacing: Theme.spaceXs
                    Layout.fillWidth: true

                    HintLabel {
                        text: updateNotice.completed ? "CURRENT" : "AVAILABLE"
                        font.pixelSize: Theme.fontBadge
                        font.letterSpacing: 1
                    }

                    Label {
                        text: updateNotice.version
                        color: Theme.accent
                        font.pixelSize: Theme.fontSection
                        font.weight: Font.DemiBold
                    }
                }
            }
        }

        DragScrollView {
            id: completedNotesScroll

            objectName: "completedUpdateNotesScroll"
            visible: updateNotice.completed
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(150, updateNotice.parent.height * 0.2)
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            rightPadding: 22

            ScrollBar.vertical: WideScrollBar {
                parent: completedNotesScroll
                x: completedNotesScroll.width - width
                height: completedNotesScroll.availableHeight
            }

            TextArea {
                objectName: "completedUpdateNotes"
                width: completedNotesScroll.availableWidth
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                text: updateNotice.notes || "Release notes are available on GitHub."
                color: Theme.ink

                background: Rectangle {
                    color: Theme.inset
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceM

            ActionButton {
                ToolTip.text: "Close this update notice. You can review releases again in Settings."
                objectName: "dismissUpdatePrompt"
                text: updateNotice.completed ? "Done" : "Not now"
                onClicked: updateNotice.close()
            }

            Item {
                Layout.fillWidth: true
            }

            ActionButton {
                ToolTip.text: "Open the release notes on GitHub, or review the offered update before installing."
                objectName: "reviewUpdatePrompt"
                text: updateNotice.completed ? "View release on GitHub" : "Review update"
                primary: true

                onClicked: {
                    if (updateNotice.completed)
                        updateNotice.updater.openReleases(updateNotice.version);
                    else
                        updateNotice.updateSettingsRequested();
                    updateNotice.close();
                }
            }
        }
    }
}
