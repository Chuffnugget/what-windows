import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: details
    required property var controller
    required property var row
    property bool active: visible
    readonly property var result: controller.haEntity.reading === row.id ? controller.haEntity : ({})
    spacing: Theme.spaceXs

    onActiveChanged: if (active) controller.requestHaEntity(row)
    Component.onCompleted: if (active) controller.requestHaEntity(row)

    Label {
        text: "Home Assistant Entity ID"
        color: Theme.muted
        font.pixelSize: Theme.fontCaption
    }
    Repeater {
        model: details.result.entries || (details.result.entities || []).map(entity => ({entity: entity, installation: ""}))
        BrowserButton {
            id: entityLink
            required property var modelData
            objectName: "haEntityCopy"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: Math.max(Theme.compactHeight, contentItem.implicitHeight + 12)
            property bool copied: false
            text: (modelData.installation ? modelData.installation + " · " : "") + modelData.entity + (copied ? "   ✓ Copied" : "   Copy")
            contentItem: Text {
                text: entityLink.text
                color: Theme.accent
                font.family: "Consolas"
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.WrapAnywhere
                verticalAlignment: Text.AlignVCenter
            }
            help: "Copy this actual Home Assistant entity ID, including any name changes made in Home Assistant."
            onClicked: {
                copied = details.controller.copyHaEntity(modelData.entity);
                if (copied) {
                    copiedTimer.restart();
                    feedback.restart();
                }
            }
            Timer {
                id: copiedTimer
                interval: 1600
                onTriggered: entityLink.copied = false
            }
            NumberAnimation {
                id: feedback
                target: entityLink.contentItem
                property: "opacity"
                from: 0.35
                to: 1
                duration: Theme.animationsEnabled ? Theme.timings.controls * 2 : 0
            }
        }
    }
    HintLabel {
        Layout.fillWidth: true
        visible: (details.result.entities || []).length === 0
        text: details.result.message || "Looking up the Home Assistant entity ID…"
        wrapMode: Text.WordWrap
    }
}
