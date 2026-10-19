import QtQuick
import QtQuick.Window
import "../controls"

Window {
    id: fluid

    objectName: "fluidWindowTransition"
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowTransparentForInput | Qt.WindowDoesNotAcceptFocus | Qt.NoDropShadowWindowHint
    color: "transparent"
    visible: false
    property rect destination: Qt.rect(0, 0, 1, 1)
    property color bodyColour: Theme.canvas
    property real progress: 1
    property bool growing: true
    readonly property bool running: motion.running
    signal completed
    x: screen.virtualX
    y: screen.virtualY
    width: screen.width
    height: screen.height

    function start(rectangle, grow) {
        motion.stop();
        destination = rectangle;
        growing = grow;
        progress = grow ? 0 : 1;
        visible = true;
        motion.from = progress;
        motion.to = grow ? 1 : 0;
        motion.duration = grow ? 620 : 430;
        motion.start();
    }

    function cancel() {
        motion.stop();
        visible = false;
        progress = 1;
    }

    onProgressChanged: silhouette.requestPaint()

    NumberAnimation {
        id: motion

        objectName: "fluidMotion"
        target: fluid
        property: "progress"
        easing.type: Easing.Linear

        onFinished: {
            fluid.visible = false;
            fluid.completed();
        }
    }

    Canvas {
        id: silhouette

        anchors.fill: parent
        renderTarget: Canvas.Image

        onPaint: {
            const ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);

            function ease(t) {
                t = Math.max(0, Math.min(1, t));
                return t * t * (3 - 2 * t);
            }

            const p = fluid.progress;
            const travel = ease(p / 0.62);
            const bloom = ease((p - 0.48) / 0.52);
            const target = fluid.destination;
            const targetX = target.x - fluid.x;
            const targetY = target.y - fluid.y;
            const ribbonWidth = Math.min(64, target.width * 0.09);
            const ribbonHeight = Math.min(230, target.height * 0.4);
            const w = ribbonWidth + (target.width - ribbonWidth) * bloom;
            const h = ribbonHeight + (target.height - ribbonHeight) * bloom;
            const cx = targetX + target.width / 2;
            const cy = (height + ribbonHeight / 2) * (1 - travel) + (targetY + target.height / 2) * travel;
            const left = cx - w / 2, top = cy - h / 2;
            const radius = (1 - bloom) * ribbonWidth / 2;
            const swell = Math.sin(Math.PI * bloom) * Math.min(35, w * 0.08);
            ctx.fillStyle = fluid.bodyColour;
            ctx.beginPath();
            ctx.moveTo(left + radius, top);
            ctx.bezierCurveTo(left + w / 2, top - swell, left + w - radius, top, left + w - radius, top);
            ctx.quadraticCurveTo(left + w, top, left + w, top + radius);
            ctx.bezierCurveTo(left + w + swell,
                top + h * 0.4,
                left + w + swell,
                top + h * 0.7,
                left + w,
                top + h - radius);
            ctx.quadraticCurveTo(left + w, top + h, left + w - radius, top + h);
            ctx.lineTo(left + radius, top + h);
            ctx.quadraticCurveTo(left, top + h, left, top + h - radius);
            ctx.bezierCurveTo(left - swell, top + h * 0.7, left - swell, top + h * 0.4, left, top + radius);
            ctx.quadraticCurveTo(left, top, left + radius, top);
            ctx.closePath();
            ctx.fill();
        }
    }
}
