import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../components"
import "../custom"
import "../sensors"

ColumnLayout {
    id: page

    objectName: "commandsPage"
    required property var controller
    readonly property var manager: controller.customSources
    property Item backdropSource: null
    property string expandedCommand: ""
    property string outputCommand: ""
    property bool followActivity: true
    property double now: Date.now()

    // One clock for the visible list, rather than a timer in every delegate.
    Timer {
        interval: 1000
        repeat: true
        triggeredOnStart: true
        running: page.visible && page.manager.commands.length > 0
        onTriggered: page.now = Date.now()
    }
    signal editRequested(var definition, bool saving)
    spacing: Theme.spaceM

    function permissionHelp(command) {
        if (!command.enabled)
            return "This command is disabled. Enable it in its editor before running it.";
        if (!manager.enabled)
            return "The Custom module is disabled. Local and Home Assistant execution are both blocked.";
        if (!command.ha_enabled)
            return "Home Assistant cannot run this command. You can still run it on this PC.";
        if (!manager.controlEnabled)
            return "Home Assistant execution is blocked by the saved-command permission.";
        if (command.cloud_enabled && manager.cloudEnabled)
            return "Home Assistant can trigger this saved command over local or cloud connections. Its script can only be edited on this PC.";
        return "Home Assistant can trigger this saved command over local connections. Cloud execution is blocked. Its script can only be edited on this PC.";
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.rightMargin: Theme.scrollGutter

        Label {
            Layout.fillWidth: true
            text: commands.count > 0
                ? commands.count + (commands.count === 1 ? " Saved Command" : " Saved Commands")
                : "Saved Commands"
            color: Theme.ink
            font.pixelSize: Theme.fontBody
            font.weight: Font.DemiBold
        }
        ActionButton {
            objectName: "addSavedCommand"
            text: "+ New Command"
            compact: true
            primary: true
            ToolTip.text: "Create a named command, test it, and choose whether Home Assistant may trigger it."
            onClicked: page.editRequested(page.manager.newDefinition("action"), true)
        }
        ActionButton {
            compact: true
            objectName: "runOneOffCommand"
            text: "Run Once"
            ToolTip.text: "Run a local command without saving it or granting Home Assistant any permission."
            onClicked: page.editRequested(page.manager.newDefinition("action"), false)
        }
        ActionButton {
            visible: !page.manager.enabled
            text: "Enable Custom Module"
            compact: true
            ToolTip.text: "Enable saved commands and custom sensor collection. Home Assistant execution keeps its separate permissions."
            onClicked: page.manager.enabled = true
        }
    }

    ListView {
        id: commands

        objectName: "savedCommandList"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 100
        clip: true
        reuseItems: true
        // Command results replace this model. Avoid cancelling partially built
        // off-screen controls when a running command completes (Qt 6.11).
        cacheBuffer: 0
        boundsBehavior: Flickable.StopAtBounds
        property FastWheel mouseWheel: FastWheel {
            parent: commands
            flick: commands
        }
        model: page.manager.commands
        spacing: 0
        ScrollBar.vertical: WideScrollBar { visible: size < 1 }

        // Permissions and activity share the list's scroll area so an expanded
        // section cannot push the command controls below a small window.
        header: Column {
            width: commands.width - Theme.scrollGutter
            CommandSection {
                id: permissionSection
                width: parent.width
                title: "Home Assistant Permissions"
                summary: "Saved: " + (page.manager.enabled && page.manager.controlEnabled
                                      ? "Per Command" : "Blocked")
                         + "  /  Shell: " + (page.controller.info.windowsEnabled
                                             && page.controller.info.controlPermissions.shell
                                             ? "Allowed" : "Blocked")
                CommandPermissions {
                    controller: page.controller
                }
            }
            RowLayout {
                visible: commands.count > 0
                width: parent.width
                height: 36
                spacing: Theme.spaceM
                Label {
                    Layout.leftMargin: 36
                    Layout.fillWidth: true
                    text: "COMMAND"
                    color: Theme.muted
                    font.pixelSize: Theme.fontLabel
                    font.letterSpacing: 1
                }
                Label {
                    visible: commands.width > 1000
                    Layout.preferredWidth: 90
                    text: "SHELL"
                    color: Theme.muted
                    font.pixelSize: Theme.fontLabel
                    font.letterSpacing: 1
                }
                Label {
                    Layout.preferredWidth: 200
                    text: "HOME ASSISTANT ACCESS"
                    color: Theme.muted
                    font.pixelSize: Theme.fontLabel
                    font.letterSpacing: 1
                }
                Label {
                    Layout.preferredWidth: 100
                    text: "LAST RUN"
                    color: Theme.muted
                    font.pixelSize: Theme.fontLabel
                    font.letterSpacing: 1
                }
                Label {
                    visible: commands.width > 760
                    Layout.preferredWidth: 96
                    text: "STATUS"
                    color: Theme.muted
                    font.pixelSize: Theme.fontLabel
                    font.letterSpacing: 1
                }
                Item {
                    Layout.preferredWidth: 64
                    Layout.rightMargin: 10
                }
            }
        }
        footer: ColumnLayout {
            width: commands.width - Theme.scrollGutter
            spacing: Theme.spaceM
            AddListEntry {
                // Empty state only; the header already has a New command button.
                visible: commands.count === 0
                title: "No Saved Commands Yet"
                detail: "Add a saved command to this list. Home Assistant access is optional."
                buttonText: "+ New Command"
                buttonName: "addSavedCommandFooter"
                primary: true
                compact: true
                onClicked: page.editRequested(page.manager.newDefinition("action"), true)
            }
            CommandSection {
                id: activity

                readonly property bool hasOutput: page.controller.commandStream.length > 0
                Layout.fillWidth: true
                title: "Home Assistant Command Activity"
                summary: "Recent commands supplied by Home Assistant and their output"

                HintLabel {
                    Layout.fillWidth: true
                    visible: !activity.hasOutput
                    wrapMode: Text.WordWrap
                    text: "No activity yet. Commands and output appear here when Home Assistant runs a shell command."
                }
                HintLabel {
                    Layout.fillWidth: true
                    visible: activity.hasOutput
                    wrapMode: Text.WordWrap
                    text: "Kept in memory only, up to 65,536 characters. Commands and output may contain private information."
                }
                DragScrollView {
                    id: activityScroll
                    visible: activity.hasOutput
                    Layout.fillWidth: true
                    Layout.preferredHeight: 160
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                    ScrollBar.vertical: WideScrollBar {
                        visible: size < 1
                        x: activityScroll.width - width
                        y: activityScroll.topPadding
                        height: activityScroll.availableHeight
                    }
                    TextArea {
                        rightPadding: 24
                        objectName: "commandStream"
                        text: page.controller.commandStream
                        readOnly: true
                        selectByMouse: true
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.WrapAnywhere
                        font.family: "Cascadia Mono"
                        color: Theme.ink
                        onTextChanged: {
                            if (page.followActivity)
                                cursorPosition = length;
                        }
                    }
                }
                RowLayout {
                    visible: activity.hasOutput
                    OptionCheckBox {
                        text: "Follow Output"
                        checked: page.followActivity
                        onToggled: page.followActivity = checked
                    }
                    ActionButton {
                        compact: true
                        text: "Clear Activity"
                        ToolTip.text: "Clear the in-memory log of Home Assistant-supplied shell commands."
                        onClicked: page.controller.clearCommandStream()
                    }
                }
            }
        }

        delegate: CommandRow {
            required property var modelData
            width: commands.width - Theme.scrollGutter
            command: modelData
            now: page.now
            haAllowed: page.manager.enabled && page.manager.controlEnabled && modelData.enabled && modelData.ha_enabled
            cloudAllowed: haAllowed && page.manager.cloudEnabled && modelData.cloud_enabled
            accessHelp: page.permissionHelp(modelData)
            canRun: page.manager.enabled && modelData.enabled && !modelData.running
            expanded: page.expandedCommand === modelData.id
            showOutput: page.outputCommand === modelData.id
            showShell: commands.width > 1000
            showStatus: commands.width > 760
            onToggleRequested: page.expandedCommand = expanded ? "" : modelData.id
            onOutputRequested: output => page.outputCommand = output ? modelData.id : ""
            onRunRequested: page.controller.runCustomAction(modelData.id)
            onEditRequested: page.editRequested(modelData, true)
            onRemoveRequested: {
                removal.identifier = modelData.id;
                removal.commandName = modelData.name;
                removal.open();
            }
        }
    }

    GlassDialog {
        id: removal

        property string identifier
        property string commandName
        backdropSource: page.backdropSource
        anchors.centerIn: parent
        width: Math.min(520, parent.width - 40)
        modal: true
        title: "Remove Saved Command?"
        standardButtons: Dialog.Yes | Dialog.Cancel
        contentItem: Label {
            color: Theme.ink
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: "Remove " + removal.commandName + " from this PC? Its Home Assistant button will become unavailable."
        }
        onAccepted: page.manager.remove(identifier)
    }
}
