import QtQuick
import "../controls"

Item {
    id: root

    default property alias pages: content.data
    property int currentIndex: 0
    required property var pageOrder
    property bool ready: false
    property int shownIndex: currentIndex
    property int destinationIndex: currentIndex
    readonly property bool moving: movement.running
    readonly property bool presented: visible && Window.window && Window.window.visible
        && Window.window.visibility !== Window.Minimized
    clip: true

    property Item pageContainer: Item {
        id: content

        parent: root
        width: root.width
        height: root.height
    }

    function sizePage(page) {
        page.width = root.width;
        page.height = root.height;
    }

    // A sidebar animation changes the viewport every frame. Hidden pages keep
    // their last layout and catch up before display, avoiding work on every page.
    function resizeVisiblePage() {
        if (!ready)
            return;
        if (moving)
            finish();
        sizePage(content.children[shownIndex]);
    }

    // Only the two endpoints participate. Moving their common parent avoids
    // evaluating page bindings or laying out intermediate pages on every frame.
    function settle(index) {
        sizePage(content.children[index]);
        content.y = 0;
        shownIndex = index;
        for (let i = 0; i < content.children.length; ++i) {
            const page = content.children[i];
            page.y = 0;
            page.visible = i === index;
            page.enabled = i === index;
        }
    }

    function finish() {
        if (!ready)
            return;
        movement.stop();
        settle(currentIndex);
    }

    function navigate() {
        if (!ready)
            return;
        if (moving) {
            finish();
            return;
        }
        if (currentIndex === shownIndex)
            return;
        const distance = pageOrder.indexOf(currentIndex) - pageOrder.indexOf(shownIndex);
        if (!presented || !Theme.animationsEnabled || Theme.timings.pages <= 0 || height <= 0) {
            settle(currentIndex);
            return;
        }

        destinationIndex = currentIndex;
        const offset = distance > 0 ? height : -height;
        const incoming = content.children[destinationIndex];
        sizePage(incoming);
        content.children[shownIndex].enabled = false;
        incoming.y = offset;
        incoming.visible = true;
        incoming.enabled = false;
        movement.from = 0;
        movement.to = -offset;
        movement.duration = Math.max(1, Math.round(Theme.timings.pages / Math.sqrt(Math.abs(distance))));
        movement.start();
    }

    onCurrentIndexChanged: navigate()
    onPresentedChanged: if (!presented) finish()
    onHeightChanged: resizeVisiblePage()
    onWidthChanged: resizeVisiblePage()

    Component.onCompleted: {
        for (const page of content.children)
            sizePage(page);

        ready = true;
        settle(currentIndex);
    }

    YAnimator {
        id: movement

        target: content
        easing.type: Easing.InOutCubic

        onFinished: {
            root.settle(root.destinationIndex);

        }
    }

    Connections {
        target: Theme

        function onAnimationsEnabledChanged() {
            if (!Theme.animationsEnabled)
                root.finish();
        }

        function onTimingsChanged() {
            if (Theme.timings.pages <= 0)
                root.finish();
        }
    }
}
