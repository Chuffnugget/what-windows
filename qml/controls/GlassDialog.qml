import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"

Dialog {
    id: glassDialog

    required property Item backdropSource
    parent: Overlay.overlay

    // Native browser windows sit above QML. Hide the embedded view while
    // a modal prompt covers it; tooltips must not blank the dashboard.
    readonly property bool blocksWebView: visible && modal
    enter: null
    exit: null

    padding: 24

    Overlay.modal: GlassBackdrop {
        sourceItem: glassDialog.backdropSource
    }

    background: GlassSurface {}

    header: RowLayout {
        spacing: Theme.spaceM

        AnimatedLogo {
            Layout.leftMargin: 24
            Layout.topMargin: 24
            Layout.preferredWidth: 36
            Layout.preferredHeight: 46
            animationsEnabled: Theme.animationsEnabled
            animationDuration: Theme.timings.logo
            pauseDuration: Theme.timings.logoPause
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.rightMargin: 24
            Layout.topMargin: 24

            Label {
                text: "WHAT"
                color: Theme.accent
                font.pixelSize: Theme.fontCaption
                font.bold: true
            }

            Label {
                text: glassDialog.title
                color: Theme.ink
                font.pixelSize: Theme.fontHeading
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }

    footer: PromptButtons {
        standardButtons: glassDialog.standardButtons
        visible: count > 0
    }
}
