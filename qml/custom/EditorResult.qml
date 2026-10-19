import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: preview

    required property var form
    required property var manager
    property string error: ""
    readonly property var result: manager.preview.definition_id === form.definition.id ? manager.preview : (
                                                                                             {})

    Layout.fillWidth: true
    spacing: Theme.spaceM

    HintLabel {
        Layout.fillWidth: true
        visible: text.length > 0
        wrapMode: Text.WrapAnywhere
        color: Theme.warning
        text: preview.error || preview.result.error || ""
        textFormat: Text.PlainText
    }

    Label {
        visible: preview.result.ok !== undefined
        text: "Last Test: " + (preview.result.ok ? "Completed" : "Failed") + " in " + (
                  preview.result.duration_ms || 0) + " ms"
        color: Theme.ink
    }

    // Show the published value and its availability, rather than raw JSON alone.
    Repeater {
        model: preview.result.readings || []

        delegate: Label {
            required property var modelData
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            textFormat: Text.PlainText
            color: modelData.available ? Theme.accent : Theme.warning
            text: modelData.name + ": " + (modelData.available
                ? (String(modelData.value) + (modelData.unit ? " " + modelData.unit : ""))
                : "Unavailable")
        }
    }

    HintLabel {
        visible: preview.result.execution !== undefined
        text: "Exit Code: " + (preview.result.execution?.exit_code ?? "Not started") + (
                  preview.result.execution?.truncated ? " | Output truncated at 64 KiB" : "")
    }

    TextArea {
        visible: preview.result.execution !== undefined
        Layout.fillWidth: true
        Layout.minimumHeight: 150
        readOnly: true
        selectByMouse: true
        textFormat: TextEdit.PlainText
        wrapMode: TextEdit.WrapAnywhere
        font.family: "Cascadia Mono"
        color: Theme.ink
        padding: 10
        text: "Output\n" + (preview.result.execution?.stdout || "(none)") + "\n\nErrors\n" + (
                  preview.result.execution?.stderr || "(none)")
        background: Rectangle {
            color: Theme.inset

        }
    }
}
