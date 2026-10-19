import QtQuick
import QtQuick.Controls.Basic
import "../controls"

// One drag state per scroll view. Both module lists retain their existing rows.
Item {
    id: reorderArea

    required property var controller
    required property Item scrollView
    required property Item contents
    required property var cards
    parent: scrollView
    anchors.fill: parent
    z: 10
    clip: true

    Binding {
        target: reorderArea.scrollView.mouseDrag
        property: "enabled"
        value: reorderArea.reorderCard === null
    }

    readonly property var order: controller.moduleOrder
    property var reorderCard: null
    property bool dragging: false
    property point dragPoint
    property real pressY: 0
    property int dropIndex: -1
    property real dropLineY: 0

    function visibleCards() {
        return cards.filter(card => card.visible)
            .sort((a, b) => order.indexOf(a.moduleKey) - order.indexOf(b.moduleKey));
    }

    function prepareReorder(card, point) {
        cancelReorder();
        reorderCard = card;
        dragPoint = point;
        pressY = point.y;
        scrollView.contentItem.cancelFlick();
        cards.forEach(item => {
            if (item.expansionScroll)
                item.expansionScroll.cancel();
        });
    }

    function updateReorder(point) {
        if (!reorderCard)
            return;
        dragPoint = point;
        if (Math.abs(point.y - pressY) >= Qt.styleHints.startDragDistance)
            dragging = true;
        if (dragging)
            updateDropPosition();
    }

    function updateDropPosition() {
        const pointer = scrollView.mapFromItem(null, dragPoint.x, dragPoint.y);
        dropIndex = -1;
        if (pointer.x < 0 || pointer.x > scrollView.availableWidth
                || pointer.y < 0 || pointer.y > scrollView.availableHeight)
            return;
        const candidates = visibleCards().filter(card => card !== reorderCard);
        if (!candidates.length)
            return;
        const remaining = order.filter(key => key !== reorderCard.moduleKey);
        const y = contents.mapFromItem(null, dragPoint.x, dragPoint.y).y;
        for (const card of candidates) {
            if (y < card.y + card.height / 2) {
                dropIndex = remaining.indexOf(card.moduleKey);
                dropLineY = card.mapToItem(scrollView, 0, 0).y;
                return;
            }
        }
        const last = candidates[candidates.length - 1];
        dropIndex = remaining.indexOf(last.moduleKey) + 1;
        dropLineY = last.mapToItem(scrollView, 0, last.height).y;
    }

    function moveModule(card, index) {
        const next = order.filter(key => key !== card.moduleKey);
        next.splice(index, 0, card.moduleKey);
        controller.setModuleOrder(next);
    }

    function finishReorder(point) {
        updateReorder(point);
        if (dragging && dropIndex >= 0)
            moveModule(reorderCard, dropIndex);
        cancelReorder();
    }

    function cancelReorder() {
        dragging = false;
        reorderCard = null;
        dropIndex = -1;
    }

    function nudgeModule(card, direction) {
        const visible = visibleCards();
        const neighbour = visible[visible.indexOf(card) + direction];
        if (neighbour)
            moveModule(card, order.indexOf(neighbour.moduleKey));
    }

    onVisibleChanged: if (!visible) cancelReorder()

    Shortcut {
        sequence: "Escape"
        enabled: reorderArea.reorderCard !== null
        onActivated: reorderArea.cancelReorder()
    }

    FrameAnimation {
        // Only active during a drag. The cards stay in place until release, so
        // expanded editors are neither rebuilt nor captured into a large texture.
        running: reorderArea.dragging
        onTriggered: {
            const point = scrollView.mapFromItem(null, reorderArea.dragPoint.x, reorderArea.dragPoint.y);
            if (point.x < 0 || point.x > scrollView.availableWidth)
                return;
            const edge = 44;
            const direction = point.y < edge ? -1 : point.y > scrollView.availableHeight - edge ? 1 : 0;
            const step = direction * 500 * Math.min(frameTime, 0.05);
            if (!step)
                return;
            const flick = scrollView.contentItem;
            const maximum = Math.max(0, flick.contentHeight - flick.height);
            flick.contentY = Math.max(0, Math.min(maximum, flick.contentY + step));
            reorderArea.updateDropPosition();
        }
    }

    Item {
        anchors.fill: parent
        z: 10
        clip: true
        visible: reorderArea.dragging

        Rectangle {
            objectName: "moduleDropMarker"
            visible: reorderArea.dropIndex >= 0
            y: reorderArea.dropLineY - height / 2
            width: contents.width
            height: 3
            color: Theme.accent
        }

        Rectangle {
            width: Math.min(320, contents.width)
            height: 44
            x: 36
            y: Math.max(0, Math.min(parent.height - height,
                scrollView.mapFromItem(null, reorderArea.dragPoint.x, reorderArea.dragPoint.y).y - height / 2))
            color: Theme.surface
            border.color: Theme.accent
            opacity: 0.95

            Label {
                anchors.fill: parent
                anchors.margins: 12
                verticalAlignment: Text.AlignVCenter
                text: reorderArea.reorderCard ? reorderArea.reorderCard.heading : ""
                color: Theme.ink
                elide: Text.ElideRight
            }
        }
    }
}
