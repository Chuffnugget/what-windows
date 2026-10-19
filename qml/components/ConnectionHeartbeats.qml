import QtQuick
import "../controls"

Item {
    id: graph

    required property var controller
    property bool active: visible
    property double now: Date.now()
    property real millisecondsPerPixel: 15000 / 220
    readonly property real historyMs: Math.max(1, width * millisecondsPerPixel)
    implicitHeight: 56
    implicitWidth: 220

    // Update roles in place. A rename or an extra installation must not destroy
    // another trace's sweep, stored beats or completed pass.
    function syncInstallations() {
        const entries = controller.installationConfiguration;
        const ids = new Set(entries.map(entry => entry.id));
        for (let i = traces.count - 1; i >= 0; --i) {
            if (!ids.has(traces.get(i).installationId))
                traces.remove(i);
        }
        for (const entry of entries) {
            const values = { installationId: entry.id, colour: entry.colour,
                installationEnabled: entry.enabled };
            let existing = -1;
            for (let i = 0; i < traces.count; ++i) {
                if (traces.get(i).installationId === entry.id) {
                    existing = i;
                    break;
                }
            }
            if (existing < 0)
                traces.append(values);
            else
                traces.set(existing, values);
        }
    }

    ListModel { id: traces }
    Component.onCompleted: syncInstallations()
    Connections {
        target: graph.controller
        function onInstallationConfigurationChanged() { graph.syncInstallations(); }
    }
    Timer {
        interval: 1000
        repeat: true
        running: graph.active
        onTriggered: graph.now = Date.now()
    }

    ConnectionHeartbeat {
        anchors.fill: parent
        visible: traces.count === 0
        active: false
    }

    Repeater {
        model: traces
        ConnectionHeartbeat {
            required property int index
            required property string installationId
            required property string colour
            required property bool installationEnabled
            readonly property var connection: graph.controller.installationStatus[installationId] || ({})
            objectName: "installationHeartbeat_" + installationId
            anchors.fill: parent
            active: graph.active
            timestamp: connection.lastCommunication || 0
            connected: installationEnabled && connection.connected === true && connection.feedback === true
            stale: !timestamp || graph.now - timestamp > 12000
            preserveColour: true
            traceColour: colour
            paperVisible: index === 0
            millisecondsPerPixel: graph.millisecondsPerPixel
            opacity: !connected ? 0.45 : stale ? 0.7 : 1
            // Small baseline offsets keep simultaneous replies and flatlines
            // distinguishable while all traces share one time scale and plot.
            baselineY: height * 0.64 + (traces.count > 1 ? -6 + 12 * index / (traces.count - 1) : 0)
        }
    }
}
