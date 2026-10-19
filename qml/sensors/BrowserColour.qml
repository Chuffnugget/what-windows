import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: editor

    required property var controller
    required property var row
    objectName: "inlineColour_" + row.id
    property color draft: row.value || "#ffffff"
    property color lastLit: "white"
    onDraftChanged: if (Math.max(draft.r, draft.g, draft.b) > 0)
        lastLit = draft
    property string result: ""
    property string request: ""
    property string openedEffect: ""
    Component.onCompleted: openedEffect = row.lightingEffect || ""
    signal colourSubmitted(string id, string colour)
    readonly property bool favourite: controller.colourPalette.favourites.indexOf(draft.toString()) >= 0
    spacing: 6
    enabled: row.available === true && (!openedEffect || row.lightingEffect === openedEffect)

    function choose(colour) {
        // Saved icon colours can include alpha. Device lighting only uses RGB.
        draft = colour.length === 9 ? "#" + colour.slice(3) : colour;
    }

    function apply() {
        if (!enabled || request)
            return;
        const current = controller.icueLightingRow(row.id);
        if (!current.id || !current.available || (openedEffect && current.lightingEffect !== openedEffect)) {
            result = "Device or effect changed. Close and reopen this row.";
            return;
        }

        let args;
        if (row.lightingParameter) {
            let parameters = {};
            parameters[row.lightingParameter] = draft.toString();
            args = {
                expected_effect: row.lightingEffect,
                parameters: parameters
            };
        } else {
            args = {
                device: row.icueDevice,
                rgb: [Math.round(draft.r * 255), Math.round(draft.g * 255), Math.round(draft.b * 255)]
            };
            if (row.icueLeds && row.icueLeds.length)
                args.leds = Array.from(row.icueLeds);
            else if (row.icueLed !== undefined)
                args.leds = [row.icueLed];
        }

        request = "browser-colour:" + row.id + ":" + Date.now();
        colourSubmitted(request, draft.toString());
        result = "Applying…";
        controller.localIcueCommand(row.lightingParameter ? "icue_lighting" : "icue_colors", args, request);
    }

    Connections {
        target: editor.controller

        function onLocalIcueReply(id, ok, message) {
            if (id !== editor.request)
                return;
            editor.request = "";
            editor.result = ok ? "Colour applied" : message;
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceL

        Rectangle {
            width: 46
            height: 66

            color: editor.draft
            border.color: Theme.outlineStrong
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Repeater {
                model: ["R", "G", "B"]

                RowLayout {
                    required property int index
                    required property string modelData
                    spacing: Theme.spaceS

                    Text {
                        text: modelData
                        color: Theme.muted
                        font.pixelSize: Theme.fontSmall
                    }

                    BrowserMeter {
                        Layout.fillWidth: true
                        implicitHeight: 22
                        tint: [Theme.channelRed, Theme.channelGreen, Theme.channelBlue][index]
                        from: 0
                        to: 255
                        stepSize: 1
                        value: Math.round([editor.draft.r, editor.draft.g, editor.draft.b][index] * 255)
                        Accessible.name: ["Red", "Green", "Blue"][index]

                        onMoved: {
                            let rgb = [editor.draft.r, editor.draft.g, editor.draft.b];
                            rgb[index] = value / 255;
                            editor.draft = Qt.rgba(rgb[0], rgb[1], rgb[2], 1);
                        }
                    }

                    Text {
                        text: Math.round([editor.draft.r, editor.draft.g, editor.draft.b][index] * 255)
                        color: Theme.ink
                        font.family: "Consolas"
                        font.pixelSize: Theme.fontSmall
                        Layout.preferredWidth: 24
                    }
                }
            }
        }

        ColumnLayout {
            InputField {
                compact: true
                implicitWidth: 90
                implicitHeight: 28
                text: editor.draft.toString().toUpperCase()
                color: Theme.ink
                font.family: "Consolas"
                selectByMouse: true

                validator: RegularExpressionValidator {
                    regularExpression: /#[0-9a-fA-F]{6}/
                }

                Accessible.name: "Hex colour"
                onEditingFinished: if (acceptableInput)
                    editor.draft = text

            }

            BrowserButton {
                text: "Apply Colour"
                selected: true
                enabled: !editor.request
                onClicked: editor.apply()
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceM

        Text {
            text: "Brightness"
            color: Theme.muted
            font.pixelSize: Theme.fontSmall
        }

        BrowserMeter {
            Layout.fillWidth: true
            implicitHeight: 24
            from: 0
            to: 100
            stepSize: 1
            value: Math.round(Math.max(editor.draft.r, editor.draft.g, editor.draft.b) * 100)
            Accessible.name: "Brightness"

            onMoved: {
                const source = Math.max(editor.draft.r, editor.draft.g, editor.draft.b) > 0
                    ? editor.draft
                    : editor.lastLit;
                const scale = value / 100 / Math.max(source.r, source.g, source.b);
                editor.draft = Qt.rgba(source.r * scale, source.g * scale, source.b * scale, 1);
            }
        }

        Text {
            text: Math.round(Math.max(editor.draft.r, editor.draft.g, editor.draft.b) * 100) + "%"
            color: Theme.ink
            font.family: "Consolas"
            font.pixelSize: Theme.fontSmall
            Layout.preferredWidth: 35
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: 5

        BrowserButton {
            text: editor.favourite ? "★ Saved" : "☆ Save Colour"
            help: "Keep this colour in favourites throughout WHAT."
            enabled: editor.favourite || editor.controller.colourPalette.favourites.length < 32
            onClicked: editor.controller.colourPalette.setFavourite(editor.draft, !editor.favourite)
        }

        Repeater {
            model: editor.controller.colourPalette.favourites

            BrowserButton {
                required property string modelData
                implicitWidth: 26
                help: "Favourite " + modelData + ". Right-click to remove it."
                onClicked: editor.choose(modelData)

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: editor.controller.colourPalette.setFavourite(modelData, false)
                }

                background: Rectangle {
                    color: modelData

                    border.color: parent.visualFocus ? Theme.accent : Theme.outlineStrong
                }
            }
        }

        Text {
            text: "Recent"
            height: 28
            verticalAlignment: Text.AlignVCenter
            color: Theme.muted
            font.pixelSize: Theme.fontSmall
        }

        Repeater {
            model: editor.controller.colourPalette.recent

            BrowserButton {
                required property string modelData
                implicitWidth: 26
                help: "Previously used " + modelData
                onClicked: editor.choose(modelData)

                background: Rectangle {
                    color: modelData

                    border.color: parent.visualFocus ? Theme.accent : Theme.outlineStrong
                }
            }
        }
    }

    Text {
        visible: text.length > 0
        text: editor.result
        color: Theme.muted
        font.pixelSize: Theme.fontSmall
        Layout.fillWidth: true
        wrapMode: Text.Wrap
    }
}
