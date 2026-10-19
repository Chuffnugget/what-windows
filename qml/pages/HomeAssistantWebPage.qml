import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

Item {
    id: page

    required property var web
    property alias browserSlot: browserSlot
    signal settingsRequested

    Item {
        id: browserSlot
        anchors.fill: parent
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(560, parent.width - 48)
        spacing: Theme.spaceM
        visible: !page.web.enabled || !page.web.url || page.web.overlayVisible || page.web.desktopActive

        Label {
            Layout.fillWidth: true
            text: page.web.overlayVisible ? (page.web.overlayMode === "pip" ? "Your Dashboard Is in Picture-in-Picture" : "Your Dashboard Is Open Fullscreen") : page.web.desktopActive ? "Your Dashboard Is on the Desktop" : "Home Assistant Web"
            color: Theme.ink
            font.pixelSize: Theme.fontHeading
            wrapMode: Text.WordWrap
        }

        HintLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: page.web.desktopActive ? "Desktop mode keeps the dashboard behind your other apps. Change Display Mode in Modules to bring it inside WHAT." : page.web.overlayVisible ? "Close the overlay to bring your dashboard back here. Your page and login stay open." : "Enable the module and enter your dashboard address in Modules. Sign in to Home Assistant once, then use it here or open it over your other apps with a hotkey."
        }

        ActionButton {
            text: page.web.overlayVisible ? (page.web.desktopActive ? "Return to Desktop" : "Return to This Page") : "Configure Module"
            ToolTip.text: page.web.overlayVisible ? "Close the overlay and return this browser session to WHAT or the desktop." : "Open the Home Assistant Web module settings."
            onClicked: page.web.overlayVisible ? page.web.hideOverlay() : page.settingsRequested()
        }
    }
}
