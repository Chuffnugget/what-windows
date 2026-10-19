import QtQuick
import QtQuick.Templates as T
import "qrc:/qml/controls"

// Attached hints share one instance, including hints on virtualised sensor rows.
T.ToolTip {
    id: tip
    objectName: "whatToolTip"
    // WebView2 sits above the QML scene in every view, including the main
    // window. A separate popup window keeps hints above it without hiding it.
    popupType: T.Popup.Window

    implicitWidth: Math.min(360, implicitContentWidth + leftPadding + rightPadding,
        parent && parent.Window.window ? Math.max(1, parent.Window.window.width - 24) : 360)
    implicitHeight: implicitContentHeight + topPadding + bottomPadding
    // Native popups can resize to fit the screen. Position from the preferred
    // size so those adjustments cannot feed back into another move/resize.
    x: parent ? Math.round((parent.width - implicitWidth) / 2) : 0
    y: -implicitHeight - 8
    margins: 12
    horizontalPadding: 12
    verticalPadding: Theme.spaceS
    font.family: "Segoe UI"
    font.pixelSize: Theme.fontBody
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
        | T.Popup.CloseOnReleaseOutsideParent

    property PopupMotion popupMotion: PopupMotion {
        popup: tip
        enterDuration: Theme.timings.tooltips
        exitDuration: Theme.timings.tooltips
    }
    enter: popupMotion.enter
    exit: popupMotion.exit

    // Qt can hand the shared tooltip to another control before the old hover
    // ends. Close the old hint so the following show starts a fresh reveal.
    // Otherwise Qt keeps it open and skips the enter transition entirely.
    onParentChanged: {
        if (visible) {
            close();
            // A different hint needs a fresh reveal, unlike reversing a hover
            // on the same control partway through its closing animation.
            popupMotion.progress = 0;
        }
    }

    contentItem: Text {
        text: tip.text
        textFormat: Text.PlainText
        font: tip.font
        color: Theme.ink
        wrapMode: Text.Wrap
        lineHeight: 1.2
    }

    background: Rectangle {
        border.width: 1
        border.color: Theme.outlineStrong
        color: Theme.surface
    }
}
