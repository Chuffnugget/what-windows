import QtQuick

MouseArea {
    id: wheel

    signal scrollActivity
    required property var flick
    property real scrollTarget: flick.contentY
    anchors.fill: parent
    // Nested scrolling panes receive wheel input first. At their limits,
    // unhandled events reach this containing page.
    z: -1
    // Handle wheel input without taking clicks or drag gestures from child controls.
    acceptedButtons: Qt.NoButton
    scrollGestureEnabled: true

    NumberAnimation {
        id: wheelMotion

        target: wheel.flick
        property: "contentY"
        duration: 130
        easing.type: Easing.OutCubic
    }

    function cancelWheel() {
        wheelMotion.stop();
        scrollTarget = flick.contentY;
    }

    Connections {
        target: wheel.flick

        function onInteractiveChanged() {
            if (!wheel.flick.interactive)
                wheel.cancelWheel();
        }

        function onDraggingChanged() {
            if (wheel.flick.dragging)
                wheel.cancelWheel();
        }

        function onVisibleChanged() {
            if (!wheel.flick.visible)
                wheel.cancelWheel();
        }

        function onContentHeightChanged() {
            const origin = wheel.flick.originY || 0;
            const limit = origin + Math.max(0, wheel.flick.contentHeight - wheel.flick.height);
            if (wheel.scrollTarget < origin || wheel.scrollTarget > limit)
                wheel.cancelWheel();
        }
    }

    onEnabledChanged: if (!enabled)
        cancelWheel()

    // ScrollView consumes ignored wheel events too. Explicitly hand an edge
    // event to the nearest containing WHAT scroller instead of trapping it.
    function scrollBy(distance, pixelMotion) {
        const origin = flick.originY || 0;
        // Accumulate wheel notches against the destination, even during an unfinished scroll.
        const base = wheelMotion.running ? scrollTarget : flick.contentY;
        scrollTarget = Math.max(origin,
            Math.min(origin + Math.max(0, flick.contentHeight - flick.height), base - distance));
        if (scrollTarget === base) {
            for (let item = flick.parent; item; item = item.parent) {
                if (item.mouseWheel && item.mouseWheel !== wheel)
                    return item.mouseWheel.scrollBy(distance, pixelMotion);
            }
            return false;
        }
        scrollActivity();
        wheelMotion.stop();
        flick.cancelFlick();
        // Trackpad pixel events already describe smooth motion; avoid animating them twice.
        if (pixelMotion || !Theme.animationsEnabled)
            flick.contentY = scrollTarget;
        else {
            wheelMotion.from = flick.contentY;
            wheelMotion.to = scrollTarget;
            wheelMotion.start();
        }

        return true;
    }

    onWheel: function (event) {
        if (event.modifiers & Qt.ControlModifier) {
            event.accepted = false;
            return;
        }
        const pixels = event.pixelDelta.y;
        const distance = pixels !== 0 ? pixels : event.angleDelta.y / 120 * 180;
        event.accepted = distance !== 0 && scrollBy(distance, pixels !== 0);
    }
}
