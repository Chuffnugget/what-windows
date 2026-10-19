import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../sensors"

Rectangle {
    id: row

    property ExpansionScroll expansionScroll: ExpansionScroll {
        targetItem: row
        expanded: row.expanded
    }
    function reveal() { expansionScroll.follow(true); }

    required property var command
    required property bool haAllowed
    required property bool cloudAllowed
    required property string accessHelp
    required property bool canRun
    property bool expanded: false
    required property double now
    readonly property double lastRun: Date.parse(command.last_attempt || "")
    readonly property string runAge: {
        if (command.running)
            return "Now";
        if (!isFinite(lastRun))
            return "Never";
        const seconds = Math.max(0, Math.floor((now - lastRun) / 1000));
        if (seconds < 60)
            return seconds + " s ago";
        if (seconds < 3600)
            return Math.floor(seconds / 60) + " min ago";
        if (seconds < 86400)
            return Math.floor(seconds / 3600) + " h ago";
        return Math.floor(seconds / 86400) + " d ago";
    }
    property bool showShell: true
    property bool showStatus: true
    property bool showOutput: false
    readonly property var result: command.result || ({})
    readonly property string status: {
        if (command.running)
            return "Running";
        if (!command.enabled)
            return "Disabled";
        if (result.ok !== undefined)
            return result.ok ? "Completed" : "Failed";
        return "Not Run";
    }
    signal toggleRequested()
    signal outputRequested(bool output)
    signal runRequested()
    signal editRequested()
    signal removeRequested()

    height: 48 + detailArea.height
    color: expanded ? Theme.selected : header.hovered ? Theme.hover : Theme.surface
    ListView.onReused: {
        resizeAnimation.complete();
    }

    AbstractButton {
        id: header
        objectName: "commandRow_" + row.command.id
        width: parent.width
        height: 48
        hoverEnabled: true
        Accessible.name: row.command.name
        Accessible.description: (row.expanded ? "Collapse" : "Expand") + " command details"
        onClicked: {
            row.toggleRequested();
            if (row.expanded)
                row.expansionScroll.follow();
        }
        background: Rectangle {
            color: "transparent"
            border.width: header.visualFocus ? 1 : 0
            border.color: Theme.accent
        }
    }

    RowLayout {
        width: parent.width
        height: 48
        spacing: Theme.spaceM

        Image {
            Layout.leftMargin: 10
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16
            source: "qrc:/assets/chevron.svg"
            rotation: row.expanded ? 0 : -90
            Behavior on rotation {
                NumberAnimation {
                    duration: Theme.animationsEnabled ? Theme.timings.controls : 0
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1

            Label {
                Layout.fillWidth: true
                text: row.command.name
                textFormat: Text.PlainText
                color: row.command.enabled ? Theme.ink : Theme.muted
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0 && !row.expanded
                text: row.command.description || ""
                textFormat: Text.PlainText
                color: Theme.muted
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
        }
        Label {
            visible: row.showShell
            Layout.preferredWidth: 90
            text: ({powershell: "PowerShell", pwsh: "PowerShell 7", cmd: "CMD", wsl: "WSL"})[
                row.command.shell] || row.command.shell
            color: Theme.muted
            font.pixelSize: Theme.fontCaption
        }
        PermissionBadges {
            objectName: "commandAccess_" + row.command.id
            Layout.preferredWidth: 200
            showRunAccess: true
            concise: true
            runAllowed: row.haAllowed
            localAllowed: row.haAllowed
            cloudAllowed: row.cloudAllowed
            runHelp: row.accessHelp
        }
        Label {
            objectName: "commandLastRun_" + row.command.id
            Layout.preferredWidth: 100
            text: row.runAge
            color: Theme.muted
            font.pixelSize: Theme.fontCaption
            ToolTip.visible: runTimeHover.hovered
            ToolTip.text: isFinite(row.lastRun)
                ? "Last Run: " + Qt.formatDateTime(new Date(row.lastRun), "dd MMM yyyy, HH:mm:ss")
                : "This command has not run during this WHAT session."
            HoverHandler { id: runTimeHover }
        }
        Label {
            visible: row.showStatus
            Layout.preferredWidth: 96
            text: row.status
            color: row.status === "Failed" ? Theme.warning
                : row.command.running || row.result.ok === true ? Theme.accent : Theme.muted
            font.pixelSize: Theme.fontCaption
        }
        ActionButton {
            objectName: "runSavedCommand_" + row.command.id
            Layout.preferredWidth: 64
            Layout.rightMargin: 10
            text: row.command.running ? "Running" : "Run"
            compact: true
            enabled: row.canRun
            ToolTip.text: "Run this command on this PC. Home Assistant permissions do not affect local runs."
            onClicked: row.runRequested()
        }
    }

    Item {
        id: detailArea
        y: 48
        width: parent.width
        height: row.expanded ? 226 : 0
        clip: true

        Behavior on height {
            NumberAnimation {
                id: resizeAnimation
                duration: Theme.animationsEnabled ? Theme.timings.lists : 0
                easing.type: Easing.OutCubic
            }
        }

        // Only the open row creates text editors. Keep it alive through collapse
        // so the animation does not reveal an empty panel.
        Loader {
            id: detailLoader
            active: row.expanded || detailArea.height > 0
            x: 36
            width: parent.width - 48
            height: 216
            sourceComponent: ColumnLayout {
                id: commandDetails

                property ExpansionBlur expansionBlur: ExpansionBlur {
                    target: commandDetails
                    progress: detailArea.height / 226
                }
                spacing: Theme.spaceS

                Label {
                    Layout.fillWidth: true
                    text: row.command.description || "Runs as your Windows user. Edit to change the script or Home Assistant access."
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.muted
                    font.pixelSize: Theme.fontCaption
                }
                RowLayout {
                    Layout.fillWidth: true
                    QuietButton {
                        compact: true
                        text: "Script"
                        checked: !row.showOutput
                        ToolTip.text: "The saved script. Edit the command to change it."
                        onClicked: row.outputRequested(false)
                    }
                    QuietButton {
                        compact: true
                        objectName: "commandOutput_" + row.command.id
                        text: "Last Output"
                        checked: row.showOutput
                        ToolTip.text: "Output and errors from the last run of this saved command."
                        onClicked: row.outputRequested(true)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: {
                            if (row.command.running)
                                return "Running on this PC";
                            if (row.result.error)
                                return row.result.error;
                            if (row.command.last_attempt)
                                return "Last Run: " + Qt.formatDateTime(new Date(row.command.last_attempt),
                                                                       "dd MMM, HH:mm:ss");
                            return "Timeout: " + row.command.timeout + " s";
                        }
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: row.result.error ? Theme.warning : Theme.muted
                        font.pixelSize: Theme.fontCaption
                    }
                    ActionButton {
                        compact: true
                        objectName: "editSavedCommand_" + row.command.id
                        text: "Edit"
                        ToolTip.text: "Edit the script, run options and Home Assistant permissions."
                        onClicked: row.editRequested()
                    }
                    ActionButton {
                        compact: true
                        text: "Remove"
                        ToolTip.text: "Remove this saved command. Its Home Assistant button will become unavailable."
                        onClicked: row.removeRequested()
                    }
                }
                DragScrollView {
                    id: outputScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ScrollBar.vertical: WideScrollBar {
                        visible: size < 1
                        x: outputScroll.width - width
                        y: outputScroll.topPadding
                        height: outputScroll.availableHeight
                    }
                    TextArea {
                        readOnly: true
                        selectByMouse: true
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.WrapAnywhere
                        font.family: "Cascadia Mono"
                        font.pixelSize: Theme.fontCaption
                        color: Theme.ink
                        padding: 10
                        rightPadding: 24
                        text: {
                            if (!row.showOutput)
                                return row.command.command;
                            const execution = row.result.execution;
                            if (!execution)
                                return row.command.running ? "Waiting for output…" : "Run this command to see its output here.";
                            return "Exit Code: " + execution.exit_code + " | " + row.command.duration_ms + " ms"
                                + (execution.truncated ? " | Output truncated at 64 KiB" : "")
                                + "\n\n" + (execution.stdout || "(No output)")
                                + (execution.stderr ? "\nErrors\n" + execution.stderr : "");
                        }
                        background: Rectangle {
                            color: Theme.inset

                        }
                    }
                }
            }
        }
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: row.expanded ? Theme.outline : Theme.separator
    }
}
