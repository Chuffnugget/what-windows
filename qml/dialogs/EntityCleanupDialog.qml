import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: entityCleanupDialog

    required property var controller

    objectName: "entityCleanupDialog"
    title: "Clear Stale or Unshared Entities"
    modal: true
    anchors.centerIn: parent
    width: Math.min(620, entityCleanupDialog.parent.width - 48)

    contentItem: ColumnLayout {
        Label {
            text: entityCleanupDialog.controller.entityCleanup.message || "Preparing preview…"
            textFormat: Text.PlainText
            color: Theme.ink
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        DragScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            visible: !!entityCleanupDialog.controller.entityCleanup.entities
            clip: true

            TextArea {
                text: entityCleanupDialog.controller.entityCleanup.entities || ""
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                color: Theme.ink

                background: Item {}
            }
        }

        HintLabel {
            text: "Removal clears these entities’ registrations and custom settings, not recorder history. Selected readings, supported controls and PC diagnostics remain."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        RowLayout {
            ActionButton {
                text: "Close"
                onClicked: entityCleanupDialog.close()
            }

            Item {
                Layout.fillWidth: true
            }

            ActionButton {
                ToolTip.text: "Ask Home Assistant to recalculate the proposed entity removals before you review them."
                text: "Refresh Preview"
                enabled: entityCleanupDialog.controller.entityCleanup.busy !== true
                onClicked: entityCleanupDialog.controller.previewEntityCleanup()
            }

            ActionButton {
                ToolTip.text: "Remove only the stale or unshared entities listed in this preview from Home Assistant."
                objectName: "confirmEntityCleanup"
                text: "Remove Reviewed Entities"
                enabled: entityCleanupDialog.controller.entityCleanup.status === "preview"
                    && entityCleanupDialog.controller.entityCleanup.count > 0
                    && entityCleanupDialog.controller.entityCleanup.busy !== true
                onClicked: entityCleanupDialog.controller.confirmEntityCleanup()
            }
        }
    }
}
