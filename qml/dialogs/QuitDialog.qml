import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: quitDialog

    signal quitRequested()
    signal hideRequested()

    objectName: "trayQuitConfirmation"
    title: "Quit WHAT?"
    parent: Overlay.overlay
    modal: true
    anchors.centerIn: parent
    width: Math.min(550, quitDialog.parent.width - 48)
    closePolicy: Popup.CloseOnEscape
    property string requestedAction: ""
    onAboutToShow: requestedAction = ""
    onOpened: confirmQuit.forceActiveFocus()
    // Close the modal before hiding the window or shutting down.
    onClosed: {
        if (requestedAction === "quit")
            quitDialog.quitRequested();
        else if (requestedAction === "hide")
            quitDialog.hideRequested();
    }

    contentItem: Label {
        text: "Quitting stops live sensors, tray readings and Home Assistant connections.\n\nSend to Tray keeps WHAT running in the background."
        textFormat: Text.PlainText
        color: Theme.ink
        wrapMode: Text.WordWrap
    }

    footer: RowLayout {
        spacing: Theme.spaceM
        Item { Layout.preferredWidth: 14 }
        ActionButton {
            objectName: "keepRunning"
            text: "Keep Running"
            Layout.topMargin: 8
            Layout.bottomMargin: 24
            onClicked: quitDialog.reject()
        }
        Item { Layout.fillWidth: true }
        ActionButton {
            objectName: "sendToTray"
            text: "Send to Tray"
            Layout.topMargin: 8
            Layout.bottomMargin: 24
            onClicked: {
                quitDialog.requestedAction = "hide";
                quitDialog.close();
            }
        }
        ActionButton {
            id: confirmQuit
            objectName: "confirmQuit"
            text: "Quit WHAT"
            primary: true
            Layout.topMargin: 8
            Layout.bottomMargin: 24
            onClicked: {
                quitDialog.requestedAction = "quit";
                quitDialog.close();
            }
            Keys.onReturnPressed: clicked()
            Keys.onEnterPressed: clicked()
        }
        Item { Layout.preferredWidth: 14 }
    }
}
