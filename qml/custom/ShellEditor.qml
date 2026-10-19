import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../sensors"

ColumnLayout {
    id: editor

    required property var definition
    property alias command: script.text
    readonly property string shell: shellChoice.currentValue || "powershell"
    Layout.fillWidth: true
    spacing: Theme.spaceS

    RowLayout {
        HintLabel {
            text: "Run Using"
        }

        BrowserChoice {
            id: shellChoice

            objectName: "customShell"
            textRole: "title"
            valueRole: "key"
            model: [
                {
                    title: "Windows PowerShell",
                    key: "powershell"
                },
                {
                    title: "PowerShell 7",
                    key: "pwsh"
                },
                {
                    title: "Command Prompt",
                    key: "cmd"
                },
                {
                    title: "WSL",
                    key: "wsl"
                }
            ]
            currentIndex: Math.max(0, model.findIndex(item => item.key === editor.definition.shell))
        }
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: editor.shell === "powershell"
              ? "Windows PowerShell is included with Windows. Example: Get-Date -Format o" : editor.shell
                === "pwsh" ? "PowerShell 7 must be installed separately. Example: Get-Date -Format o" :
                             editor.shell === "cmd"
                             ? "Use Windows Command Prompt syntax. Example: echo Hello" :
                               "Runs in your default installed WSL distribution. Example: date -Iseconds"
    }

    TextArea {
        Layout.fillWidth: true
        Layout.minimumHeight: 130
        id: script

        readonly property var viewport: {
            let ancestor = parent;
            while (ancestor && !(ancestor instanceof Flickable))
                ancestor = ancestor.parent;
            return ancestor;
        }

        // The form owns scrolling. Keep the caret visible after its layout grows,
        // without introducing a second scrollbar inside the script field.
        function revealCursor() {
            if (!activeFocus)
                return;
            const viewport = script.viewport;
            if (!viewport)
                return;
            const cursor = mapToItem(viewport, cursorRectangle.x, cursorRectangle.y);
            const shift = cursor.y < 0 ? cursor.y
                : Math.max(0, cursor.y + cursorRectangle.height - viewport.height);
            viewport.contentY = Math.max(0, Math.min(viewport.contentHeight - viewport.height,
                viewport.contentY + shift));
        }
        // Text layout and the surrounding form can settle in separate frames.
        // Follow their final geometry, not only the earlier cursor notification.
        onYChanged: cursorFollow.restart()
        onHeightChanged: cursorFollow.restart()
        Connections {
            target: script.viewport
            function onContentHeightChanged() { cursorFollow.restart(); }
            function onHeightChanged() { cursorFollow.restart(); }
        }
        onCursorRectangleChanged: cursorFollow.restart()
        onActiveFocusChanged: if (activeFocus) cursorFollow.restart()
        Timer {
            id: cursorFollow
            interval: 0
            onTriggered: script.revealCursor()
        }

        objectName: "customScript"
        text: editor.definition.command || ""
        placeholderText: "Enter the command or script to run (up to 2048 characters)"
        Accessible.name: "Command or script"
        textFormat: TextEdit.PlainText
        wrapMode: TextEdit.Wrap
        font.family: "Cascadia Mono"
        color: Theme.ink
        selectByMouse: true
        padding: 10
        background: Rectangle {
            color: Theme.inset

        }
    }
}
