import QtQuick
import QtQuick.Effects
import "../controls"

Item {
    id: root

    objectName: "glassBackdrop"
    required property Item sourceItem
    clip: true

    Item {
        anchors.fill: parent

        Rectangle {
            anchors.fill: parent
            color: Theme.canvas
        }

        // Freeze the page once. Live readings must not keep rerendering the blur.
        ShaderEffectSource {
            id: snapshot

            sourceItem: root.sourceItem
            live: false
            hideSource: false
            visible: false
            textureSize: Qt.size(Math.max(1, root.width / 2), Math.max(1, root.height / 2))
        }

        MultiEffect {
            width: root.width
            height: root.sourceItem ? root.sourceItem.height : root.height
            y: root.height - height
            source: snapshot
            blurEnabled: true
            blurMax: 48
            blur: 0.8
            autoPaddingEnabled: false
            saturation: -0.25
            brightness: -0.08
        }

        Rectangle {
            anchors.fill: parent
            color: Theme.glassTint
        }
    }
}
