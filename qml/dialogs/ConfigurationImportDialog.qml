import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: configurationImport

    required property var controller
    function preview(text) {
        importPreview.text = text;
        importPermissions.checked = false;
        open();
    }

    objectName: "configurationImportDialog"
    title: "Review Configuration Import"
    modal: true
    anchors.centerIn: parent
    width: Math.min(740, configurationImport.parent.width - 40)
    height: Math.min(650, configurationImport.parent.height - 60)

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceM

        HintLabel {
            text: "Listed settings replace your saved values on the next start. Other settings and this PC's pairing identity are kept. MQTT and remote connections must be enabled again; MQTT credentials must be re-entered. A private local settings backup is saved before applying."
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        DragScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: importPreview

                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                color: Theme.ink
                font.family: "Consolas"
                font.pixelSize: Theme.fontCaption

                background: Rectangle {
                    color: Theme.field
                }
            }
        }

        OptionCheckBox {
            id: importPermissions

            ToolTip.text: "Include the backup's source and control permissions in the import. Leave this off to keep this PC's current permissions."
            objectName: "configurationImportPermissions"
            text: "Also Restore Module, Collection, Sharing and Control Permissions"
        }

        HintLabel {
            text: "Leave unticked to keep your current permissions and shared sensor selections. Imported sensor IDs may be unavailable on a different PC."
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            ActionButton {
                ToolTip.text: "Stage the reviewed configuration for import the next time WHAT starts."
                text: "Import on Next Start"
                primary: true

                onClicked: {
                    configurationImport.controller.stageConfiguration(importPermissions.checked);
                    configurationImport.close();
                }
            }

            ActionButton {
                ToolTip.text: "Close this dialog without confirming the pending action."
                text: "Cancel"
                onClicked: configurationImport.close()
            }
        }
    }
}
