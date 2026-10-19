import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: reportDialog

    required property var controller
    signal notification(string message)

    objectName: "diagnosticReportDialog"
    anchors.centerIn: parent
    width: Math.min(reportDialog.parent.width - 60, 1000)
    height: Math.min(reportDialog.parent.height - 60, 760)
    title: "Bug Report Preview"
    modal: true
    standardButtons: Dialog.NoButton

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceM

        Label {
            text: "Private values are excluded and common identifiers redacted. Review custom device names before sharing."
            color: Theme.ink
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        Label {
            visible: reportDialog.controller.diagnosticReportBusy
            text: "Preparing report…"
            color: Theme.accent
        }

        DragScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                objectName: "diagnosticReportText"
                text: reportDialog.controller.diagnosticReport
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                color: Theme.ink
                font.family: "Consolas"
                font.pixelSize: Theme.fontCaption

                background: Rectangle {
                    color: Theme.inset
                }
            }
        }

        RowLayout {
            ActionButton {
                text: "Close"
                onClicked: reportDialog.close()
            }

            ActionButton {
                ToolTip.text: "Generate a new redacted report from the latest cached module and connection state."
                text: "Refresh Report"
                enabled: !reportDialog.controller.diagnosticReportBusy
                onClicked: reportDialog.controller.prepareDiagnosticReport()
            }

            Item {
                Layout.fillWidth: true
            }

            ActionButton {
                ToolTip.text: "Copy the redacted report shown in this preview to the clipboard."
                text: "Copy Report"
                enabled: !reportDialog.controller.diagnosticReportBusy && reportDialog.controller.diagnosticReport.length > 0

                onClicked: {
                    reportDialog.controller.copyDiagnostics();
                    reportDialog.notification("Redacted report copied");
                }
            }

            ActionButton {
                ToolTip.text: "Save the redacted report shown in this preview as a text file."
                text: "Save Report…"
                enabled: !reportDialog.controller.diagnosticReportBusy && reportDialog.controller.diagnosticReport.length > 0
                onClicked: reportDialog.controller.exportLogs()
            }
        }
    }
}
