import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import "../controls"

RowLayout {
    id: meter
    required property var source
    required property string readingId
    property bool active: false
    property string registeredId: ""
    readonly property bool watching: active && visible && Window.window !== null && Window.window.visible && Window.window.visibility !== Window.Minimized
    readonly property var sample: source.meters[readingId]
    readonly property bool available: sample !== undefined && sample !== null && isFinite(Number(sample))
    readonly property real level: available ? Math.max(0, Math.min(100, Number(sample))) : 0
    spacing: Theme.spaceM
    function syncSubscription() {
        const next = watching ? readingId : "";
        if (registeredId === next)
            return;
        if (registeredId.length)
            source.watchMeter(registeredId, false);
        registeredId = next;
        if (next.length)
            source.watchMeter(next, true);
    }
    onWatchingChanged: syncSubscription()
    onReadingIdChanged: syncSubscription()
    Component.onCompleted: syncSubscription()
    Component.onDestruction: if (registeredId.length) source.watchMeter(registeredId, false)

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 20
        color: Theme.field
        Rectangle {
            height: parent.height
            width: parent.width * meter.level / 100
            color: meter.level >= 95 ? Theme.warning : Theme.accent
            Behavior on width {
                NumberAnimation { duration: meter.watching && Theme.animationsEnabled ? 45 : 0 }
            }
        }
        Accessible.role: Accessible.ProgressBar
        Accessible.name: "Live audio signal level"
        Accessible.description: meter.available ? meter.level.toFixed(1) + " percent" : "Unavailable"
    }
    HintLabel {
        Layout.preferredWidth: 90
        horizontalAlignment: Text.AlignRight
        text: meter.available ? meter.level.toFixed(1) + " %" : "Unavailable"
    }
}
