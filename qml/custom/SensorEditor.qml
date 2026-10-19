import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../sensors"

ColumnLayout {
    id: editor

    objectName: "sensorEditor"
    required property var manager
    required property var definition
    readonly property bool saved: manager.sensors.some(entry => entry.id === definition.id)
    readonly property string sourceKind: source.currentValue || definition.source
    property string error: ""
    spacing: Theme.spaceM

    function newOutput() {
        return {
            id: manager.newId(),
            name: "Reading",
            path: "",
            kind: "text",
            unit: "",
            state_class: ""
        };
    }

    function draftValue() {
        const value = JSON.parse(JSON.stringify(definition));
        value.name = entryName.text.trim();
        value.enabled = enabledBox.checked;
        value.source = sourceKind;
        value.target = target.text;
        value.property = sourceKind === "file" ? fileProperty.currentValue : wmiProperty.text;
        value.namespace = wmiNamespace.text;
        value.query = wmiQuery.text;
        value.shell = shellEditor.shell;
        value.command = shellEditor.command;
        value.format = jsonOutput.checked ? "json" : "text";
        value.interval = interval.value;
        value.timeout = timeout.value;
        if (value.outputs.length === 1 && value.outputs[0].name === "Reading" && value.name)
            value.outputs[0].name = value.name;
        return value;
    }

    function suggestOutput() {
        if (saved || definition.outputs.length !== 1)
            return;
        const value = draftValue();
        const property = value.property;
        let kind = "text";
        if (sourceKind === "process" || (sourceKind === "file" && property === "exists"))
            kind = "binary";
        else if (["counter", "wmi"].includes(sourceKind) || (sourceKind === "file" && property === "size"))
            kind = "number";
        else if (sourceKind === "file")
            kind = "timestamp";
        value.outputs[0].kind = kind;
        value.outputs[0].unit = sourceKind === "file" && property === "size" ? "B" : "";
        value.outputs[0].state_class = "";
        definition = value;
    }

    Label {
        text: "What Do You Want to Monitor?"
        color: Theme.ink
    }
    BrowserChoice {
        id: source
        objectName: "customSource"
        Layout.fillWidth: true
        textRole: "title"
        valueRole: "key"
        model: [
            {
                title: "Whether an App Is Running",
                key: "process"
            },
            {
                title: "A Windows Service",
                key: "service"
            },
            {
                title: "A File",
                key: "file"
            },
            {
                title: "A Performance Counter",
                key: "counter"
            },
            {
                title: "A WMI Query",
                key: "wmi"
            },
            {
                title: "A Script's Output",
                key: "script"
            }
        ]
        currentIndex: Math.max(0, model.findIndex(item => item.key === editor.definition.source))
        onActivated: editor.suggestOutput()
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: editor.sourceKind === "process"
              ? "Checks for an executable and returns true or false. For example, monitor Discord.exe to tell Home Assistant whether Discord is running." :
                editor.sourceKind === "service"
                ? "Reads a Windows service's state, such as Running or Stopped. Use its service name from Services, not its display name." :
                  editor.sourceKind === "file"
                  ? "Checks whether a file exists, its size in bytes, or the time it was last modified." :
                    editor.sourceKind === "counter"
                    ? "Reads a numeric Windows performance counter. Copy its English counter path, including any instance name." :
                      editor.sourceKind === "wmi"
                      ? "Reads a property returned by a Windows SELECT query. The example below gives free physical memory in KiB." :
                        "Runs your script on a schedule and uses its standard output as the reading. Sensor scripts should collect data; they still run with your Windows permissions."
    }

    Label {
        text: "Sensor Name"
        color: Theme.ink
    }
    InputField {
        id: entryName
        objectName: "customName"
        Layout.fillWidth: true
        text: editor.definition.name
        placeholderText: "For example, Discord Running"
        maximumLength: 100
    }

    Label {
        visible: target.visible
        text: editor.sourceKind === "process" ? "Executable"
            : editor.sourceKind === "service" ? "Service name"
            : editor.sourceKind === "file" ? "File path"
            : "Counter path"
        color: Theme.ink
    }
    InputField {
        id: target
        objectName: "customTarget"
        Layout.fillWidth: true
        visible: ["process", "service", "file", "counter"].includes(editor.sourceKind)
        text: editor.definition.target || ""
        maximumLength: 1024
        placeholderText: editor.sourceKind === "process" ? "Executable name, for example Discord.exe" :
                                                           editor.sourceKind === "service"
                                                           ? "Service name, for example Spooler" :
                                                             editor.sourceKind === "file"
                                                             ? "Full file path, for example C:\\Logs\\backup.log" :
                                                               "Counter path, for example \\Processor(_Total)\\% Processor Time"
        Accessible.name: "Source target"
    }

    BrowserChoice {
        id: fileProperty
        visible: editor.sourceKind === "file"
        textRole: "title"
        valueRole: "key"
        model: [
            {
                title: "Whether It Exists",
                key: "exists"
            },
            {
                title: "Size in Bytes",
                key: "size"
            },
            {
                title: "Last Modified Time",
                key: "modified"
            }
        ]
        currentIndex: Math.max(0, model.findIndex(item => item.key === editor.definition.property))
        onActivated: editor.suggestOutput()
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: editor.sourceKind === "wmi"

        HintLabel {
            text: "Query"
        }
        InputField {
            id: wmiQuery
            Layout.fillWidth: true
            text: editor.definition.query || ""
            placeholderText: "SELECT FreePhysicalMemory FROM Win32_OperatingSystem"
            maximumLength: 1024
        }
        HintLabel {
            text: "Property to Read"
        }
        InputField {
            id: wmiProperty
            Layout.fillWidth: true
            text: editor.definition.property || ""
            placeholderText: "FreePhysicalMemory"
            maximumLength: 128
        }
    }

    ShellEditor {
        id: shellEditor
        visible: editor.sourceKind === "script"
        definition: editor.definition
    }

    OptionCheckBox {
        id: jsonOutput
        visible: editor.sourceKind === "script"
        text: "My Script Returns JSON"
        checked: editor.definition.format === "json"
    }
    HintLabel {
        visible: editor.sourceKind === "script"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: jsonOutput.checked
              ? 'Use Advanced to map fields to readings. Example: {"disk":{"free":123}} uses the path disk.free.' :
                "Write one value to standard output. Choose Text, Number, Binary or Timestamp under Advanced to match it."
    }

    RowLayout {
        HintLabel {
            text: "Check Every (Seconds)"
        }
        NumberInput {
            id: interval
            from: 1
            to: 86400
            value: editor.definition.interval
        }
        OptionCheckBox {
            id: enabledBox
            text: "Enabled"
            checked: editor.definition.enabled
        }
    }

    HintLabel {
        visible: (editor.definition.source === "script" || editor.definition.source === "wmi") && interval.value < 5
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.warning
        text: "Short intervals start a new shell frequently and can use noticeable CPU. Use 5 seconds or longer unless this reading needs faster updates."
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: "Saving adds this sensor to the browser. Select Local or Cloud there to share its readings with Home Assistant. Sharing results does not allow Home Assistant to change this definition or send scripts."
    }

    RowLayout {
        visible: !editor.manager.enabled
        Layout.fillWidth: true
        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "The Custom module is off. Saved sensors will stay unavailable until you enable it."
        }
        ActionButton {
            text: "Enable Custom Module"
            onClicked: editor.manager.enabled = true
        }
    }

    SettingsSection {
        title: "Advanced"
        detail: "Output type, units, JSON fields and script timeout"

        RowLayout {
            visible: ["script", "wmi"].includes(editor.sourceKind)
            HintLabel {
                text: "Timeout (Seconds)"
            }
            NumberInput {
                id: timeout
                from: 1
                to: 60
                value: editor.definition.timeout
            }
        }
        InputField {
            id: wmiNamespace
            visible: editor.sourceKind === "wmi"
            Layout.fillWidth: true
            text: editor.definition.namespace || "root/cimv2"
            placeholderText: "WMI namespace, usually root/cimv2"
            maximumLength: 128
        }
        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Basic sources choose their output type automatically. Leave Path empty for a single value. JSON fields use paths such as disks.0.free. Units and statistics apply to numbers only."
        }
        HintLabel {
            visible: editor.saved
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Existing output IDs keep their type, unit and statistics to protect Home Assistant history. Add a new output when changing what a reading represents."
        }
        Repeater {
            model: editor.definition.outputs

            delegate: ColumnLayout {
                id: outputRow
                required property var modelData
                required property int index
                Layout.fillWidth: true

                RowLayout {
                    Layout.fillWidth: true
                    InputField {
                        Layout.fillWidth: true
                        text: outputRow.modelData.name
                        objectName: "customOutputName_" + outputRow.index
                        placeholderText: "Reading name"
                        maximumLength: 100
                        onTextEdited: editor.definition.outputs[outputRow.index].name = text
                    }
                    InputField {
                        Layout.fillWidth: true
                        text: outputRow.modelData.path
                        placeholderText: "JSON path (optional)"
                        maximumLength: 256
                        onTextEdited: editor.definition.outputs[outputRow.index].path = text
                    }
                    QuietButton {
                        text: "Remove Output"
                        enabled: editor.definition.outputs.length > 1
                        onClicked: {
                            let draft = editor.draftValue();
                            draft.outputs.splice(outputRow.index, 1);
                            editor.definition = draft;
                        }
                    }
                }

                RowLayout {
                    BrowserChoice {
                        id: kind
                        textRole: "title"
                        valueRole: "key"
                        model: [
                            {
                                title: "Text",
                                key: "text"
                            },
                            {
                                title: "Number",
                                key: "number"
                            },
                            {
                                title: "Binary (True/False)",
                                key: "binary"
                            },
                            {
                                title: "Timestamp",
                                key: "timestamp"
                            }
                        ]
                        currentIndex: Math.max(0, model.findIndex(item => item.key
                                                                          === outputRow.modelData.kind))
                        onActivated: {
                            const draft = editor.draftValue();
                            const output = draft.outputs[outputRow.index];
                            output.kind = currentValue;
                            if (currentValue !== "number") {
                                output.unit = "";
                                output.state_class = "";
                            }
                            // Replace the draft so dependent unit/statistics fields update too.
                            editor.definition = draft;
                        }
                    }
                    InputField {
                        enabled: kind.currentValue === "number"
                        text: outputRow.modelData.unit
                        placeholderText: "Unit (optional)"
                        maximumLength: 32
                        onTextEdited: editor.definition.outputs[outputRow.index].unit = text
                    }
                    BrowserChoice {
                        enabled: kind.currentValue === "number"
                        textRole: "title"
                        valueRole: "key"
                        model: [
                            {
                                title: "No Long-Term Statistics",
                                key: ""
                            },
                            {
                                title: "Current Reading",
                                key: "measurement"
                            },
                            {
                                title: "Running Total",
                                key: "total"
                            },
                            {
                                title: "Increasing Total",
                                key: "total_increasing"
                            }
                        ]
                        currentIndex: Math.max(0, model.findIndex(item => item.key
                                                                          === outputRow.modelData.state_class))
                        onActivated: editor.definition.outputs[outputRow.index].state_class = currentValue
                        ToolTip.text:
                            "Leave blank to omit long-term statistics. Measurement is a current reading; totals are accumulated counters."
                    }
                }
            }
        }

        QuietButton {
            text: "Add Output"
            enabled: editor.definition.outputs.length < 32
            onClicked: {
                let draft = editor.draftValue();
                draft.outputs.push(editor.newOutput());
                editor.definition = draft;
            }
        }
    }

    EditorResult {
        form: editor
        manager: editor.manager
        error: editor.error
    }
}
