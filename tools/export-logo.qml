import QtQuick
import QtQuick.Window
import "../qml/components"

// Run from a directory containing an empty frames/ subdirectory.
Window {
    id: window
    width: 1336; height: 1696; visible: true; color: "transparent"
    flags: Qt.FramelessWindowHint
    property int frame: 0
    AnimatedLogo { id: logo; anchors.fill: parent; animationsEnabled: false }
    Timer {
        id: capture; interval: 40; running: true; repeat: false
        onTriggered: logo.grabToImage(function(result) {
            if (!result.saveToFile("frames/frame"+String(window.frame).padStart(3,"0")+".png")) {
                console.error("Could not save logo frame"); Qt.exit(1); return
            }
            if(window.frame===75) { Qt.quit(); return }
            window.frame++
            logo.progress=window.frame/75
            capture.restart()
        })
    }
}
