import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

Control {
    id: reorderGrip

    required property Item moduleItem
    required property Item reorderHost

    objectName: moduleItem.moduleKey + "ReorderGrip"
    visible: reorderHost !== null
    Layout.preferredWidth: 24
    Layout.preferredHeight: 40
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: "Reorder " + moduleItem.heading
    Accessible.description: "Drag to move this module, or press Alt+Up or Alt+Down."
    ToolTip.text: Accessible.description
    ToolTip.visible: (gripMouse.containsMouse || activeFocus) && !gripMouse.pressed
    ToolTip.delay: Theme.toolTipDelay
    ToolTip.timeout: Theme.toolTipTimeout
    Keys.onPressed: event => {
        if ((event.modifiers & Qt.AltModifier)
                && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
            reorderHost.nudgeModule(moduleItem, event.key === Qt.Key_Up ? -1 : 1);
            event.accepted = true;
        }
    }

    background: Rectangle {
        color: reorderGrip.activeFocus || gripMouse.containsMouse ? Theme.hover : "transparent"
    }

    Grid {
        anchors.centerIn: parent
        columns: 2
        spacing: 3
        Repeater {
            model: 6
            Rectangle {
                width: 3
                height: 3
                color: gripMouse.pressed ? Theme.accent : Theme.muted
            }
        }
    }

    MouseArea {
        id: gripMouse

        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        onPressed: mouse => {
            reorderGrip.forceActiveFocus(Qt.MouseFocusReason);
            reorderHost.prepareReorder(moduleItem, mapToItem(null, mouse.x, mouse.y));
        }
        onPositionChanged: mouse => {
            if (pressed)
                reorderHost.updateReorder(mapToItem(null, mouse.x, mouse.y));
        }
        onReleased: mouse => reorderHost.finishReorder(mapToItem(null, mouse.x, mouse.y))
        onCanceled: reorderHost.cancelReorder()
    }
}
