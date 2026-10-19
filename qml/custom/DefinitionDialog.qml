import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../components"

GlassDialog {
    id: dialog

    objectName: "customDefinitionDialog"
    required property var manager
    property var definition: null
    property bool saving: true
    property string error: ""
    anchors.centerIn: parent
    width: Math.min(780, parent.width - 40)
    height: Math.min(780, parent.height - 40)
    modal: true
    closePolicy: Popup.CloseOnEscape
    title: !saving ? "Run a Command Once" : definition?.mode === "action" ? "Saved Command" : "Custom Sensor"
    function edit(value, save = true) {
        error = "";
        definition = JSON.parse(JSON.stringify(value));
        saving = save;
        // Reset before opening. A deferred reset can run after the user focuses
        // the script field and undo its caret-following scroll.
        scroll.contentItem.cancelFlick();
        scroll.contentItem.contentY = 0;
        open();
    }

    onClosed: {
        manager.cancelTest();
        definition = null;
    }

    contentItem: DragScrollView {
        id: scroll
        objectName: "customEditorScroll"

        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical: WideScrollBar {
            x: scroll.width - width
            y: scroll.topPadding
            height: scroll.availableHeight
        }

        Loader {
            id: formLoader
            width: scroll.availableWidth - Theme.scrollGutter
            active: dialog.definition !== null
            sourceComponent: dialog.definition?.mode === "action" ? commandForm : sensorForm
        }
    }

    footer: Item {
        implicitHeight: 64
        RowLayout {
            anchors.fill: parent
            anchors.margins: 12
            anchors.leftMargin: Theme.spaceXl
            anchors.rightMargin: Theme.spaceXl

            ActionButton {
                objectName: "testCustomDefinition"
                text: dialog.manager.testing ? "Running…" : dialog.definition?.mode === "action"
                                               ? "Run and Test" : "Test Sensor"
                enabled: formLoader.item !== null && !dialog.manager.testing
                ToolTip.text:
                    "Runs the actual source on this PC. Scripts and commands can change files or settings."
                onClicked: {
                    dialog.error = "";
                    dialog.manager.test(formLoader.item.draftValue());
                }
            }
            QuietButton {
                visible: dialog.manager.testing
                text: "Stop Test"
                onClicked: dialog.manager.cancelTest()
            }
            Item {
                Layout.fillWidth: true
            }
            ActionButton {
                objectName: "saveCustomDefinition"
                visible: dialog.saving
                text: "Save"
                primary: true
                enabled: formLoader.item !== null && !dialog.manager.testing
                onClicked: {
                    const saved = dialog.manager.save(formLoader.item.draftValue());
                    dialog.error = saved.error || "";
                    if (dialog.error)
                        dialog.showResult();
                    else
                        dialog.close();
                }
            }
            QuietButton {
                text: dialog.saving ? "Cancel" : "Close"
                onClicked: dialog.close()
            }
        }
    }

    function showResult() {
        Qt.callLater(function () {
            scroll.contentItem.contentY = Math.max(0, scroll.contentHeight - scroll.availableHeight);
        });
    }

    Connections {
        target: dialog.manager
        function onPreviewChanged() {
            if (dialog.visible && dialog.definition?.id === dialog.manager.preview.definition_id)
                dialog.showResult();
        }
    }

    Component {
        id: commandForm
        CommandEditor {
            manager: dialog.manager
            definition: dialog.definition
            saving: dialog.saving
            error: dialog.error
        }
    }

    Component {
        id: sensorForm
        SensorEditor {
            manager: dialog.manager
            definition: dialog.definition
            error: dialog.error
        }
    }
}
