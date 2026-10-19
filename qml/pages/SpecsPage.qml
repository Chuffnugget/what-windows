import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: specsPage

    required property var controller
    signal notification(string message)
    spacing: Theme.spaceM

    HintLabel {
        text: "Hardware, Windows, displays, network adapters and firmware. Serial numbers, usernames and network addresses are excluded."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    RowLayout {
        Layout.fillWidth: true

        BusyIndicator {
            objectName: "specsProgress"
            running: specsPage.visible && controller.specsRefreshing
            visible: controller.specsRefreshing
            Layout.preferredWidth: Theme.controlHeight
            Layout.preferredHeight: Theme.controlHeight
        }

        HintLabel {
            objectName: "specsRefreshStatus"
            text: controller.specsRefreshing
                ? "Collecting specifications in the background…"
                : (controller.specsLastRefreshed.length > 0
                    ? "Last refreshed: " + controller.specsLastRefreshed
                    : "Specifications have not been collected yet.")
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    SettingsSection {
        objectName: "specsCollectionNotice"
        title: "Some Details Are Unavailable"
        detail: "Expand to see the affected devices, missing values and reported errors."
        visible: controller.specsNotice.length > 0

        DragScrollView {
            id: issuesScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(220, issueText.implicitHeight)
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical: WideScrollBar {
                parent: issuesScroll
                x: issuesScroll.width - width
                height: issuesScroll.availableHeight
                visible: size < 1
            }
            TextArea {
                id: issueText
                objectName: "specsIssueDetails"
                text: controller.specsIssues.join("\n\n")
                color: Theme.warning
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                textFormat: TextEdit.PlainText
                rightPadding: 20
                background: Item {}
            }
        }
    }

    Card {
        Layout.fillWidth: true
        Layout.fillHeight: true

        DragScrollView {
            anchors.fill: parent
            anchors.margins: Theme.spaceL
            anchors.rightMargin: 0
            id: specsScroll

            clip: true
            contentWidth: availableWidth
            rightPadding: Theme.scrollGutter
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ScrollBar.vertical: WideScrollBar {
                parent: specsScroll
                x: specsScroll.width - width
                height: specsScroll.availableHeight
            }

            TextArea {
                text: controller.specs
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                color: Theme.ink
                font.family: "Consolas"

                background: Item {}
            }
        }
    }

    RowLayout {
        ActionButton {
            objectName: "refreshSpecsButton"
            ToolTip.text: "Collect a fresh hardware and Windows specification summary in the background."
            text: "Refresh Specs"
            enabled: !controller.specsRefreshing
            onClicked: controller.refreshSpecs()
        }

        Item {
            Layout.fillWidth: true
        }

        ActionButton {
            ToolTip.text: "Copy the displayed hardware summary. Serial numbers, usernames and network addresses are excluded."
            text: "Copy Specifications"
            enabled: controller.specsLastRefreshed.length > 0
            primary: true

            onClicked: {
                controller.copySpecs();
                specsPage.notification("Specifications copied");
            }
        }
    }
}
