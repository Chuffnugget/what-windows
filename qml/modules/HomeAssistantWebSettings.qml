import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ModuleCard {
    id: card

    readonly property var web: controller.homeAssistantWeb
    readonly property var modes: ["windowed", "fullscreen", "desktop", "pip"]
    property bool settingsReady: false
    readonly property bool dirty: settingsReady && (address.text.trim() !== web.url
        || modes[viewMode.currentIndex] !== web.mode || renderScale.value !== web.scale
        || shortcut.text.trim() !== web.hotkey
        || Object.keys(advanced.draft).some(key => advanced.draft[key] !== advanced.saved[key]))
    signal clearDataRequested
    moduleKey: "haweb"
    heading: "Home Assistant Web"
    summary: "Your dashboard inside WHAT, on your desktop or above your apps"
    keywords: "home assistant web dashboard browser overlay fullscreen window desktop picture pip hotkey scale zoom cache login"
    toggleHelp: "Enable the embedded dashboard, global hotkeys and Web tray actions. Disabling closes the browser but keeps your saved login."

    function readSettings() {
        address.text = web.url;
        viewMode.currentIndex = modes.indexOf(web.mode);
        renderScale.value = web.scale;
        shortcut.text = web.hotkey;
        advanced.readSettings();
        settingsReady = true;
    }

    Component.onCompleted: readSettings()

    Connections {
        target: card.web
        function onScaleChanged() {
            // Refresh only zoom; keep any unsaved address and option edits.
            renderScale.value = card.web.scale;
        }
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: "Sign in through the dashboard. This browser has its own saved login, separate from WHAT's integration pairing and your other browsers."
    }

    HintLabel {
        text: "Dashboard Address"
    }

    InputField {
        id: address
        objectName: "hawebAddress"
        Layout.fillWidth: true
        placeholderText: "http://homeassistant.local:8123/lovelace"
        maximumLength: 2048
        ToolTip.text: "The HTTP or HTTPS address of your Home Assistant dashboard. Local and Home Assistant Cloud addresses both work when this PC can reach them. Do not include passwords or tokens in the address."
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceM

        HintLabel {
            text: "Display Mode"
            Layout.fillWidth: true
        }

        ChoiceBox {
            id: viewMode
            objectName: "hawebMode"
            Layout.preferredWidth: 230
            model: ["Inside WHAT", "Fullscreen Overlay", "Desktop (Keep Taskbar)", "Picture-in-Picture"]
            ToolTip.text: "Choose how the dashboard opens. Desktop starts with WHAT and stays behind your apps. Picture-in-picture stays above them and remembers its size and position."
        }

        HintLabel {
            text: "Render Scale"
        }

        NumberInput {
            id: renderScale
            objectName: "hawebScale"
            from: 50
            to: 200
            stepSize: 10
            unit: "%"
            ToolTip.text: "Zoom the dashboard inside WHAT, on the desktop and fullscreen from 50% to 200%. PiP has its own Dashboard Zoom setting under Display and Audio. This does not resize WHAT's controls."
        }
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: viewMode.currentIndex === 2
            ? "Desktop covers the wallpaper and icons on one monitor while WHAT runs, leaving the taskbar visible. Press Win+D to view it. Choose a monitor below, or use Automatic and Open Dashboard to move it to the pointer's monitor. The hotkey brings it forward fullscreen. Change mode or disable this module to restore your desktop."
            : viewMode.currentIndex === 3
            ? "Drag the picture-in-picture title bar to move it. Drag any edge or corner to resize it. Collapse hides the toolbar; Minimise rolls the window up to its title. Both hotkeys toggle picture-in-picture in this mode."
            : "The hotkey toggles a fullscreen overlay, including while WHAT is in the tray. Escape dismisses Home Assistant dialogs first, then closes the overlay. F6 focuses the browser toolbar."
    }

    RowLayout {
        Layout.fillWidth: true

        HintLabel {
            text: "Overlay Hotkey"
            Layout.fillWidth: true
        }

        InputField {
            id: shortcut
            objectName: "hawebHotkey"
            Layout.preferredWidth: 230
            placeholderText: "Ctrl+Alt+H"
            maximumLength: 80
            ToolTip.text: "Type a combination such as Ctrl+Alt+H or Ctrl+Shift+F12. Use Ctrl, Alt or Meta (Windows) with a letter, number, function key or Space. Leave blank to disable. The hotkey always toggles the overlay; Escape gives Home Assistant's dialogs priority."
        }
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.warning
        visible: text.length > 0
        text: card.web.hotkeyError
    }

    HomeAssistantWebOptions {
        id: advanced
        web: card.web
    }

    HintLabel {
        objectName: "hawebSaveStatus"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: card.dirty ? "Unsaved Changes - Apply to use these settings." : "All Changes Saved"
        color: card.dirty ? Theme.warning : Theme.muted
    }

    RowLayout {
        Layout.fillWidth: true
        ActionButton {
            objectName: "hawebApply"
            text: "Apply"
            primary: true
            enabled: card.dirty && !card.web.cacheBusy
            ToolTip.text: "Save the address, view, scale, hotkeys, tray click action and advanced options. Site permissions are saved separately as you change them."
            onClicked: {
                if (card.web.configure(address.text, card.modes[viewMode.currentIndex], renderScale.value, shortcut.text, advanced.draft)) {
                    card.readSettings();
                }
            }
        }

        QuietButton {
            objectName: "hawebRevert"
            text: "Revert Changes"
            enabled: card.dirty && !card.web.cacheBusy
            ToolTip.text: "Discard unsaved dashboard settings. Site permissions are saved immediately and are not reverted."
            onClicked: card.readSettings()
        }

        QuietButton {
            text: "Open Dashboard"
            enabled: card.web.url.length > 0 && !card.web.cacheBusy
            ToolTip.text: "Open the saved dashboard using your selected view. Apply changes first."
            onClicked: card.web.open()
        }

        QuietButton {
            text: "Open in Browser"
            enabled: card.web.url.length > 0
            ToolTip.text: "Open the current dashboard address in your default browser. That browser uses its own sign-in."
            onClicked: card.web.openInBrowser()
        }
    }

    Rectangle {
        Layout.fillWidth: true
        height: 1
        color: Theme.separator
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: "Clear Cache reloads dashboard files and keeps your login. Clear Login and Site Data signs you out of this browser only."
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.spaceS

        ActionButton {
            text: card.web.cacheBusy ? "Clearing…" : "Clear Cache"
            enabled: !card.web.cacheBusy
            ToolTip.text: "Remove cached files and service workers, then reload. Cookies and local storage, including your Home Assistant login, are kept."
            onClicked: card.web.clearCache(false)
        }

        QuietButton {
            text: "Clear Login and Site Data"
            enabled: !card.web.cacheBusy
            ToolTip.text: "Ask before clearing this browser's cookies, local storage and cache. WHAT settings and integration pairing are kept."
            onClicked: card.clearDataRequested()
        }

        QuietButton {
            text: "WebView2 Runtime"
            ToolTip.text: "Open Microsoft's WebView2 Runtime download page if the browser cannot start. Windows normally keeps this runtime updated."
            onClicked: Qt.openUrlExternally("https://developer.microsoft.com/microsoft-edge/webview2/")
        }
    }

    HintLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: card.web.status
    }
}
