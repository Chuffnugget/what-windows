import QtQuick
import "../controls"

Transition {
    id: transition

    property bool entering: true
    required property QtObject motion
    readonly property bool shaderEffect: Theme.shaderTransitionEnabled
    readonly property int duration: Theme.openCloseEnabled
        ? (entering ? motion.enterDuration : motion.exitDuration)
        : 0

    ParallelAnimation {
        NumberAnimation {
            target: transition.motion
            property: "progress"
            // Start from the current reveal when a hover or click reverses it.
            // The shader shapes its own reveal; easing it again causes a long
            // pause followed by a rush through the visible part of the effect.
            to: transition.entering ? 1 : 0
            duration: transition.duration
            easing.type: transition.shaderEffect ? Easing.Linear : Easing.InOutCubic
        }

        NumberAnimation {
            property: "scale"
            from: transition.shaderEffect ? 1 : transition.entering ? 0.94 : 1
            to: transition.shaderEffect ? 1 : transition.entering ? 1 : 0.97
            duration: transition.duration
            easing.type: transition.entering ? Easing.OutCubic : Easing.InCubic
        }

        NumberAnimation {
            property: "opacity"
            from: transition.shaderEffect ? 1 : transition.entering ? 0 : 1
            to: transition.shaderEffect ? 1 : transition.entering ? 1 : 0
            duration: transition.entering ? Math.min(80, transition.duration) : transition.duration
            easing.type: Easing.OutQuad
        }
    }
}
