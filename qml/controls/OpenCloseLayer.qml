import QtQuick

// Capture only while moving. At rest, the original items render normally and
// Qt releases the offscreen layer and shader. Shared by windows and popups.
QtObject {
    id: effect

    property Item target: null
    // Separate window surfaces share one effect coordinate system. Ordinary
    // controls use their own size and an origin of zero.
    property size viewportSize: Qt.size(target ? target.width : 0, target ? target.height : 0)
    property point viewportOffset: Qt.point(0, 0)
    property real progress: 1
    property int mode: Theme.transitionMode
    property real maximumBlur: 0
    property Item backgroundSource: null
    property rect backgroundRect: Qt.rect(0, 0, 1, 1)
    property bool effectsEnabled: Theme.shaderTransitionEnabled
    property bool allowed: progress > 0 && progress < 1
    readonly property bool active: allowed && effectsEnabled && target
        && target.visible && progress < 1 && target.width > 0 && target.height > 0
        && target.GraphicsInfo.api !== GraphicsInfo.Software
        // Bound temporary render targets on very large displays.
        && target.width * target.height < 8000000
        && target.width < 8192 && target.height < 8192

    property Binding layerEnabled: Binding {
        target: effect.target
        property: "layer.enabled"
        value: true
        when: effect.active
        restoreMode: Binding.RestoreBindingOrValue
    }

    property Binding layerEffect: Binding {
        target: effect.target
        property: "layer.effect"
        value: shader
        when: effect.active
        restoreMode: Binding.RestoreBindingOrValue
    }

    property Binding layerSampling: Binding {
        target: effect.target
        property: "layer.smooth"
        value: true
        when: effect.active
        restoreMode: Binding.RestoreBindingOrValue
    }

    // The short reveal only needs one picture of the content. Recapturing live
    // readings and animations underneath it adds an offscreen pass every frame.
    property Binding layerLive: Binding {
        target: effect.target
        property: "layer.live"
        value: false
        when: effect.active
        restoreMode: Binding.RestoreBindingOrValue
    }

    property Component shader: Component {
        ShaderEffect {
            property variant source
            property real progress: effect.progress
            property int mode: effect.mode
            property real blurRadius: effect.maximumBlur * (1 - Math.max(0, Math.min(1, effect.progress)))
            property real sourcePixelRatio: Screen.devicePixelRatio
            property variant sharpBackground: effect.backgroundSource || source
            property bool preserveBackground: effect.backgroundSource !== null
            property vector4d backgroundRect: Qt.vector4d(effect.backgroundRect.x,
                effect.backgroundRect.y, effect.backgroundRect.width, effect.backgroundRect.height)
            property real strength: Theme.refractionStrength / 100
            property vector2d resolution: Qt.vector2d(effect.viewportSize.width, effect.viewportSize.height)
            property vector4d sourceRect: Qt.vector4d(effect.viewportOffset.x, effect.viewportOffset.y, width, height)
            property color tint: Theme.accent
            fragmentShader: "qrc:/shaders/openclose.frag.qsb"
        }
    }
}
