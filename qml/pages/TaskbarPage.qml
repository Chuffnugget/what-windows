import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../components"

ColumnLayout {
    id: root

    property Item backdropSource: null
    required property var manager
    required property var colourPalette
    property bool animationsEnabled: true

    spacing: Theme.spaceM
    property bool active: visible
    property string expandedIcon: ""
    property ExpansionScroll iconExpansion: ExpansionScroll {
        targetItem: null
        expanded: targetItem !== null && targetItem.expanded
    }

    onActiveChanged: {
        manager.browsing = active;
        if (!active)
            picker.close();
    }

    Component.onCompleted: manager.browsing = active

    // Sensor-browser shortcuts prefill this picker. Icons change only after confirmation.
    function openForSensor(sensorId, sensorName, appIcon, replaceDefault) {
        picker.mode = replaceDefault ? "edit" : appIcon ? "app" : "tray";
        picker.targetApp = appIcon;
        picker.targetId = "main";
        picker.initialReading = { id: sensorId, name: sensorName };
        picker.open();
    }

    Label {
        text: "Add readings beside the clock or as separate app icons. You can also customise WHAT's own app and system tray icons."
        color: Theme.muted
        Layout.fillWidth: true
        Layout.rightMargin: Theme.scrollGutter
        wrapMode: Text.WordWrap
    }

    Label {
        visible: text.length > 0
        text: manager.error || manager.appIcons.error
        color: Theme.error
        Layout.fillWidth: true
        Layout.rightMargin: Theme.scrollGutter
        wrapMode: Text.WordWrap
    }

    HintLabel {
        objectName: "iconTextWarning"
        text: "Icons work best with 1-2 characters, such as a two-digit number. You can choose any reading, but longer text or values may be difficult to read at taskbar and system tray sizes."
        color: Theme.muted
        Layout.fillWidth: true
        Layout.rightMargin: Theme.scrollGutter
        wrapMode: Text.WordWrap
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.rightMargin: Theme.scrollGutter

        Label {
            text: "Your Icons"
            color: Theme.ink
            font.pixelSize: 18
            font.weight: Font.DemiBold
            Layout.fillWidth: true
        }

        ActionButton {
            ToolTip.text: "Choose sensor readings to show as extra icons beside the Windows clock. Home Assistant sharing is not required."
            text: "Add System Tray…"
            objectName: "addTraySensor"
            enabled: manager.entries.length < 65

            onClicked: {
                picker.mode = "tray";
                picker.open();
            }
        }

        ActionButton {
            ToolTip.text: "Choose readings to show as separate Windows taskbar icons. They stay active while WHAT is running."
            text: "Add App Icon…"
            objectName: "addAppIcon"
            enabled: manager.appIcons.entries.length < 33

            onClicked: {
                picker.mode = "app";
                picker.open();
            }
        }
    }

    GlassDialog {
        id: picker
        backdropSource: root.backdropSource

        objectName: "traySensorPicker"
        anchors.centerIn: parent
        width: Math.min(820, parent.width - 48)
        height: Math.min(650, parent.height - 48)
        modal: true
        title: {
            if (mode === "tray")
                return "Add System Tray Icons";
            if (mode === "app")
                return "Add App Icons";
            return "Choose Icon Reading";
        }

        property string mode: "tray"
        property string targetId: "main"
        property bool targetApp: true
        readonly property int spaces: {
            if (mode === "tray")
                return 65 - manager.entries.length;
            if (mode === "app")
                return 33 - manager.appIcons.entries.length;
            return 1;
        }

        property var selected: ({})
        property var initialReading: ({})
        property var collapsed: ({})
        readonly property int selectedCount: Object.keys(selected).length

        // The manager searches off the GUI thread and publishes the picker model.
        function refresh() {
            manager.searchPicker(search.text, collapsed);
        }

        onAboutToShow: {
            selected = ({});
            const id = initialReading.id;
            if (id && spaces > 0 && !(mode === "tray" && manager.entries.some(entry => entry.id === id)))
                selected = { [id]: true };
            search.text = initialReading.name || "";
            // Consume the shortcut selection so a later normal open starts unfiltered.
            initialReading = ({});
            refresh();
        }

        onOpened: search.forceActiveFocus()

        onClosed: {
            debounce.stop();
            manager.closePicker();
        }

        contentItem: ColumnLayout {
            spacing: Theme.spaceM

            InputField {
                id: search

                objectName: "traySensorSearch"
                Layout.fillWidth: true
                placeholderText: "Search sensors, devices or modules…"
                color: Theme.ink
                selectByMouse: true

                onTextChanged: if (picker.opened)
                    debounce.restart()
            }

            Timer {
                id: debounce

                interval: 180
                onTriggered: picker.refresh()
            }

            Label {
                text: picker.mode === "edit"
                    ? "Choose one reading for this icon. Sharing with Home Assistant is not required."
                    : "Select several sensors to add them together. Sharing with Home Assistant is not required."
                color: Theme.muted
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            ListView {
                id: tree

                objectName: "traySensorResults"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: manager.pickerModel
                reuseItems: true
                boundsBehavior: Flickable.StopAtBounds
                property FastWheel mouseWheel: FastWheel {
                    parent: tree
                    flick: tree
                }
                enabled: !manager.pickerBusy

                ScrollBar.vertical: WideScrollBar {}

                delegate: ItemDelegate {
                    id: sensorRow

                    required property var modelData
                    width: tree.width - Theme.scrollGutter
                    height: 42
                    visible: !manager.pickerBusy
                    property bool added: picker.mode === "tray"
                        && !modelData.header
                        && manager.entries.some(e => e.id === modelData.id)

                    onClicked: {
                        if (modelData.header) {
                            let c = Object.assign({}, picker.collapsed);
                            c[modelData.key] = !c[modelData.key];
                            picker.collapsed = c;
                            picker.refresh();
                        } else if (!added) {
                            let chosen = Object.assign({}, picker.selected);
                            if (chosen[modelData.id])
                                delete chosen[modelData.id];
                            else if (picker.mode === "edit") {
                                chosen = ({});
                                chosen[modelData.id] = true;
                            } else if (picker.selectedCount < picker.spaces)
                                chosen[modelData.id] = true;
                            picker.selected = chosen;
                        }
                    }

                    background: Rectangle {
                        color: sensorRow.hovered ? Theme.hover : sensorRow.modelData.header ? Theme.surface : "transparent"
                    }

                    contentItem: RowLayout {
                        spacing: Theme.spaceM

                        Item {
                            Layout.preferredWidth: sensorRow.modelData.depth * 18
                        }

                        Item {
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 22

                            Image {
                                anchors.centerIn: parent
                                visible: sensorRow.modelData.header
                                source: "qrc:/assets/chevron.svg"
                                sourceSize.width: 16
                                sourceSize.height: 16
                                rotation: !search.text && picker.collapsed[sensorRow.modelData.key] ? -90 : 0
                            }

                            Rectangle {
                                anchors.centerIn: parent
                                visible: !sensorRow.modelData.header
                                width: 20
                                height: 20

                                color: Theme.field
                                border.color: Theme.outlineStrong

                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 10
                                    height: 10

                                    // Rows that are already icons are greyed out, so they don't read
                                    // as part of the "selected" count in the footer.
                                    color: sensorRow.added ? Theme.muted : Theme.accent
                                    visible: sensorRow.added || !!picker.selected[sensorRow.modelData.id]
                                }
                            }
                        }

                        Label {
                            text: sensorRow.modelData.name
                            color: sensorRow.added ? Theme.muted : Theme.ink
                            font.weight: sensorRow.modelData.header ? Font.DemiBold : Font.Normal
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Label {
                            text: sensorRow.modelData.header
                                ? ""
                                : sensorRow.added
                                ? "Already added"
                                : sensorRow.modelData.reading
                            color: Theme.muted
                            Layout.maximumWidth: 190
                            elide: Text.ElideRight
                        }
                    }

                    ToolTip.visible: hovered && !modelData.header
                    ToolTip.text: modelData.label || modelData.name
                }

                Label {
                    anchors.centerIn: parent
                    visible: tree.count === 0 || manager.pickerBusy
                    text: manager.pickerBusy ? "Loading sensors…" : "No matching sensors"
                    color: Theme.muted
                }
            }

            RowLayout {
                Label {
                    text: picker.selectedCount + " selected · " + picker.spaces + " spaces available"
                    color: Theme.muted
                    Layout.fillWidth: true
                }

                ActionButton {
                    ToolTip.text: "Create an additional taskbar icon from an image on your PC instead of a sensor reading."
                    visible: picker.mode === "app"
                    text: "Use Custom Image…"

                    onClicked: {
                        manager.appIcons.addCustom();
                        picker.close();
                    }
                }

                ActionButton {
                    ToolTip.text: "Close this dialog without confirming the pending action."
                    text: "Cancel"
                    onClicked: picker.close()
                }

                ActionButton {
                    ToolTip.text: "Add the selected readings to Windows icons, or use the chosen reading for the icon being edited. Home Assistant sharing is unchanged."
                    objectName: "confirmTraySensors"
                    primary: true
                    text: picker.mode === "edit" ? "Use Selected Sensor" : "Add Selected"
                    enabled: picker.selectedCount > 0 && picker.selectedCount <= picker.spaces && !manager.pickerBusy

                    onClicked: {
                        let ids = Object.keys(picker.selected);
                        if (picker.mode === "tray")
                            manager.addSelected(ids);
                        else if (picker.mode === "app")
                            manager.appIcons.addSelected(ids);
                        else
                            (picker.targetApp ? manager.appIcons : manager).setSensor(picker.targetId, ids[0]);

                        picker.close();
                    }
                }
            }
        }
    }

    DragScrollView {
        id: iconsScroll

        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentWidth: availableWidth
        rightPadding: Theme.scrollGutter
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ScrollBar.vertical: WideScrollBar {
            parent: iconsScroll
            x: iconsScroll.width - width
            height: iconsScroll.availableHeight
        }

        ColumnLayout {
            width: iconsScroll.availableWidth
            spacing: 2

            Repeater {
                objectName: "managedIcons"
                model: {
                    const app = root.manager.appIcons.entries.map(e => Object.assign({}, e, {
                            appIcon: true
                        }));
                    const tray = root.manager.entries.map(e => Object.assign({}, e, {
                            appIcon: false
                        }));
                    return [app[0], tray[0]].concat(app.slice(1), tray.slice(1));
                }

                delegate: Rectangle {
                    id: entry

                    required property var modelData
                    readonly property var editor: modelData.appIcon ? root.manager.appIcons : root.manager
                    readonly property var reading: editor.values[modelData.id] || ({})
                    Layout.fillWidth: true
                    readonly property string key: (modelData.appIcon ? "app:" : "tray:") + modelData.id
                    readonly property bool expanded: root.expandedIcon === key
                    implicitHeight: iconHeader.height + iconDetails.height + 1
                    color: Theme.surface

                    AbstractButton {
                        id: iconHeader
                        objectName: "iconHeader_" + entry.key
                        width: parent.width
                        height: 72
                        padding: Theme.spaceM
                        hoverEnabled: true
                        Accessible.name: entry.modelData.name
                        Accessible.description: (entry.expanded ? "Collapse" : "Expand") + " icon settings"
                        onClicked: {
                            root.expandedIcon = entry.expanded ? "" : entry.key;
                            if (entry.expanded) {
                                root.iconExpansion.targetItem = entry;
                                root.iconExpansion.follow();
                            }
                        }
                        background: Rectangle {
                            color: iconHeader.hovered ? Theme.hover : entry.expanded ? Theme.expandedHeader : Theme.surface
                            border.width: iconHeader.visualFocus ? 1 : 0
                            border.color: Theme.accent
                        }
                        contentItem: RowLayout {
                            Image {
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16
                                source: "qrc:/assets/chevron.svg"
                                rotation: entry.expanded ? 0 : -90
                                Behavior on rotation {
                                    NumberAnimation { duration: Theme.animationsEnabled ? Theme.timings.controls : 0 }
                                }
                            }
                            Rectangle {
                                Layout.preferredWidth: 48
                                Layout.preferredHeight: 48
                                color: Theme.separator

                                Image {
                                    anchors.fill: parent
                                    source: entry.reading.preview || ""
                                    fillMode: Image.PreserveAspectFit
                                    smooth: true
                                    mipmap: true
                                }

                                ToolTip.visible: previewHover.hovered
                                ToolTip.text: entry.reading.tooltip || "Unavailable"

                                HoverHandler {
                                    id: previewHover

                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3

                                Label {
                                    text: entry.modelData.name + (entry.modelData.id === "main"
                                        ? ""
                                        : entry.modelData.appIcon
                                        ? " · App Icon"
                                        : " · System Tray")
                                    color: Theme.ink
                                    font.weight: Font.DemiBold
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }

                                Label {
                                    text: entry.reading.tooltip || "Unavailable"
                                    color: Theme.muted
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                            }

                            ActionButton {
                                ToolTip.text: "Remove this extra icon from Windows and WHAT. The sensor and its Home Assistant sharing are kept."
                                visible: entry.modelData.id !== "main"
                                text: "Remove"
                                onClicked: entry.editor.remove(entry.modelData.id)
                            }
                        }

                    }

                    Item {
                        id: iconDetails
                        y: iconHeader.height
                        width: parent.width
                        property real expansion: entry.expanded ? 1 : 0
                        height: expansion * (iconEditor.implicitHeight + Theme.spaceXl)
                        clip: true
                        visible: height > 0
                        Behavior on expansion {
                            NumberAnimation {
                                duration: Theme.animationsEnabled ? Theme.timings.panels : 0
                                easing.type: Easing.OutCubic
                            }
                        }
                        ExpansionBlur {
                            target: iconEditor
                            progress: iconDetails.expansion
                        }
                        Loader {
                            id: iconEditor
                            x: Theme.spaceM
                            y: Theme.spaceM
                            width: parent.width - Theme.spaceXl
                            // Keep controls through the closing animation, then release them.
                            active: entry.expanded || iconDetails.expansion > 0
                            sourceComponent: ColumnLayout {
                                spacing: Theme.spaceM
                                InputField {
                                    visible: entry.modelData.appIcon && entry.modelData.id !== "main"
                                    Layout.fillWidth: true
                                    text: entry.modelData.name
                                    placeholderText: "App Icon Name"
                                    onEditingFinished: entry.editor.rename(entry.modelData.id, text)
                                }

                                Label {
                                    visible: !!entry.reading.error
                                    text: entry.reading.error || ""
                                    color: Theme.error
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                }

                                Flow {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.preferredWidth: 0
                                    spacing: Theme.spaceS

                                    ActionButton {
                                        ToolTip.text: "Replace this icon with a live sensor reading. This does not change its Home Assistant sharing permissions."
                                        visible: entry.modelData.appIcon || entry.modelData.id === "main"
                                        text: "Choose Sensor…"

                                        onClicked: {
                                            picker.targetApp = entry.modelData.appIcon;
                                            picker.mode = "edit";
                                            picker.targetId = entry.modelData.id;
                                            picker.open();
                                        }
                                    }

                                    ActionButton {
                                        ToolTip.text: "Replace this icon with an image from your PC. You can choose a sensor again later."
                                        visible: entry.modelData.appIcon || entry.modelData.id === "main"
                                        text: "Choose Image…"
                                        onClicked: entry.editor.chooseImage(entry.modelData.id)
                                    }

                                    ActionButton {
                                        ToolTip.text: "Restore the original WHAT logo for this icon, replacing its custom image or sensor reading."
                                        visible: entry.modelData.id === "main"
                                        text: "Restore WHAT Logo"
                                        onClicked: entry.editor.restoreLogo()
                                    }

                                    ActionButton {
                                        ToolTip.text: "Choose the colour used to draw the sensor value on this icon."
                                        enabled: (!entry.modelData.appIcon && entry.modelData.id !== "main")
                                            || !!entry.modelData.sensor
                                        text: "Text Colour…"

                                        onClicked: {
                                            const target = entry.editor;
                                            const id = entry.modelData.id;
                                            const colour = root.colourPalette.choose(entry.modelData.foreground,
                                                "Icon Text Colour and Opacity",
                                                true);
                                            target.setColor(id, false, colour);
                                        }
                                    }

                                    ActionButton {
                                        ToolTip.text: "Choose the icon background colour and transparency. A fully transparent background leaves just the reading visible."
                                        enabled: (!entry.modelData.appIcon && entry.modelData.id !== "main")
                                            || !!entry.modelData.sensor
                                        text: "Background / Opacity…"

                                        onClicked: {
                                            const target = entry.editor;
                                            const id = entry.modelData.id;
                                            const colour = root.colourPalette.choose(entry.modelData.background,
                                                "Icon Background Colour and Opacity",
                                                true);
                                            target.setColor(id, true, colour);
                                        }
                                    }

                                    ActionButton {
                                        ToolTip.text: "Choose the font for this sensor icon. WHAT reduces the text size as needed to fit the icon."
                                        enabled: (!entry.modelData.appIcon && entry.modelData.id !== "main")
                                            || !!entry.modelData.sensor
                                        text: "Font…"
                                        onClicked: entry.editor.chooseFont(entry.modelData.id)
                                    }

                                    ActionButton {
                                        ToolTip.text: "Reset the icon text colour, background and font to WHAT defaults. Keep the chosen sensor or image."
                                        text: "Reset Style"
                                        onClicked: entry.editor.resetStyle(entry.modelData.id)
                                    }
                                }
                            }
                        }
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.separator
                    }
                }
            }

            AddListEntry {
                Layout.topMargin: Theme.spaceM
                title: root.manager.entries.length === 1 ? "No Sensor Icons Yet" : "Add a System Tray Icon"
                detail: "Choose a reading to show beside the Windows clock."
                buttonText: "+ System Tray Icon"
                buttonName: "addTraySensorFooter"
                canAdd: root.manager.entries.length < 65
                onClicked: {
                    picker.mode = "tray";
                    picker.open();
                }
            }

            AddListEntry {
                title: root.manager.appIcons.entries.length === 1 ? "No Extra App Icons Yet" : "Add an App Icon"
                detail: "Show a reading or a custom image as a separate taskbar icon."
                buttonText: "+ App Icon"
                buttonName: "addAppIconFooter"
                canAdd: root.manager.appIcons.entries.length < 33
                onClicked: {
                    picker.mode = "app";
                    picker.open();
                }
            }
        }
    }

    Label {
        text: (manager.entries.length - 1) + " / 64 extra system tray icons · " + (manager.appIcons.entries.length - 1) + " / 32 extra app icons. App icons run while WHAT is open. Clicking one opens this page without a separate sensor window. Remove an icon here to stop it. Windows may put system tray icons in the tray overflow."
        color: Theme.muted
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }
}
