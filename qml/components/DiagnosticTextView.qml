import QtQuick
import QtQuick.Controls.Basic
import "../controls"

Flickable {
    id: view

    property string sourceText: ""
    property bool paused: false
    property bool updatesEnabled: true
    readonly property string displayedText: contents.text

    function syncText() {
        if (!updatesEnabled || paused || contents.selectedText.length > 0)
            return;
        const savedY = contentY;
        const wasFollowing = following;
        contents.text = sourceText;
        positioning = true;
        contentY = Math.min(savedY, Math.max(0, contentHeight - height));
        positioning = false;
        following = wasFollowing;
        Qt.callLater(followEnd);
    }

    onSourceTextChanged: syncText()
    onPausedChanged: syncText()
    onUpdatesEnabledChanged: syncText()
    Component.onCompleted: syncText()
    property bool followTail: false
    property bool following: true
    property bool positioning: false
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    contentWidth: width
    contentHeight: contents.height

    function followEnd() {
        if (!updatesEnabled
            || paused
            || contents.selectedText.length > 0
            || !followTail
            || !following
            || moving
            || scrollBar.pressed
            || wheelPause.running)
            return;
        positioning = true;
        contentY = Math.max(0, contentHeight - height);
        positioning = false;
    }

    onContentHeightChanged: Qt.callLater(followEnd)
    onHeightChanged: Qt.callLater(followEnd)
    onContentYChanged: if (!positioning)
        following = contentY >= Math.max(0, contentHeight - height) - 2

    onMovementEnded: {
        following = atYEnd;
        followEnd();
    }

    ScrollBar.vertical: WideScrollBar {
        id: scrollBar

        onPressedChanged: if (!pressed) {
            view.following = view.atYEnd;
            view.followEnd();
        }
    }

    Timer {
        id: wheelPause

        interval: 180
        onTriggered: view.followEnd()
    }

    FastWheel {
        flick: view
        enabled: !scrollBar.pressed
        onScrollActivity: wheelPause.restart()
    }

    TextEdit {
        id: contents

        objectName: view.objectName + "Text"
        width: Math.max(0, view.width - scrollBar.width)
        height: implicitHeight
        onSelectedTextChanged: if (selectedText.length === 0)
            Qt.callLater(view.syncText)
        readOnly: true
        selectByMouse: true
        textFormat: TextEdit.PlainText
        wrapMode: TextEdit.WrapAnywhere
        color: Theme.ink
        font.family: "Consolas"
        font.pixelSize: Theme.fontCaption
    }
}
