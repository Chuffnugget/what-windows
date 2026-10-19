import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../components"

ColumnLayout {
    id: diagnosticsPage

    property bool active: visible
    required property var controller
    readonly property var status: controller.info
    // Reserve room for all three headers and the open section's tools and hint.
    readonly property real readingHeight: Math.max(180, sectionsScroll.height
        - Theme.sectionHeight * 3 - Theme.controlHeight - Theme.spaceXl * 3 - 40)
    readonly property bool liveUpdates: active && liveSection.expanded && !pauseDiagnostics.checked
    onLiveUpdatesChanged: controller.setDiagnosticsVisible(liveUpdates)
    Component.onCompleted: controller.setDiagnosticsVisible(liveUpdates)
    Component.onDestruction: controller.setDiagnosticsVisible(false)
    signal notification(string message)
    signal reportRequested
    spacing: Theme.spaceM

    RowLayout {
        Layout.fillWidth: true

        HintLabel {
            text: "Open a section to inspect activity, connection details or local logging settings."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        ActionButton {
            text: "Prepare Bug Report"
            ToolTip.text: "Build a redacted report with module versions, connection state, sensor inventory and recent activity. Review it before sharing. Nothing is uploaded."
            onClicked: {
                diagnosticsPage.reportRequested();
                controller.prepareDiagnosticReport();
            }
        }
    }

    DragScrollView {
        id: sectionsScroll

        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentWidth: availableWidth
        rightPadding: Theme.scrollGutter
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical: WideScrollBar {
            parent: sectionsScroll
            x: sectionsScroll.width - width
            height: sectionsScroll.availableHeight
        }

        ColumnLayout {
            width: sectionsScroll.availableWidth
            spacing: 0

            SettingsSection {
                id: activitySection

                objectName: "activityLogSection"
                title: "Activity Log"
                detail: "Recent events from WHAT and its modules. Last 500 entries."
                expanded: true
                onExpandedChanged: if (expanded && liveSection && loggingSection) {
                    liveSection.expanded = false;
                    loggingSection.expanded = false;
                }

                RowLayout {
                    Layout.fillWidth: true

                    OptionCheckBox {
                        id: pauseActivity

                        objectName: "pauseActivityLog"
                        text: "Pause Live Updates"
                        ToolTip.text: "Freeze the displayed log for reading or copying. Collection continues in the background."
                    }

                    Item { Layout.fillWidth: true }

                    ActionButton {
                        objectName: "copyActivityLog"
                        text: "Copy Log"
                        ToolTip.text: "Copy the log exactly as displayed, including local paths and device labels."
                        onClicked: {
                            controller.copyLogSnapshot(activityLog.displayedText);
                            diagnosticsPage.notification("Log snapshot copied");
                        }
                    }

                    ActionButton {
                        objectName: "saveActivityLog"
                        text: "Save Log…"
                        ToolTip.text: "Save the displayed activity log to a text file on your PC."
                        onClicked: controller.saveLogSnapshot(activityLog.displayedText)
                    }
                }

                Card {
                    Layout.fillWidth: true
                    Layout.preferredHeight: diagnosticsPage.readingHeight

                    DiagnosticTextView {
                        id: activityLog

                        objectName: "activityLogView"
                        anchors.fill: parent
                        anchors.margins: Theme.spaceM
                        updatesEnabled: diagnosticsPage.active && activitySection.expanded
                        paused: pauseActivity.checked
                        sourceText: controller.logs
                        followTail: true
                    }
                }

                HintLabel {
                    text: "Selecting text freezes this view. Copy/save includes local details; use Prepare Bug Report for a redacted copy."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }

            SettingsSection {
                id: liveSection

                objectName: "liveDiagnosticsSection"
                title: "Live Diagnostics"
                detail: "Module versions, connection state, permissions and sensor collection."
                onExpandedChanged: if (expanded) {
                    activitySection.expanded = false;
                    loggingSection.expanded = false;
                }

                RowLayout {
                    Layout.fillWidth: true

                    OptionCheckBox {
                        id: pauseDiagnostics

                        objectName: "pauseDiagnostics"
                        text: "Pause Live Updates"
                        ToolTip.text: "Freeze the displayed snapshot. Collection continues in the background."
                    }

                    Item { Layout.fillWidth: true }

                    ActionButton {
                        text: "Read Now"
                        ToolTip.text: "Request fresh sensor data and an updated diagnostics snapshot. Installed software version checks are cached for up to a minute."
                        onClicked: controller.refreshDiagnostics()
                    }

                    ActionButton {
                        text: "Copy Diagnostics"
                        ToolTip.text: "Copy the displayed snapshot, including local paths and device labels. Use Prepare Bug Report for a redacted copy."
                        onClicked: controller.copyLogSnapshot(liveDiagnostics.displayedText)
                    }
                }

                Card {
                    Layout.fillWidth: true
                    Layout.preferredHeight: diagnosticsPage.readingHeight

                    DiagnosticTextView {
                        id: liveDiagnostics

                        objectName: "liveDiagnosticsView"
                        anchors.fill: parent
                        anchors.margins: Theme.spaceM
                        updatesEnabled: diagnosticsPage.active && liveSection.expanded
                        paused: pauseDiagnostics.checked
                        // Reading controller.diagnostics starts a collection job, so only
                        // bind it while the section is on screen.
                        sourceText: updatesEnabled ? controller.diagnostics : ""
                    }
                }

                HintLabel {
                    text: "Selecting text freezes this view until the selection is cleared."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }

            SettingsSection {
                id: loggingSection

                objectName: "diagnosticLoggingSection"
                title: "Logging and Crash Reports"
                detail: "Choose what WHAT saves locally. Nothing is uploaded."
                onExpandedChanged: if (expanded) {
                    activitySection.expanded = false;
                    liveSection.expanded = false;
                }

                OptionCheckBox {
                    objectName: "diagnosticLoggingToggle"
                    text: "Save Diagnostic and Crash Logs Locally"
                    checked: diagnosticsPage.status.diagnosticLogging === true
                    ToolTip.text: "Write activity and crash reports to this PC. Turning this off stops new logs but keeps existing files."
                    onClicked: controller.setDiagnosticLogging(checked)
                }

                OptionCheckBox {
                    objectName: "crashDumpsToggle"
                    text: "Include Memory Dumps in Crash Reports"
                    checked: diagnosticsPage.status.crashDumps === true
                    enabled: diagnosticsPage.status.diagnosticLogging === true
                    ToolTip.text: "Save process memory after a native crash. Dumps can contain tokens and other private data."
                    onClicked: controller.setCrashDumpsEnabled(checked)
                }

                HintLabel {
                    text: "Keeps 5 session logs and recent crash reports, labelled by module and process. Logging changes apply to running helpers. Memory dumps are optional and can contain tokens or private data. Existing files remain when logging is disabled."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }

                ActionButton {
                    text: "Open Log Folder"
                    ToolTip.text: "Open the local folder containing activity logs and any saved crash reports."
                    onClicked: controller.openDiagnosticFolder()
                }
            }
        }
    }
}
