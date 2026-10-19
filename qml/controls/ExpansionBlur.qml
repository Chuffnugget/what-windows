import QtQuick

// Keep the slide's geometry and timing. Its fixed-size content snapshot blurs
// and bends at the revealing edge, then returns to normal rendering at rest.
OpenCloseLayer {
    mode: Theme.shaderTransitionEnabled ? 18 : -1
    maximumBlur: Theme.motionBlurRadius
    effectsEnabled: Theme.animationsEnabled && Theme.shaderEffectsSupported
}
