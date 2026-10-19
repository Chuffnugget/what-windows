import QtQuick
import QtQuick.Controls.Basic

ScrollView {
    id: scroll

    // ScrollView suppresses native mouse flicking. Use a mouse-only handler;
    // Qt continues handling touch and the scrollbars; FastWheel handles the wheel.
    // Do not take grabs from text editors or sliders that own their gesture.
    property DragHandler mouseDrag: DragHandler {
        parent: scroll.contentItem
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedButtons: Qt.LeftButton
        grabPermissions: PointerHandler.CanTakeOverFromItems
            | PointerHandler.ApprovesTakeOverByAnything
        xAxis.enabled: scroll.contentWidth > scroll.availableWidth
        yAxis.enabled: scroll.contentHeight > scroll.availableHeight
        property point startScroll
        property point startTranslation

        onActiveChanged: {
            if (active) {
                scroll.contentItem.cancelFlick();
                startScroll = Qt.point(scroll.contentItem.contentX, scroll.contentItem.contentY);
                startTranslation = Qt.point(translation.x, translation.y);
            }
        }
        onTranslationChanged: {
            if (!active)
                return;
            const flick = scroll.contentItem;
            if (xAxis.enabled)
                flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width,
                    startScroll.x - translation.x + startTranslation.x));
            if (yAxis.enabled)
                flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height,
                    startScroll.y - translation.y + startTranslation.y));
        }
    }

    property FastWheel mouseWheel: FastWheel {
        parent: scroll.contentItem
        flick: scroll.contentItem
    }

    Binding {
        target: scroll.contentItem
        property: "interactive"
        value: !scroll.mouseDrag.active
    }
}
