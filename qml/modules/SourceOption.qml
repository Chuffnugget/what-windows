import QtQuick
import QtQuick.Controls.Basic
import "../controls"

OptionCheckBox {
    id: option

    ToolTip.text: ({
        "media_titles": "Include track title and artist in media readings. Leave this off to keep listening details private.",
        "window_titles": "Include the foreground window title. Titles may contain document names, browser pages or other private text.",
        "event_messages": "Include the text of collected Windows events. Messages may contain local paths or user information.",
        "notification_contents": "Include notification titles and message bodies in collected readings. Notifications can contain private messages.",
        "auto_drives": "Automatically select connected state and usage readings for newly discovered drives. Existing sensor selections are kept."
    })[key] || "Include this optional information in Windows readings."
    required property var controller
    property string query: ""

    function matches(text) {
        return query.toLowerCase().trim().split(/\s+/).filter(Boolean).every(word => text.toLowerCase().includes(word));
    }

    property string key
    visible: option.matches("windows privacy " + text + " " + key.replace(/_/g, " ")
        + (key.startsWith("notification") ? " notifications contents title body count"
            : key.startsWith("media") ? " media track title artist" : ""))
    checked: controller.info.sources[key] === true

    onClicked: {
        let change = {};
        change[key] = checked;
        controller.saveSources(change);
    }
}
