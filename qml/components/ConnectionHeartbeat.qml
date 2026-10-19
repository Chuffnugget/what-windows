import QtQuick
import QtQuick.Shapes
import "../controls"
import "HeartbeatTrace.js" as HeartbeatTrace

Item {
    id: heartbeat

    property double timestamp: 0
    property bool connected: false
    // Visibility controls drawing; confirmed replies are collected on every page.
    property bool active: visible
    property bool stale: false
    property color traceColour: Theme.accent
    property bool preserveColour: false
    property bool paperVisible: true
    readonly property color lineColor: preserveColour ? traceColour
        : !connected ? Theme.muted : stale ? Theme.warning : traceColour
    // Resizing changes how much time fits, never the spacing of recorded beats.
    property real millisecondsPerPixel: 15000 / 220
    readonly property real historyMs: Math.max(1, width * millisecondsPerPixel)
    property real sweepWidth: implicitWidth
    property real sweepProgress: 0
    property int remainingMs: 15000
    property real baselineY: height * 0.625
    readonly property bool animating: active && visible && Theme.animationsEnabled && width > 0
    readonly property real eraseGap: 10
    readonly property real tipWidth: 3
    property double sweepStarted: Date.now()
    property double previousSweepStarted: 0
    property string currentPath: ""
    property string previousPath: ""
    property var beats: []
    implicitWidth: 220
    implicitHeight: 40
    clip: true

    function clearHistory() {
        beats = [];
        rebuildPaths();
    }

    function advanceSweep(now) {
        // Hidden pages need no animation timer. Catch up directly to the current
        // pass when shown, keeping the same clock and each connection's history.
        const passes = Math.floor((now - sweepStarted) / historyMs);
        if (passes > 0) {
            previousSweepStarted = sweepStarted + (passes - 1) * historyMs;
            sweepStarted += passes * historyMs;
        }
        const oldest = (previousSweepStarted || sweepStarted) - 3000;
        beats = beats.filter(beat => beat.time >= oldest);
        rebuildPaths();
    }

    function rebuildPaths() {
        currentPath = pathForSweep(sweepStarted);
        // Widening a completed pass must not replay replies from the current one
        // in the extra space. That area fills when the drawing tip reaches it.
        previousPath = pathForSweep(previousSweepStarted, sweepStarted);
    }

    function recordReply() {
        if (!connected || stale || timestamp <= 0 || !Theme.animationsEnabled)
            return;

        // Bound retained data even while Overview is hidden for hours.
        const oldest = Date.now() - 2 * historyMs - 3000;
        let recent = beats.filter(beat => beat.time >= oldest);
        // API and MQTT may confirm the same check almost simultaneously.
        if (!recent.length || timestamp - recent[recent.length - 1].time >= 250) {
            // Choose the visual differences once instead of randomising each redraw.
            recent.push({
                time: timestamp,
                spacingMs: recent.length ? timestamp - recent[recent.length - 1].time : historyMs,
                amplitude: 0.90 + Math.random() * 0.20,
                width: 0.94 + Math.random() * 0.12,
                recovery: 0.88 + Math.random() * 0.24
            });
        }
        // The 250 ms coalescing and timed expiry bound this history. A fixed
        // count could discard a beat before the drawing tip had erased it.
        beats = recent;
        if (animating)
            currentPath = pathForSweep(sweepStarted);
    }

    // Rebuild on replies, sweep boundaries or resizing, never on animation frames.
    function pathForSweep(started, finishedAt) {
        const replies = finishedAt ? beats.filter(beat => beat.time < finishedAt) : beats;
        return HeartbeatTrace.path(replies, started, millisecondsPerPixel, width, baselineY);
    }

    // A lost connection changes the styling and draws silence. Historical beats
    // still need to be overwritten by the sweep, rather than vanishing at once.
    onTimestampChanged: pulseRequest.restart()
    onAnimatingChanged: if (!animating) pulse.stop()
    onWidthChanged: {
        // Keep the current clip's width matched to the stopped animator. Changing
        // it first would expose future beats before the restart timer runs.
        pulse.stop();
        rebuildPaths();
    }
    onBaselineYChanged: rebuildPaths()
    Connections {
        target: Theme
        function onAnimationsEnabledChanged() {
            if (!Theme.animationsEnabled)
                heartbeat.clearHistory();
        }
    }

    // Let freshness bindings settle before deciding whether to show this reply.
    Timer {
        id: pulseRequest
        interval: 0
        onTriggered: heartbeat.recordReply()
    }

    // Static dotted paper behind the trace, rebuilt only if its dimensions change.
    Shape {
        anchors.fill: parent
        visible: heartbeat.paperVisible
        opacity: 0.20
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: 1
            strokeColor: Theme.muted
            fillColor: "transparent"
            strokeStyle: ShapePath.DashLine
            dashPattern: [1, 5]
            PathSvg {
                path: {
                    let lines = "";
                    for (let y = 2; y < heartbeat.height; y += 6)
                        lines += "M 2 " + y + " H " + (heartbeat.width - 2) + " ";
                    return lines;
                }
            }
        }
    }

    Rectangle {
        objectName: "haHeartbeatBaseline"
        y: heartbeat.baselineY - height / 2
        width: parent.width
        height: 1.6
        visible: !heartbeat.animating
        color: heartbeat.lineColor
    }

    component Trace: Shape {
        id: strokeShape
        property string path
        property color ink: heartbeat.lineColor
        property real thickness: 1.6
        width: heartbeat.width
        height: heartbeat.height
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: strokeShape.thickness
            strokeColor: strokeShape.ink
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: strokeShape.path }
        }
    }

    // Each clip moves with the drawing tip. Its trace moves in the opposite
    // direction, keeping the waveform fixed while the visible portion changes.
    Item {
        id: currentClip
        width: heartbeat.sweepWidth
        height: heartbeat.height
        clip: true
        visible: heartbeat.animating
        Trace {
            id: trace
            objectName: "haHeartbeatTrace"
            path: heartbeat.currentPath
        }
    }

    Item {
        id: previousClip
        width: heartbeat.width
        height: heartbeat.height
        clip: true
        visible: heartbeat.animating
        Trace {
            id: previousTrace
            path: heartbeat.previousPath
            opacity: 0.50
        }
    }

    Item {
        id: tipClip
        width: heartbeat.tipWidth
        height: heartbeat.height
        clip: true
        visible: heartbeat.animating
        Trace {
            id: tipTrace
            path: heartbeat.currentPath
            ink: heartbeat.preserveColour ? Qt.lighter(heartbeat.lineColor, 1.25) : Theme.ink
            thickness: 2.8
        }
    }

    // Restart after a resize using the elapsed time and the new space remaining.
    // The six render-thread animators then keep moving at the same speed.
    Timer {
        interval: 0
        running: heartbeat.animating && !pulse.running
        onTriggered: {
            const now = Date.now();
            heartbeat.advanceSweep(now);
            const elapsed = Math.max(0, now - heartbeat.sweepStarted);
            heartbeat.sweepWidth = heartbeat.width;
            heartbeat.sweepProgress = Math.min(1, elapsed / heartbeat.historyMs);
            heartbeat.remainingMs = Math.max(1, Math.round(heartbeat.historyMs - elapsed));
            pulse.start();
        }
    }

    ParallelAnimation {
        id: pulse
        objectName: "haHeartbeatPulse"

        XAnimator {
            target: currentClip
            from: -heartbeat.sweepWidth * (1 - heartbeat.sweepProgress); to: 0
            duration: heartbeat.remainingMs
        }
        XAnimator {
            target: trace
            from: heartbeat.sweepWidth * (1 - heartbeat.sweepProgress); to: 0
            duration: heartbeat.remainingMs
        }
        XAnimator {
            target: previousClip
            from: heartbeat.sweepWidth * heartbeat.sweepProgress + heartbeat.eraseGap
            to: heartbeat.sweepWidth + heartbeat.eraseGap
            duration: heartbeat.remainingMs
        }
        XAnimator {
            target: previousTrace
            from: -heartbeat.sweepWidth * heartbeat.sweepProgress - heartbeat.eraseGap
            to: -heartbeat.sweepWidth - heartbeat.eraseGap
            duration: heartbeat.remainingMs
        }
        XAnimator {
            target: tipClip
            from: heartbeat.sweepWidth * heartbeat.sweepProgress - heartbeat.tipWidth
            to: heartbeat.sweepWidth - heartbeat.tipWidth
            duration: heartbeat.remainingMs
        }
        XAnimator {
            target: tipTrace
            from: heartbeat.tipWidth - heartbeat.sweepWidth * heartbeat.sweepProgress
            to: heartbeat.tipWidth - heartbeat.sweepWidth
            duration: heartbeat.remainingMs
        }
    }
}
