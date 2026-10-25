import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

RowLayout {
    id: root

    required property var controller
    required property var control
    objectName: "beacnControl_" + (control.id || "")
    enabled: control.available === true && controller.beacn.ready === true
    spacing: Theme.spaceS

    property bool hasDraft: false
    property real draftValue: 0
    property bool awaitingReply: false
    property var queuedValue: undefined
    readonly property real actualValue: Number(control.value) || 0

    // Polling must not move the handle under the pointer. Keep the user's last
    // edit until its reply, and coalesce further edits while the worker is busy.
    function edit(value) {
        draftValue = Math.round(value);
        hasDraft = true;
    }

    function submit(value) {
        edit(value);
        queuedValue = draftValue;
        dispatchPending();
    }

    function dispatchPending() {
        if (!enabled || awaitingReply || controller.beacn.busy || queuedValue === undefined)
            return;
        const value = queuedValue;
        queuedValue = undefined;
        awaitingReply = true;
        send(value);
    }

    onEnabledChanged: {
        if (!enabled) {
            queuedValue = undefined;
            hasDraft = false;
        }
    }

    Connections {
        target: root.controller

        function onChanged() {
            if (root.queuedValue !== undefined)
                Qt.callLater(root.dispatchPending);
        }

        function onLocalBeacnFinished(control, ok, message) {
            if (control !== root.control.id || !root.awaitingReply)
                return;
            root.awaitingReply = false;
            // An uncertain failure must never automatically replay another edit.
            if (!ok)
                root.queuedValue = undefined;
            if (root.queuedValue === undefined)
                root.hasDraft = false;
            else
                Qt.callLater(root.dispatchPending);
        }
    }

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
        objectName: root.objectName + "_slider"
        Accessible.name: root.control.name
        ToolTip.text: root.awaitingReply || root.queuedValue !== undefined
                      ? "Applying the latest value to BEACN..."
                      : "Drag to choose a value. Release to apply it to BEACN."
        onMoved: root.edit(value)
        onPressedChanged: if (!pressed && root.enabled && root.hasDraft)
            root.submit(value)

        Binding {
            target: level
            property: "value"
            value: root.hasDraft ? root.draftValue : root.actualValue
            when: !level.pressed
            restoreMode: Binding.RestoreNone
        }
        Keys.onReleased: event => {
            if ([Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End].indexOf(event.key) >= 0)
                root.submit(value);
        }
    }

    InputField {
        id: exact

        property bool edited: false
        objectName: root.objectName + "_exact"
        compact: true
        visible: root.control.kind === "number"
        Layout.preferredWidth: 66
        color: Theme.ink
        font.family: "Consolas"
        horizontalAlignment: Text.AlignHCenter
        selectByMouse: true

        validator: IntValidator {
            bottom: root.control.min || 0
            top: root.control.max || 100
        }

        Accessible.name: root.control.name + " exact value"
        onTextEdited: {
            edited = true;
            if (acceptableInput)
                root.edit(Number(text));
        }
        onEditingFinished: {
            if (!edited)
                return;
            edited = false;
            if (acceptableInput && root.enabled &&
                    (root.awaitingReply || root.queuedValue !== undefined || Number(text) !== root.actualValue))
                root.submit(Number(text));
            else if (!root.awaitingReply && root.queuedValue === undefined)
                root.hasDraft = false;
        }

        Binding {
            target: exact
            property: "text"
            value: Math.round(root.hasDraft ? root.draftValue : root.actualValue).toString()
            when: !exact.activeFocus
            restoreMode: Binding.RestoreNone
        }
    }

    BrowserChoice {
        enabled: !root.controller.beacn.busy
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
        enabled: !root.controller.beacn.busy
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
        enabled: !root.controller.beacn.busy
        visible: root.control.kind === "button"
        text: root.control.name || "Apply"
        onClicked: root.controller.beacnCommand({
            control: root.control.id
        })
    }
}
