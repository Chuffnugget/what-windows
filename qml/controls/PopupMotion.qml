import QtQuick
import "../components"

QtObject {
    id: popupMotion

    required property QtObject popup
    property real progress: 0
    property int enterDuration: Theme.timings.dialogEnter
    property int exitDuration: Theme.timings.dialogExit
    readonly property bool running: enter.running || exit.running

    // Capture the complete popup so the lens can bend text across its padding.
    // A second, frozen background lets the shader soften only the foreground.
    property OpenCloseLayer lens: OpenCloseLayer {
        target: popupMotion.popup.contentItem ? popupMotion.popup.contentItem.parent : null
        progress: popupMotion.progress
        allowed: popupMotion.running
        maximumBlur: Theme.motionBlurRadius
        backgroundSource: popupMotion.backgroundSnapshot
        backgroundRect: {
            const background = popupMotion.popup.background;
            return background ? Qt.rect(background.x, background.y, background.width, background.height)
                : Qt.rect(0, 0, 1, 1);
        }
        effectsEnabled: Theme.openCloseEnabled && Theme.shaderEffectsSupported
    }

    property ShaderEffectSource backgroundSnapshot: ShaderEffectSource {
        parent: popupMotion.lens.target
        sourceItem: popupMotion.lens.active ? popupMotion.popup.background : null
        visible: false
        hideSource: false
        live: false
    }

    property Transition enter: PopupTransition {
        motion: popupMotion
    }
    property Transition exit: PopupTransition {
        motion: popupMotion
        entering: false
    }
}
