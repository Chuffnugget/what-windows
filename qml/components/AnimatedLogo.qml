import QtQuick
import QtQuick.Shapes
import "../controls"

Item {
    id: root

    objectName: "animatedSidebarLogo"
    property bool animationsEnabled: true
    property bool hoverAnimationEnabled: true
    property int animationDuration: 1500
    property int pauseDuration: 180
    property bool textHovered: false
    property bool hovered: pointer.hovered || textHovered
    readonly property bool animating: motion.running
    property real progress: 0
    property bool repeatOnHover: true

    // A completed sidebar transition gets one cycle, even if the pointer rests
    // over the logo. Moving onto it again restores the normal hover animation.
    function playOnce() {
        if (!visible || !animationsEnabled)
            return;
        repeatOnHover = false;
        motion.restart();
    }

    function playOnHover() {
        if (visible && hovered && hoverAnimationEnabled && animationsEnabled && !motion.running) {
            repeatOnHover = true;
            motion.start();
        }
    }
    readonly property real traceProgress: Math.min(1, progress / 0.25)
    // Trace during extension, hold at full spread, and wipe away during reassembly.
    readonly property real eraseProgress: Math.min(1, Math.max(0, (progress - 0.38) / 0.42))
    readonly property real traceOpacity: progress > 0 && progress < 1 ? 0.85 : 0

    HoverHandler {
        id: pointer
    }

    SequentialAnimation {
        id: motion

        NumberAnimation {
            target: root
            property: "progress"
            from: 0
            to: 1
            duration: root.animationDuration
        }

        PauseAnimation {
            duration: root.pauseDuration
        }

        onStopped: root.progress = 0
        onFinished: Qt.callLater(function () {
            if (root.repeatOnHover)
                root.playOnHover();
        })
    }

    onVisibleChanged: if (!visible) {
        motion.stop();
        progress = 0;
    }

    onHoveredChanged: playOnHover()

    onAnimationsEnabledChanged: {
        if (!animationsEnabled) {
            motion.stop();
            progress = 0;
        } else
            playOnHover();
    }

    function easeSpread(x) {
        x = Math.max(0, Math.min(1, x));
        return x * x * (3 - 2 * x);
    }

    readonly property real pieceSpread: easeSpread(progress / 0.25) * (1 - easeSpread((progress - 0.38) / 0.42))
    readonly property var vectorFrame: makeVectorFrame()

    function makeVectorFrame() {
        var spread = root.pieceSpread;
        // One assembled gap for both panel axes and the roof-to-panel spacing.
        var gap = 30, tileWidth = 267, tileHeight = 226, roofHeight = 240;
        var left = 230, roofTop = 120, right = left + tileWidth + gap;
        var top = roofTop + roofHeight + gap, bottom = top + tileHeight + gap;
        // Each piece travels the same distance, including the roof.
        var travel = 50 * spread, topX = travel * 35 / 37, topY = travel * 12 / 37, diagonal = travel / Math.sqrt(2);
        var points = [[left - topX, top - topY],
            [right + topX, top - topY],
            [right + diagonal, bottom + diagonal],
            [left - diagonal, bottom + diagonal],
            [left, roofTop - travel]];
        var shapes = [], fillPath = "", tracePath = "", tipPath = "";
        for (var i = 0; i < 5; i++) {
            var x = points[i][0], y = points[i][1];
            var vertices = i === 4
                ? [[x, y + roofHeight], [x + tileWidth + gap / 2, y],
                    [x + 2 * tileWidth + gap, y + roofHeight], [x, y + roofHeight]]
                : [[x, y], [x + tileWidth, y], [x + tileWidth, y + tileHeight],
                    [x, y + tileHeight], [x, y]];
            shapes.push(vertices);
            fillPath += "M" + vertices[0][0] + "," + vertices[0][1];
            for (var v = 1; v < vertices.length; v++)
                fillPath += "L" + vertices[v][0] + "," + vertices[v][1];
            fillPath += "Z";
        }

        // One global route and one moving tip, without restarting easing on each shape.
        // Transfers between outlines consume distance but leave no permanent connecting line.
        var route = [], total = 0, last = null, order = [4, 0, 1, 2, 3];

        function segment(a, b, draw) {
            var dx = b[0] - a[0], dy = b[1] - a[1], length = Math.sqrt(dx * dx + dy * dy);
            route.push({
                a: a,
                b: b,
                start: total,
                length: length,
                draw: draw
            });
            total += length;
        }

        for (var n = 0; n < order.length; n++) {
            var polygon = shapes[order[n]];
            if (last)
                segment(last, polygon[0], false);
            for (var edge = 1; edge < polygon.length; edge++)
                segment(polygon[edge - 1], polygon[edge], true);
            last = polygon[polygon.length - 1];
        }

        var start = total * root.eraseProgress, end = total * root.traceProgress;
        if (end > start && root.traceOpacity > 0) {
            var connected = false;
            for (var k = 0; k < route.length; k++) {
                var part = route[k], from = Math.max(start, part.start), to = Math.min(end, part.start + part.length);
                if (!part.draw || to <= from) {
                    connected = false;
                    continue;
                }

                var ax = part.a[0], ay = part.a[1], dx = part.b[0] - ax, dy = part.b[1] - ay;
                if (!connected)
                    tracePath += "M" + (ax + dx * (from - part.start) / part.length) + "," + (ay + dy * (from - part.start) / part.length);
                tracePath += "L" + (ax + dx * (to - part.start) / part.length) + "," + (ay + dy * (to - part.start) / part.length);
                connected = true;
            }

            var tip = root.eraseProgress > 0 ? start : end;
            if (tip > 0 && tip < total)
                for (var t = 0; t < route.length; t++) {
                    var part = route[t];
                    if (tip >= part.start && tip < part.start + part.length) {
                        // Keep transfer time, but do not flash a detached white dot in the gaps.
                        if (!part.draw)
                            break;
                        var f = (tip - part.start) / part.length;
                        var tx = part.a[0] + (part.b[0] - part.a[0]) * f, ty = part.a[1] + (part.b[1] - part.a[1]) * f;
                        tipPath = "M" + (tx - 5) + "," + ty + "a5,5 0 1,0 10,0a5,5 0 1,0 -10,0Z";
                        break;
                    }
                }
        }

        return {
            fill: fillPath,
            trace: tracePath,
            tip: tipPath
        };
    }

    // All pieces and the tracer share one vector frame and coordinate transform.
    // Native curve antialiasing avoids bitmap resampling on moving diagonals.
    Shape {
        readonly property real unit: Math.min(root.width / 668, root.height / 848)
        x: (root.width - 668 * unit) / 2 - 178 * unit
        y: (root.height - 848 * unit) / 2 - 66 * unit
        width: 1024
        height: 1024

        transform: Scale {
            xScale: Math.min(root.width / 668, root.height / 848)
            yScale: xScale
        }

        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: "transparent"
            fillColor: Theme.brand

            PathSvg {
                path: root.vectorFrame.fill
            }
        }

        ShapePath {
            fillColor: "transparent"
            strokeColor: Qt.alpha(Theme.brandTrace, root.traceOpacity)
            strokeWidth: 6
            joinStyle: ShapePath.RoundJoin
            capStyle: ShapePath.FlatCap

            PathSvg {
                path: root.vectorFrame.trace
            }
        }

        ShapePath {
            strokeColor: "transparent"
            fillColor: "white"

            PathSvg {
                path: root.vectorFrame.tip
            }
        }
    }
}
