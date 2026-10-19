import QtQuick
import QtQuick.Controls.Basic
import "../controls"

PermissionOption {
    id: option

    help: permission === "media"
        ? "Allow paired Home Assistant clients to control playback, seeking, volume and mute on this PC."
        : "Allow paired Home Assistant clients to use this Windows control."
    required property var controller
    property string query: ""

    function matches(text) {
        return query.toLowerCase().trim().split(/\s+/).filter(Boolean).every(word => text.toLowerCase().includes(word));
    }

    required property string permission
    visible: option.matches("windows remote control " + text + " " + permission)
    checked: controller.info.controlPermissions[permission] === true
    onToggled: allowed => controller.setControlPermission(permission, allowed)
}
