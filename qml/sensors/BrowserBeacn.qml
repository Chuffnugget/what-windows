import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

RowLayout {
    id: root

    required property var controller
    required property var control
    objectName: "beacnControl_" + (control.id || "")
    enabled: control.available === true && controller.beacn.ready === true && !controller.beacn.busy
    spacing: Theme.spaceS

    function send(value) {
        controller.beacnCommand({
            control: control.id,
            value: value
        });
    }

    BrowserMeter {
        id: level

        visible: root.control.kind === "number"
        Layout.fillWidth: true
        from: root.control.min || 0
        to: root.control.max || 100
        stepSize: 1
        value: root.control.value || 0
        Accessible.name: root.control.name
        onPressedChanged: if (!pressed && root.enabled)
            root.send(Math.round(value))
        Keys.onReleased: event => {
            if ([Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End].indexOf(event.key) >= 0)
                root.send(Math.round(value));
        }
    }

    InputField {
        compact: true
        visible: root.control.kind === "number"
        Layout.preferredWidth: 66
        text: Math.round(root.control.value || 0)
        color: Theme.ink
        font.family: "Consolas"
        horizontalAlignment: Text.AlignHCenter
        selectByMouse: true

        validator: IntValidator {
            bottom: root.control.min || 0
            top: root.control.max || 100
        }

        Accessible.name: root.control.name + " exact value"
        onEditingFinished: if (acceptableInput && root.enabled && Number(text) !== root.control.value)
            root.send(Number(text))
    }

    BrowserChoice {
        visible: root.control.kind === "select"
        Layout.fillWidth: true
        model: root.control.options || []
        currentIndex: model.indexOf(root.control.value)
        displayText: currentIndex < 0 ? (root.control.value || "Unavailable") : currentText
        Accessible.name: root.control.name

        onActivated: {
            root.send(currentText);
            currentIndex = Qt.binding(function () {
                return model.indexOf(root.control.value);
            });
        }
    }

    BrowserButton {
        visible: root.control.kind === "switch"
        checkable: true
        checked: root.control.value === true
        text: checked ? "On" : "Off"
        Accessible.name: root.control.name

        onClicked: {
            root.send(checked);
            checked = Qt.binding(function () {
                return root.control.value === true;
            });
        }
    }

    BrowserButton {
        visible: root.control.kind === "button"
        text: root.control.name || "Apply"
        onClicked: root.controller.beacnCommand({
            control: root.control.id
        })
    }
}
