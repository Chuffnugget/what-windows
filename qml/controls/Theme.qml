pragma Singleton
import QtQuick

QtObject {
    property string name: "blue"
    readonly property bool grey: name !== "blue"
    readonly property var choices: [
        { label: "Blue (Default)", name: "blue" },
        { label: "Dark Grey", name: "dark-grey" },
        { label: "Slate", name: "slate" },
        { label: "Forest", name: "forest" },
        { label: "Plum", name: "plum" }
    ]

    // Keep the same surface contrast in every palette. A low-opacity tint adds
    // colour to the neutral themes without changing the logo or warning colours.
    readonly property color surfaceTint: name === "slate" ? "#202e4965"
        : name === "forest" ? "#20264736"
        : name === "plum" ? "#20473755" : "transparent"

    function surfaceColour(blue, neutral) {
        return name === "blue" ? blue : Qt.tint(neutral, surfaceTint);
    }

    // Surface roles are shared by pages and controls. Borders identify inputs
    // and popups; ordinary lists and sections use separators instead.
    readonly property color canvas: surfaceColour("#10151e", "#1b1b1b")
    readonly property color sidebar: surfaceColour("#131b27", "#202020")
    readonly property color surface: surfaceColour("#111a23", "#222222")
    readonly property color inset: surfaceColour("#0d141d", "#171717")
    readonly property color field: surfaceColour("#111a27", "#242424")
    readonly property color button: surfaceColour("#202c3e", "#2c2c2c")
    readonly property color hover: surfaceColour("#273c4c", "#383838")
    readonly property color pressed: surfaceColour("#34556a", "#494949")
    readonly property color selected: surfaceColour("#182b39", "#303030")
    readonly property color expandedHeader: surfaceColour("#162331", "#2b2b2b")
    readonly property color category: surfaceColour("#293e4e", "#353535")
    readonly property color subcategory: surfaceColour("#1c2e3c", "#272727")
    readonly property color categoryExpanded: Qt.lighter(category, 1.12)
    readonly property color subcategoryExpanded: Qt.lighter(subcategory, 1.12)
    readonly property color alternateRow: surfaceColour("#16212b", "#262626")
    readonly property color separator: surfaceColour("#273343", "#333333")
    readonly property color outline: surfaceColour("#34435a", "#494949")
    readonly property color outlineStrong: surfaceColour("#526b80", "#727272")

    readonly property color ink: grey ? "#eeeeee" : "#e7edf6"
    readonly property color muted: grey ? "#a0a0a0" : "#8d9db2"
    readonly property color secondary: grey ? "#c6c6c6" : "#b5c8d8"
    readonly property color brand: "#1eb9e8"
    // Lighter accents stay legible for small text and keyboard focus.
    readonly property color accent: name === "forest" ? "#94d4af"
        : name === "plum" ? "#c6b2e8"
        : name === "slate" ? "#acc8e2" : "#66caff"
    readonly property color accentHover: name === "blue" || name === "dark-grey" ? "#9cddff" : Qt.lighter(accent, 1.15)
    readonly property color selectionAccent: Qt.tint(ink, Qt.rgba(accent.r, accent.g, accent.b, 0.15))
    readonly property color accentText: name === "blue" || name === "dark-grey" ? "#102738" : "#19211f"
    readonly property color warning: "#e6b572"
    readonly property color warningSurface: "#35291c"
    readonly property color warningOutline: "#936d38"
    readonly property color error: "#ff8585"
    readonly property color errorSurface: "#35232c"
    readonly property color errorOutline: "#a65262"
    // Permission colours keep their meaning when the overall theme changes.
    readonly property color permissionReading: "#66caff"
    readonly property color permissionRead: "#12232f"
    readonly property color permissionControl: warningSurface
    readonly property color closeHover: "#d83e50"
    readonly property color closePressed: "#b52d3d"

    // Glass is reserved for prompts and their frozen backdrop.
    readonly property color glassTop: surfaceColour("#203748", "#383838")
    readonly property color glassMiddle: surfaceColour("#142230", "#282828")
    readonly property color glassBottom: surfaceColour("#101922", "#202020")
    readonly property color glassTint: surfaceColour("#70202d40", "#70252525")
    readonly property color glassEdge: surfaceColour("#708cc0d2", "#70bcbcbc")

    // Colour data and branding keep their meaning in every theme.
    readonly property color checkerDark: "#aaaaaa"
    readonly property color checkerLight: "#eeeeee"
    readonly property color channelRed: "#ef8288"
    readonly property color channelGreen: "#65ccaf"
    readonly property color channelBlue: "#66baff"
    readonly property color brandTrace: "#d1f9ff"

    // Native tray menus, notices and colour dialogs share the QML palette.
    readonly property var widgetColours: ({
        canvas, field, button, hover, pressed, outline, separator, ink, muted,
        surface, accent, accentHover, accentText
    })

    readonly property int spaceXs: 4
    readonly property int spaceS: 8
    readonly property int spaceM: 12
    readonly property int spaceL: 16
    readonly property int spaceXl: 24
    readonly property int scrollBarWidth: 14
    readonly property int scrollGutter: scrollBarWidth + spaceS
    readonly property int controlHeight: 36
    readonly property int compactHeight: 28
    readonly property int sectionHeight: 64
    readonly property int fontBadge: 9
    readonly property int fontLabel: 10
    readonly property int fontSmall: 11
    readonly property int fontCaption: 12
    readonly property int fontBody: 13
    readonly property int fontSection: 16
    readonly property int fontHeading: 20
    readonly property int fontTitle: 28
    readonly property real disabledOpacity: 0.45

    readonly property int toolTipDelay: 600
    readonly property int toolTipTimeout: 12000
    readonly property real motionBlurRadius: 6
    property bool animationsEnabled: true
    property string transitionStyle: "refraction"
    property int refractionStrength: 100
    property bool shaderEffectsSupported: true
    property int transitionMode: 7
    readonly property bool openCloseEnabled: animationsEnabled && transitionStyle !== "none"
    readonly property bool shaderTransitionEnabled: openCloseEnabled && shaderEffectsSupported
        && transitionMode >= 0
    // Main binds these to the saved settings; controls can also run in isolation.
    property var timings: ({
            hover: 140,
            tooltips: 350,
            controls: 120,
            panels: 250,
            lists: 170,
            pages: 320,
            dialogEnter: 250,
            dialogExit: 180,
            logo: 1500,
            logoPause: 180,
            toast: 3500
        })
}
