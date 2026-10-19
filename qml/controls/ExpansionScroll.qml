import QtQuick

// Follow the visible reveal, rather than jumping after its animation finishes.
// Only a requested expansion runs this helper; live data and searches do not
// move the viewport. Large sections retain their header at the top.
Item {
    id: reveal

    required property Item targetItem
    required property bool expanded
    property var flick: null
    property real progress: 0
    property real startY: 0
    property bool following: false
    property bool pendingFollow: false
    property bool alignHeader: false
    property bool finishing: false
    visible: false

    function follow(header = false) {
        cancel();
        if (!targetItem || !expanded)
            return;
        alignHeader = header;
        // Navigation can queue a destination behind an unfinished page slide.
        // Retain the request until that page is visible and can be laid out.
        if (!targetItem.visible) {
            pendingFollow = true;
            return;
        }
        let ancestor = targetItem.parent;
        while (ancestor && !(ancestor instanceof Flickable))
            ancestor = ancestor.parent;
        flick = ancestor;
        if (!flick)
            return;
        flick.cancelFlick();
        startY = flick.contentY;
        progress = 0;
        following = true;
        finishing = false;
        motion.start();
    }

    function updatePosition() {
        if (!following || !flick || !targetItem || !expanded || !targetItem.visible)
            return;
        if (flick.dragging || !flick.interactive) {
            cancel();
            return;
        }
        const top = targetItem.mapToItem(flick.contentItem, 0, 0).y;
        const bottom = top + targetItem.height + 12;
        // ListView places an inline header above its first delegate, so its
        // scrollable start can be negative even when originY is zero.
        const header = flick.headerItem;
        const origin = header ? Math.min(flick.originY || 0, header.y) : (flick.originY || 0);
        const limit = origin + Math.max(0, flick.contentHeight - flick.height);
        const needed = alignHeader || top < startY ? top
            : Math.max(startY, Math.min(top, bottom - flick.height));
        const destination = Math.max(origin, Math.min(limit, needed));
        // A disclosure's height is already eased. Track that edge directly;
        // easing it again makes the viewport trail behind the content. Explicit
        // navigation to an existing header still needs its own scroll animation.
        flick.contentY = alignHeader && progress < 1
            ? startY + (destination - startY) * progress : destination;
    }

    function cancel() {
        pendingFollow = false;
        resumeFollow.stop();
        following = false;
        motion.stop();
        positionUpdate.stop();
        settled.stop();
    }

    // A page's visibleChanged signal can precede its viewport becoming usable.
    // Resume after that visibility/layout pass, retaining cancellation ownership.
    Timer {
        id: resumeFollow
        interval: 0
        onTriggered: if (reveal.pendingFollow) reveal.follow(reveal.alignHeader)
    }

    onProgressChanged: positionUpdate.restart()
    onExpandedChanged: if (!expanded) cancel()
    onTargetItemChanged: cancel()

    NumberAnimation {
        id: motion
        target: reveal
        property: "progress"
        from: 0
        to: 1
        duration: Theme.animationsEnabled ? Theme.timings.panels : 0
        easing.type: Easing.OutCubic
        onFinished: {
            reveal.finishing = true;
            positionUpdate.restart();
            settled.restart();
        }
    }

    // The owned timer runs after layout and is destroyed with its section.
    // A queued JavaScript closure can outlive a recycled delegate's context.
    Timer {
        id: positionUpdate
        interval: 0
        onTriggered: {
            reveal.updatePosition();

        }
    }

    // Wrapped text and nested disclosures may finish layout after the nominal
    // animation duration. Keep following until their geometry settles.
    Timer {
        id: settled
        interval: 50
        onTriggered: reveal.following = false
    }

    Connections {
        target: reveal.targetItem
        function onVisibleChanged() {
            if (reveal.pendingFollow && reveal.targetItem.visible)
                resumeFollow.restart();
            else if (reveal.following && !reveal.targetItem.visible)
                reveal.cancel();
        }
        function onHeightChanged() {
            if (!reveal.following)
                return;
            positionUpdate.restart();
            if (reveal.finishing)
                settled.restart();
        }
    }

    Connections {
        target: reveal.flick
        function onDraggingChanged() {
            if (reveal.flick.dragging) reveal.cancel();
        }
        function onInteractiveChanged() {
            if (!reveal.flick.interactive) reveal.cancel();
        }
    }

    WheelHandler {
        parent: reveal.flick
        enabled: reveal.following
        target: null
        onWheel: event => {
            reveal.cancel();
            event.accepted = false;
        }
    }
}
