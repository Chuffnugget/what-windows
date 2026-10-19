import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"
import "../components"
import "../sensors"

ColumnLayout {
    id: sensorsPage

    objectName: "sensorsPage"
    required property var controller
    readonly property var status: controller.info
    readonly property string publishingHelp: "Normal: " + (status.publishingNormalMs || 1000)
        + " ms (" + (status.publishingNormalMs || 1000) / 1000 + " s). Fast: "
        + (status.publishingHighMs || 100) + " ms (" + (status.publishingHighMs || 100) / 1000 + " s)."
        + " Enable Fast to use the shorter interval; leave it off for Normal. Change these rates in Settings."
        + " These intervals control changed readings sent to Home Assistant, not source sampling or connection checks."
    property bool ready: false
    // One scroll follower for the expanded editor, shared by recycled rows.
    property ExpansionScroll expansionScroll: ExpansionScroll {
        targetItem: null
        expanded: sensorsPage.expandedId.length > 0
    }
    property string expandedId: ""
    onExpandedIdChanged: if (!expandedId) controller.requestHaEntity({})
    onVisibleChanged: if (!visible) controller.requestHaEntity({})
    property bool bulkOpen: false
    property bool displaysOpen: false
    property var pendingColours: ({})
    readonly property bool listLoading: !ready || controller.browserLoading
    property Item backdropSource: null
    signal iconRequested(string sensorId, string sensorName, bool appIcon, bool replaceDefault)
    signal customSensorRequested
    signal customLibraryRequested
    signal customEditRequested(string identifier)
    spacing: 6

    ContextMenu {
        id: iconMenu

        objectName: "sensorIconMenu"
        property string sensorId: ""
        property string sensorName: ""

        ContextMenuItem {
            objectName: "setReadingAppIcon"
            text: "Set App Icon…"
            onTriggered: sensorsPage.iconRequested(iconMenu.sensorId, iconMenu.sensorName, true, true)
        }

        ContextMenuItem {
            objectName: "setReadingTrayIcon"
            text: "Set System Tray Icon…"
            onTriggered: sensorsPage.iconRequested(iconMenu.sensorId, iconMenu.sensorName, false, true)
        }

        ContextMenuItem {
            objectName: "addReadingAppIcon"
            text: "Add App Icon…"
            onTriggered: sensorsPage.iconRequested(iconMenu.sensorId, iconMenu.sensorName, true, false)
        }

        ContextMenuItem {
            objectName: "addReadingTrayIcon"
            text: "Add System Tray Icon…"
            onTriggered: sensorsPage.iconRequested(iconMenu.sensorId, iconMenu.sensorName, false, false)
        }
    }

    Connections {
        target: sensorsPage.controller

        function onLocalIcueReply(id, ok, message) {
            // Replies can arrive after an expanded row has closed or been recycled.
            const colour = sensorsPage.pendingColours[id];
            if (colour === undefined)
                return;
            delete sensorsPage.pendingColours[id];
            if (ok)
                sensorsPage.controller.colourPalette.remember(colour);
        }
    }

    // Restore the filters before allowing their change handlers to submit a search.
    Component.onCompleted: {
        customFilter.checked = status.browserSource === "all" || status.browserSource.split(",").indexOf("custom") >= 0;
        search.text = status.browserQuery;
        wallpaperFilter.checked = status.browserSource === "all"
            || status.browserSource.split(",").indexOf("wallpaper") >= 0;
        beacnFilter.checked = status.browserSource === "all" || status.browserSource.split(",").indexOf("beacn") >= 0;
        coretempFilter.checked = status.browserSource === "all"
            || status.browserSource.split(",").indexOf("coretemp") >= 0;
        hwinfoFilter.checked = status.browserSource === "all" || status.browserSource.split(",").indexOf("hwinfo") >= 0;
        windowsFilter.checked = status.browserSource === "all"
            || status.browserSource.split(",").indexOf("windows") >= 0;
        icueFilter.checked = status.browserSource === "all" || status.browserSource.split(",").indexOf("icue") >= 0;
        nvidiaFilter.checked = status.browserSource === "all" || status.browserSource.split(",").indexOf("nvidia") >= 0;
        sharingFilter.currentIndex = status.browserUnshared ? 2 : status.browserSelected ? 1 : 0;
        favouriteFilter.checked = status.browserFavourites;
        Qt.callLater(function () {
            sensorsPage.ready = true;
        });
    }

    // C++ filters the catalogue on its worker; QML only submits the current controls.
    function applyFilters() {
        if (!ready)
            return;
        var sources = [];
        if (wallpaperFilter.checked)
            sources.push("wallpaper");
        if (beacnFilter.checked)
            sources.push("beacn");
        if (coretempFilter.checked)
            sources.push("coretemp");
        if (hwinfoFilter.checked)
            sources.push("hwinfo");
        if (windowsFilter.checked)
            sources.push("windows");
        if (icueFilter.checked)
            sources.push("icue");
        if (nvidiaFilter.checked)
            sources.push("nvidia");
        if (customFilter.checked)
            sources.push("custom");
        const source = sources.length === 8 ? "all" : sources.length ? sources.join(",") : "none";
        const changed = search.text !== status.browserQuery
            || source !== status.browserSource
            || (sharingFilter.currentIndex === 1) !== status.browserSelected
            || (sharingFilter.currentIndex === 2) !== status.browserUnshared
            || favouriteFilter.checked !== status.browserFavourites;
        controller.setBrowserFilters(search.text,
            source,
            sharingFilter.currentIndex === 1,
            favouriteFilter.checked,
            sharingFilter.currentIndex === 2);
        stopScrolling();
        // Initial filter setup can complete after a row has already opened.
        if (changed) {
            expandedId = "";
            sensorList.positionViewAtBeginning();
        }
    }

    function stopScrolling() {
        // Explicit list actions take priority over a pending scroll refresh.
        sensorWheel.cancelWheel();
        sensorList.cancelFlick();
        wheelQuiet.stop();
        controller.setBrowserInteracting(false);
    }

    // Keep the transition flag set until ListView consumes the cached model change.
    function foldCategory(category, close) {
        sensorList.categoryChange = true;
        controller.setCategoryCollapsed(category, close);
        stopScrolling();
        sensorList.forceLayout();
        sensorList.categoryChange = false;
    }

    function foldGroups(close) {
        expandedId = "";
        sensorList.categoryChange = true;
        controller.collapseCategories(close);
        stopScrolling();
        sensorList.forceLayout();
        sensorList.categoryChange = false;
    }

    function rowName(row) {
        if (!row.header || row.depth > 0)
            return row.name;
        const module = ({
                hwinfo: "HWiNFO",
                coretemp: "Core Temp",
                beacn: "BEACN",
                windows: "Windows",
                icue: "iCUE",
                nvidia: "NVIDIA",
                wallpaper: "Wallpaper Engine"
            })[(row.category || "").split(" / ")[0]];
        return module
            && row.name !== module
            && !row.name.startsWith(module + " / ")
            ? module + " / " + row.name
            : row.name;
    }

    function showValue(row) {
        if (!row.available || row.value === null || row.value === undefined)
            return {
                value: "Unavailable",
                unit: ""
            };
        if (row.kind === "binary")
            return {
                value: row.value ? "On" : "Off",
                unit: ""
            };
        if (row.kind === "number") {
            // Temperatures use one decimal. Other whole readings stay compact.
            const temperature = ["C", "°C", "F", "°F", "K"].includes(row.unit);
            const rounded = Number(Number(row.value).toFixed(temperature ? 1 : 2));
            const decimals = temperature ? 1 : row.unit === "B" || Number.isInteger(rounded) ? 0
                : Math.abs(rounded * 10 - Math.round(rounded * 10)) < 1e-9 ? 1 : 2;
            return {
                value: rounded.toLocaleString(Qt.locale(), 'f', decimals),
                unit: row.unit || ""
            };
        }
        return {
            value: String(row.value) || "(empty)",
            unit: ""
        };
    }

    function humanValue(row) {
        if (!row.available || row.kind !== "number" || row.value === null)
            return null;
        const bits = row.unit === "bit/s" || row.unit === "b/s" || row.unit === "bps";
        const bytes = row.unit === "B" || row.unit === "B/s";
        if (!bits && !bytes)
            return null;
        const base = bits ? 1000 : 1024;
        let value = Number(row.value), index = 0;
        const units = bits
            ? ["bit/s", "kb/s", "Mb/s", "Gb/s", "Tb/s", "Pb/s", "Eb/s"]
            : ["B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"];
        if (!isFinite(value) || Math.abs(value) < base)
            return null;
        while (Math.abs(value) >= base && index < units.length - 1) {
            value /= base;
            ++index;
        }

        return {
            value: Number(value.toFixed(2)).toLocaleString(Qt.locale(), 'f', Number.isInteger(Number(value.toFixed(2))) ? 0 : 2).replace(/([.,]\d)0$/, "$1"),
            unit: units[index] + (!bits && row.unit === "B/s" ? "/s" : "")
        };
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.rightMargin: Theme.scrollGutter
        spacing: 6

        InputField {
            id: search

            objectName: "sensorSearch"
            compact: true
            Layout.fillWidth: true
            leftPadding: 10
            rightPadding: 30
            color: Theme.ink
            placeholderTextColor: Theme.muted
            font.pixelSize: Theme.fontCaption
            placeholderText: "Search readings, devices or controls…"
            selectByMouse: true

            onTextChanged: filterDelay.restart()

            onAccepted: {
                filterDelay.stop();
                sensorsPage.applyFilters();
            }

            BrowserButton {
                objectName: "clearSensorSearch"
                anchors.right: parent.right
                anchors.rightMargin: 2
                anchors.verticalCenter: parent.verticalCenter
                width: 28
                height: 28
                z: 1
                visible: search.text.length > 0
                text: "×"
                subtle: true
                help: "Clear the search."
                Accessible.name: "Clear search"

                onClicked: {
                    search.clear();
                    filterDelay.stop();
                    sensorsPage.applyFilters();
                    search.forceActiveFocus();
                }
            }
        }

        BrowserChoice {
            id: sharingFilter

            objectName: "sharingFilter"
            implicitWidth: 117
            model: ["All Readings", "Shared", "Unshared"]
            ToolTip.text: "Filter by sharing state without changing your selection."
            onActivated: sensorsPage.applyFilters()
        }

        BrowserButton {
            id: favouriteFilter

            text: "Favourites"
            checkable: true
            Accessible.name: "Show Favourites"
            help: "Show only your favourite readings and controls."
            onClicked: sensorsPage.applyFilters()
        }

        BrowserButton {
            text: "Share…"
            selected: sensorsPage.bulkOpen
            help: "Sharing actions for all, filtered or recommended sensors."
            onClicked: sensorsPage.bulkOpen = !sensorsPage.bulkOpen
        }
        BrowserButton {
            objectName: "addCustomSensor"
            text: "Add Custom Sensor"
            help: "Choose what to monitor, test its reading and save it. Home Assistant sharing is selected separately."
            onClicked: sensorsPage.customSensorRequested()
        }
        BrowserButton {
            text: "Manage Custom Sensors"
            subtle: true
            help: "Edit or remove existing custom definitions, including disabled sensors."
            onClicked: sensorsPage.customLibraryRequested()
        }
    }

    Timer {
        id: filterDelay

        // Coalesce edits in this event-loop turn without adding a typing delay.
        interval: 0
        onTriggered: sensorsPage.applyFilters()
    }

    Flow {
        objectName: "sensorFilterFlow"
        Layout.rightMargin: Theme.scrollGutter
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredWidth: 0
        spacing: Theme.spaceXs

        BrowserButton {
            id: windowsFilter

            objectName: "windowsFilter"
            visible: sensorsPage.status.windowsEnabled
            text: "Windows"
            checkable: true
            checked: true
            help: "Show or hide Windows readings. This does not enable or disable the module."
            onClicked: sensorsPage.applyFilters()
        }

        BrowserButton {
            id: hwinfoFilter

            objectName: "hwinfoFilter"
            visible: sensorsPage.status.hwinfoEnabled
            text: "HWiNFO"
            checkable: true
            checked: true
            help: "Show or hide HWiNFO readings. This does not enable or disable the module."
            onClicked: sensorsPage.applyFilters()
        }

        BrowserButton {
            id: coretempFilter

            objectName: "coretempFilter"
            visible: sensorsPage.status.coretempEnabled
            text: "Core Temp"
            checkable: true
            checked: true
            help: "Show or hide Core Temp readings. This does not enable or disable the module."
            onClicked: sensorsPage.applyFilters()
        }

        BrowserButton {
            id: nvidiaFilter

            objectName: "nvidiaFilter"
            visible: sensorsPage.status.nvidiaEnabled
            text: "NVIDIA"
            checkable: true
            checked: true
            help: "Show or hide NVIDIA readings. This does not enable or disable the module."
            onClicked: sensorsPage.applyFilters()
        }

        BrowserButton {
            id: beacnFilter

            objectName: "beacnFilter"
            visible: controller.beacn.enabled === true
            text: "BEACN"
            checkable: true
            checked: true
            help: "Show or hide BEACN readings. This does not enable or disable the module."
            onClicked: sensorsPage.applyFilters()
        }

        BrowserButton {
            id: icueFilter

            objectName: "icueFilter"
            visible: sensorsPage.status.icueEnabled
            text: "iCUE"
            checkable: true
            checked: true
            help: "Show or hide iCUE readings. This does not enable or disable the module."
            onClicked: sensorsPage.applyFilters()
        }

        BrowserButton {
            id: wallpaperFilter

            objectName: "wallpaperFilter"
            visible: controller.wallpaper.enabled === true
            text: "Wallpaper"
            checkable: true
            checked: true
            help: "Show or hide Wallpaper readings. This does not enable or disable the module."
            onClicked: sensorsPage.applyFilters()
        }

        BrowserButton {
            id: customFilter
            objectName: "customFilter"
            visible: controller.customSources.enabled
            text: "Custom"
            checkable: true
            checked: true
            help: "Show or hide custom sensors and saved commands."
            onClicked: sensorsPage.applyFilters()
        }

        BrowserButton {
            objectName: "expandSensorGroups"
            text: "Expand Groups"
            subtle: true
            help: "Show the readings in every category."
            onClicked: sensorsPage.foldGroups(false)
        }

        BrowserButton {
            objectName: "collapseSensorGroups"
            text: "Collapse Groups"
            subtle: true
            help: "Fold every category to scan the device groups."
            onClicked: sensorsPage.foldGroups(true)
        }

        BrowserButton {
            visible: controller.displays.enabled && windowsFilter.checked
            text: "Display Layouts"
            selected: sensorsPage.displaysOpen
            help: "Manage saved monitor arrangements or rescan connected monitors."
            onClicked: sensorsPage.displaysOpen = !sensorsPage.displaysOpen
        }
    }

    Flow {
        Layout.fillWidth: true
        visible: sensorsPage.bulkOpen
        spacing: 5

        Repeater {
            model: ["Share Filtered", "Unshare Filtered", "Share Recommended", "Share All", "Unshare All"]

            BrowserButton {
                required property int index
                required property string modelData
                text: modelData
                help: index < 2
                    ? "Apply to every reading matching the current filters, including collapsed groups."
                    : "Apply to the full sensor catalogue."

                onClicked: {
                    if (index < 2) {
                        sensorsPage.applyFilters();
                        controller.selectFiltered(index === 0);
                    } else
                        controller.selectSensors(["recommended", "all", "none"][index - 2]);
                }
            }
        }
    }

    DisplayControls {
        visible: sensorsPage.displaysOpen && controller.displays.enabled && windowsFilter.checked
        controller: sensorsPage.controller
        backdropSource: sensorsPage.backdropSource
        animationsEnabled: Theme.animationsEnabled
        hoverDuration: Theme.timings.hover
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.surface
        border.color: Theme.outline

        Item {
            id: columns

            // Let long names use more width, while keeping the value column nearby.
            // The remaining space belongs to values such as titles, URLs and paths.
            readonly property real nameWidth: Math.min(560,
                420 + Math.max(0, sensorList.width - 800) * 0.2,
                Math.max(0, sensorList.width - 150 - 176))
            readonly property int accessWidth: 66
            height: 27
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 1
            anchors.rightMargin: Theme.scrollGutter

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 28
                spacing: 0

                Item {
                    Layout.preferredWidth: columns.nameWidth - 28
                    Layout.maximumWidth: Layout.preferredWidth
                    Layout.preferredHeight: 18

                    Caption {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.rightMargin: columns.accessWidth + 20
                        anchors.verticalCenter: parent.verticalCenter
                        text: "READING / CONTROL"
                        elide: Text.ElideRight
                    }

                    Caption {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: columns.accessWidth
                        text: "API ACCESS"
                        horizontalAlignment: Text.AlignHCenter
                        ToolTip.visible: apiAccessHover.hovered
                        ToolTip.text: "Read Only and Writable describe the underlying API's capabilities, not permissions you can change in WHAT. Home Assistant access is configured separately."
                        ToolTip.delay: Theme.toolTipDelay
                        ToolTip.timeout: Theme.toolTipTimeout

                        HoverHandler {
                            id: apiAccessHover
                        }
                    }
                }

                Caption {
                    text: "VALUE"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 150
                    Layout.minimumWidth: 150
                }

                Repeater {
                    model: ["LOCAL", "CLOUD"]

                    Item {
                        required property string modelData
                        Layout.preferredWidth: 44
                        Layout.preferredHeight: 18
                        PermissionBadge {
                            anchors.centerIn: parent
                            width: 40
                            compact: true
                            text: parent.modelData
                            access: parent.modelData === "LOCAL" ? PermissionBadge.Readings : PermissionBadge.Cloud
                            help: parent.modelData === "LOCAL"
                                ? "Select readings to share with Home Assistant over local connections. This does not grant control permission."
                                : "Select readings to share with Home Assistant over cloud connections. Module cloud permissions also apply. This does not grant control permission."
                        }
                    }
                }

                Caption {
                    text: "FAST"
                    Layout.preferredWidth: 44
                    horizontalAlignment: Text.AlignHCenter
                    ToolTip.text: sensorsPage.publishingHelp + "."
                    ToolTip.visible: fastHeadingHover.hovered

                    HoverHandler { id: fastHeadingHover }
                }

                Caption {
                    text: "FAV"
                    Layout.preferredWidth: 44
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }

        ListView {
            id: sensorList

            objectName: "sensorList"
            anchors.fill: parent
            anchors.topMargin: columns.height
            anchors.margins: 1
            anchors.rightMargin: Theme.scrollGutter
            clip: true
            reuseItems: true
            // Keep the same rows cached below an open editor. Otherwise collapse
            // recreates the displaced rows in the middle of the animation.
            property real editorSpace: 0
            cacheBuffer: 180
            onEditorSpaceChanged: editorCacheUpdate.restart()

            Timer {
                id: editorCacheUpdate

                interval: 0
                // Delegate creation can change editorSpace during cache layout.
                onTriggered: sensorList.cacheBuffer = 180 + Math.max(0, sensorList.editorSpace)
            }

            model: sensorsPage.ready ? controller.sensorModel : null
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            acceptedButtons: Qt.LeftButton
            property bool categoryChange: false

            // View transitions animate the instantiated rows, leaving the cached
            // model update immediate and avoiding per-frame row-height layout.
            add: Transition {
                objectName: "categoryRowsAdded"
                enabled: sensorList.categoryChange && Theme.animationsEnabled

                NumberAnimation {
                    property: "opacity"
                    from: 0
                    to: 1
                    duration: Theme.timings.panels
                    easing.type: Easing.OutCubic
                }

                NumberAnimation {
                    property: "revealOffset"
                    from: -16
                    to: 0
                    duration: Theme.timings.panels
                    easing.type: Easing.OutCubic
                }
            }

            remove: Transition {
                objectName: "categoryRowsRemoved"
                enabled: sensorList.categoryChange && Theme.animationsEnabled

                NumberAnimation {
                    property: "opacity"
                    to: 0
                    duration: Theme.timings.panels
                    easing.type: Easing.OutCubic
                }

                NumberAnimation {
                    property: "revealOffset"
                    to: -16
                    duration: Theme.timings.panels
                    easing.type: Easing.OutCubic
                }
            }

            displaced: Transition {
                objectName: "categoryRowsDisplaced"
                enabled: sensorList.categoryChange && Theme.animationsEnabled

                NumberAnimation {
                    property: "y"
                    duration: Theme.timings.lists
                    easing.type: Easing.OutCubic
                }

                NumberAnimation {
                    property: "revealOffset"
                    to: 0
                    duration: Theme.timings.lists
                    easing.type: Easing.OutCubic
                }

                // A rapid second click can interrupt an entering row's fade.
                NumberAnimation {
                    property: "opacity"
                    to: 1
                    duration: Theme.timings.lists
                    easing.type: Easing.OutCubic
                }
            }

            FastWheel {
                id: sensorWheel

                objectName: "sensorWheel"
                parent: sensorList
                flick: sensorList
                enabled: !sensorBar.pressed
                onScrollActivity: wheelQuiet.restart()
            }

            Timer {
                id: wheelQuiet

                interval: 160
            }

            // Defer live refreshes while scrolling so incoming samples do not disturb the gesture.
            readonly property bool browserBusy: visible && (moving || sensorBar.pressed || wheelQuiet.running)
            onBrowserBusyChanged: controller.setBrowserInteracting(browserBusy)
            onVisibleChanged: if (!visible)
                controller.setBrowserInteracting(false)

            ScrollBar.vertical: WideScrollBar {
                id: sensorBar

                objectName: "sensorScrollBar"
                parent: sensorList.parent
                anchors.top: sensorList.top
                anchors.bottom: sensorList.bottom
                anchors.right: parent.right
            }

            delegate: Rectangle {
                id: sensorRow

                required property var modelData
                required property int index
                required property var readingValue
                required property bool categoryCollapsed
                required property bool readingAvailable
                required property string readingReason
                property bool pooled: false
                property bool retiring: false
                property real revealOffset: 0
                enabled: !retiring
                // Pooled rows retain their bindings and may have interrupted
                // transitions. Hide them explicitly until ListView reuses them.
                visible: !pooled

                transform: Translate {
                    y: sensorRow.revealOffset
                }

                readonly property bool expanded: !modelData.header && sensorsPage.expandedId === modelData.id
                // Measure the complete editor before starting its reveal.
                property real expansion: expanded && !pooled && detail.status === Loader.Ready ? 1 : 0
                property real cachedSpace: 0
                readonly property real editorHeight: detail.item ? detail.item.implicitHeight + 18 : 0
                readonly property real detailHeight: expansion * editorHeight
                // Reserve the complete editor through its opening and closing.
                // The animated height must not resize ListView's cache each frame.
                readonly property real cacheReservation: !pooled && (expanded || expansion > 0) ? editorHeight : 0

                onCacheReservationChanged: {
                    const difference = cacheReservation - cachedSpace;
                    cachedSpace = cacheReservation;
                    sensorList.editorSpace += difference;
                }

                Component.onDestruction: if (sensorList)
                    sensorList.editorSpace -= cachedSpace
                readonly property int groupDepth: Math.max(0, modelData.depth || 0)
                readonly property bool mainCategory: modelData.header && groupDepth === 0
                readonly property int treeLevel: Math.min(groupDepth + (modelData.header ? 0 : 1), 4)
                readonly property int lineHeight: modelData.header ? 32 : 30
                readonly property var liveReading: ({
                        value: readingValue,
                        available: readingAvailable,
                        kind: modelData.kind,
                        unit: modelData.unit
                    })
                ListView.onRemove: retiring = true
                ListView.onPooled: {
                    pooled = true;
                    if (sensorsPage.expansionScroll.targetItem === sensorRow)
                        sensorsPage.expansionScroll.targetItem = null;
                }

                // A recycled delegate must not retain an interrupted fade or removal state.
                ListView.onReused: {
                    pooled = false;
                    retiring = false;
                    opacity = 1;
                    revealOffset = 0;
                }

                width: sensorList.width
                height: lineHeight + detailHeight
                color: expanded
                    ? Theme.selected
                    : rowHover.hovered
                    ? Theme.hover
                    : mainCategory
                    ? (categoryCollapsed ? Theme.category : Theme.categoryExpanded)
                    : modelData.header
                    ? (categoryCollapsed ? Theme.subcategory : Theme.subcategoryExpanded)
                    : index % 2
                    ? Theme.alternateRow
                    : Theme.surface
                // Only the moving reveal needs clipping. Clipping every row breaks
                // scene-graph batching across the visible list.
                clip: detail.active && expansion < 1

                Behavior on expansion {
                    enabled: !sensorRow.pooled && Theme.animationsEnabled

                    NumberAnimation {
                        duration: Theme.timings.panels
                        easing.type: Easing.OutCubic
                    }
                }

                // Keep the effect object with its recycled row. Destroying a
                // transient effect can leave Qt's layer binding holding it.
                // Only moving, visible rows allocate a render layer.
                property ExpansionBlur categoryBlur: ExpansionBlur {
                    target: rowContents
                    progress: sensorRow.opacity
                    allowed: !sensorRow.pooled && sensorList.categoryChange
                        && sensorRow.opacity > 0 && sensorRow.opacity < 1
                        && sensorRow.y + sensorRow.height >= sensorList.contentY
                        && sensorRow.y <= sensorList.contentY + sensorList.height
                }

                HoverHandler {
                    id: rowHover

                }

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    enabled: !sensorRow.modelData.header
                    onTapped: eventPoint => {
                        sensorsPage.stopScrolling();
                        // Copy the identity before the list can recycle this row.
                        iconMenu.sensorId = sensorRow.modelData.id;
                        iconMenu.sensorName = sensorRow.modelData.name;
                        const position = sensorRow.mapToItem(sensorsPage, eventPoint.position.x, eventPoint.position.y);
                        iconMenu.popup(position.x, position.y);
                    }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: sensorRow.expanded ? Theme.outline : Theme.separator
                }

                Rectangle {
                    objectName: "sensorSelectionStrip"
                    width: sensorRow.mainCategory ? 3 : 2
                    height: parent.height
                    color: sensorRow.expanded || (sensorRow.modelData.header && !sensorRow.categoryCollapsed)
                        ? Theme.selectionAccent : Theme.accent
                    visible: sensorRow.mainCategory || sensorRow.expanded

                    Behavior on color {
                        ColorAnimation { duration: Theme.animationsEnabled ? Theme.timings.controls : 0 }
                    }
                }

                Rectangle {
                    x: 16 + Math.max(0, sensorRow.treeLevel - 1) * 18
                    width: 1
                    height: parent.height
                    color: Theme.outline
                    visible: sensorRow.treeLevel > 0
                }

                FocusScope {
                    id: rowButton

                    objectName: "expand_" + (sensorRow.modelData.id || sensorRow.modelData.category)
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: sensorRow.lineHeight
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: (sensorRow.modelData.header
                        ? "Toggle category "
                        : sensorRow.expanded
                        ? "Close controls for "
                        : "Open controls for ") + sensorRow.modelData.name
                    Accessible.onPressAction: clicked()
                    signal clicked
                    Keys.onSpacePressed: clicked()
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()

                    TapHandler {
                        // A tap opens the row; a drag stays available to the list.
                        gesturePolicy: TapHandler.DragThreshold
                        onTapped: rowButton.clicked()
                    }

                    Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.width: rowButton.activeFocus ? 1 : 0
                        border.color: Theme.accent
                    }

                    onClicked: {
                        if (sensorRow.modelData.header) {
                            sensorsPage.foldCategory(sensorRow.modelData.category, !sensorRow.categoryCollapsed);
                        } else {
                            sensorsPage.expandedId = sensorRow.expanded ? "" : sensorRow.modelData.id;
                            if (sensorRow.expanded) {
                                sensorsPage.expansionScroll.targetItem = sensorRow;
                                sensorsPage.expansionScroll.follow();
                            }
                        }
                    }
                }

                RowLayout {
                    id: rowContents
                    height: sensorRow.lineHeight
                    anchors.left: parent.left
                    anchors.right: parent.right
                    spacing: 0

                    Item {
                        id: rowIndentation

                        Layout.preferredWidth: 28 + sensorRow.treeLevel * 18

                        Image {
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            source: "qrc:/assets/chevron.svg"
                            sourceSize.width: 16
                            sourceSize.height: 16
                            rotation: sensorRow.modelData.header
                                ? (sensorRow.categoryCollapsed ? -90 : 0)
                                : sensorRow.expanded
                                ? 0
                                : -90
                            opacity: sensorRow.modelData.header || sensorRow.expanded ? 1 : 0.55

                            Behavior on rotation {
                                NumberAnimation {
                                    duration: Theme.animationsEnabled ? Theme.timings.controls : 0
                                }
                            }
                        }
                    }

                    Item {
                        implicitHeight: readingName.implicitHeight
                        Layout.preferredWidth: Math.max(0, columns.nameWidth - rowIndentation.width)
                        Layout.maximumWidth: Layout.preferredWidth

                        Text {
                            id: readingName

                            anchors.fill: parent
                            anchors.rightMargin: accessBadge.width + 20
                            text: sensorsPage.rowName(sensorRow.modelData)
                            elide: Text.ElideRight
                            color: sensorRow.modelData.header || sensorRow.readingAvailable ? Theme.ink : Theme.muted
                            font.pixelSize: sensorRow.mainCategory ? 13 : 12
                            font.weight: sensorRow.mainCategory
                                ? Font.Bold
                                : sensorRow.modelData.header
                                ? Font.DemiBold
                                : Font.Normal
                            ToolTip.visible: rowHover.hovered && truncated
                            ToolTip.text: sensorRow.modelData.tooltip || text
                            ToolTip.delay: Theme.toolTipDelay
                        }

                        PermissionBadge {
                            id: accessBadge

                            objectName: "sensorWritability"
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: columns.accessWidth
                            compact: true
                            readonly property bool writable: sensorRow.modelData.header
                                ? sensorRow.modelData.writable === true
                                : !!sensorRow.modelData.control
                            text: writable ? "Writable" : "Read Only"
                            access: writable ? PermissionBadge.Controls : PermissionBadge.Blocked
                            help: (sensorRow.modelData.header
                                ? writable
                                    ? "Contains API controls or actions, including in collapsed subcategories. Labels follow the current filters."
                                    : "The API entries matching your filters in this category only provide readings."
                                : !writable
                                    ? "This API entry only provides a reading."
                                    : sensorRow.readingAvailable
                                        ? "The API supports changing this value or running an action. Expand the entry to use its controls."
                                        : "The API supports this control, but it is currently unavailable. Check its device connection and control requirements.")
                                + " This describes API capability, not a permission you can change in WHAT. Home Assistant access is configured separately."
                        }
                    }

                    Item {
                        id: summary

                        objectName: "readingSummary"
                        visible: sensorRow.modelData.control !== "custom"
                        implicitHeight: 18
                        Layout.fillWidth: true
                        Layout.preferredWidth: 150
                        Layout.minimumWidth: 150
                        readonly property var parts: {
                            const row = sensorRow.modelData;
                            if (row.header)
                                return {
                                    value: row.controlOnly ? "Controls" : row.selected + " / " + row.total + " shared",
                                    unit: ""
                                };
                            if (row.control === "beacn")
                                return {
                                    value: row.beacn.kind === "button"
                                        ? "Action"
                                        : String(row.beacn.value === undefined ? "Unavailable" : row.beacn.value),
                                    unit: ""
                                };
                            if (row.options) {
                                const option = row.options.find(o => o.id === sensorRow.readingValue);
                                if (option)
                                    return {
                                        value: option.name,
                                        unit: ""
                                    };
                            }

                            if (row.control === "release")
                                return {
                                    value: "Clear Override",
                                    unit: ""
                                };
                            if (row.control === "custom")
                                return {value: row.available ? "Run Command" : "Disabled", unit: ""};
                            if (row.control === "wallpaper")
                                return {
                                    value: "Playback",
                                    unit: ""
                                };
                            if (row.control === "rgb")
                                return {
                                    value: String(sensorRow.readingValue || "Unavailable").toUpperCase(),
                                    unit: ""
                                };
                            return sensorsPage.humanValue(sensorRow.liveReading)
                                || sensorsPage.showValue(sensorRow.liveReading);
                        }

                        Text {
                            id: valueLabel

                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(implicitWidth, Math.max(0, summary.width - (unitLabel.visible
                                ? unitLabel.width + 4
                                : 0) - 12))
                            elide: Text.ElideRight
                            font.pixelSize: Theme.fontCaption
                            font.family: sensorRow.modelData.header ? "Segoe UI" : "Consolas"
                            color: sensorRow.modelData.header
                                ? Theme.muted
                                : sensorRow.readingAvailable
                                ? Theme.accent
                                : Theme.muted
                            text: summary.parts.value
                        }

                        Text {
                            id: unitLabel

                            anchors.verticalCenter: parent.verticalCenter
                            x: valueLabel.width + 4
                            visible: text.length > 0
                            width: Math.min(implicitWidth, 150, summary.width / 2)
                            elide: Text.ElideRight
                            font.pixelSize: Theme.fontCaption
                            color: Theme.muted
                            text: summary.parts.unit
                        }
                    }

                    Repeater {
                        // These bits are shared with C++: local, cloud and fast publishing.
                        model: [1, 2, 4]

                        OptionCheckBox {
                            required property int modelData
                            objectName: "sharing_" + modelData
                            checkedColor: modelData === 2 ? Theme.warning
                                : modelData === 1 ? Theme.permissionReading : Theme.accent
                            Layout.preferredWidth: 44
                            implicitHeight: 26
                            visible: !sensorRow.modelData.controlOnly
                            readonly property var row: sensorRow.modelData
                            readonly property int selectedCount: row.header
                                ? ((row.sharingCounts || {})[modelData === 4 ? "high" : String(modelData)] || 0)
                                : modelData === 4
                                ? (row.highFrequency ? 1 : 0)
                                : (row.shareScope & modelData ? 1 : 0)
                            text: ""
                            indicatorCentered: true
                            checkState: selectedCount === 0
                                ? Qt.Unchecked
                                : row.header
                                && selectedCount < row.total
                                ? Qt.PartiallyChecked
                                : Qt.Checked
                            nextCheckState: function () {
                                return checkState === Qt.Checked ? Qt.Unchecked : Qt.Checked;
                            }

                            ToolTip.text: (modelData === 1
                                ? "Share over local connections"
                                : modelData === 2
                                ? "Share over cloud connections"
                                : sensorsPage.publishingHelp) + (row.header
                                ? ". Applies to this entire category."
                                : ".")
                            Accessible.name: row.name + ": " + (modelData === 1
                                ? "Local"
                                : modelData === 2
                                ? "Cloud"
                                : "Fast Updates")

                            onClicked: {
                                if (row.header)
                                    controller.setCategoryPublishing(row.category, modelData, checked);
                                else
                                    controller.setSensorPublishing(row.id, modelData === 4
                                        ? row.shareScope
                                        : checked
                                        ? row.shareScope | modelData
                                        : row.shareScope & ~modelData, modelData === 4
                                        ? checked
                                        : row.highFrequency, row.decimals);
                                sensorsPage.stopScrolling();
                            }
                        }
                    }

                    Item {
                        visible: sensorRow.modelData.controlOnly === true
                        Layout.fillWidth: sensorRow.modelData.control === "custom"
                        Layout.preferredWidth: sensorRow.modelData.control === "custom" ? 260 : 132
                        Layout.minimumWidth: Layout.preferredWidth
                        Layout.preferredHeight: 24
                        Loader {
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            active: sensorRow.modelData.controlOnly === true && sensorRow.modelData.source === "custom"
                            sourceComponent: PermissionBadges {
                                objectName: "savedCommandAccessBadge"
                                showRunAccess: true
                                runAllowed: sensorRow.modelData.haAllowed === true
                                localAllowed: runAllowed
                                cloudAllowed: sensorRow.modelData.cloudAllowed === true
                                runHelp: sensorRow.modelData.haAllowed
                                    ? "Home Assistant may trigger this saved command. Cloud execution also needs the command and module cloud permissions. Its script is edited on this PC."
                                    : "Home Assistant execution is blocked. Open Commands to inspect this command's permissions. Local runs keep their separate enabled setting."
                            }
                        }
                    }

                    OptionCheckBox {
                        Layout.preferredWidth: 44
                        implicitHeight: 26
                        checked: sensorRow.modelData.favourite === true
                        text: ""
                        indicatorCentered: true
                        indicatorVisible: !sensorRow.modelData.header
                        enabled: !sensorRow.modelData.header
                        Accessible.name: sensorRow.modelData.name + ": Favourite"
                        ToolTip.text: "Add or remove this reading from favourites. Sharing is unchanged."

                        onClicked: {
                            controller.setFavourite(sensorRow.modelData.id, checked);
                            sensorsPage.stopScrolling();
                        }
                    }
                }

                // Expansion owns the editor lifetime. Depending on height here would
                // create a cycle through the loaded item's implicit height.
                Loader {
                    id: detail

                    objectName: "sensorDetailLoader"
                    x: rowIndentation.width
                    y: sensorRow.lineHeight + 7
                    width: parent.width - x - 16
                    active: !sensorRow.pooled && (sensorRow.expanded || sensorRow.expansion > 0)
                    asynchronous: true
                    visible: active

                    sourceComponent: ColumnLayout {
                        id: sensorDetails

                        property ExpansionBlur expansionBlur: ExpansionBlur {
                            target: sensorDetails
                            progress: sensorRow.expansion
                        }
                        spacing: 7

                        HaEntityDetails {
                            Layout.fillWidth: true
                            controller: sensorsPage.controller
                            row: sensorRow.modelData
                            active: sensorRow.expanded && sensorsPage.visible
                        }

                        RowLayout {
                            visible: sensorRow.modelData.source === "custom"
                            Layout.fillWidth: true
                            BrowserButton {
                                text: sensorRow.modelData.controlOnly ? "Edit Saved Command" : "Edit Sensor"
                                help: "Edit this local definition. Existing entity IDs are kept."
                                onClicked: sensorsPage.customEditRequested(sensorRow.modelData.id)
                            }
                        }

                        // The collapsed command row already shows this exact group.
                        PermissionBadges {
                            objectName: "readingAccessBadge"
                            visible: !sensorRow.modelData.controlOnly
                            localAllowed: (sensorRow.modelData.shareScope & 1) !== 0
                            cloudAllowed: (sensorRow.modelData.shareScope & 2) !== 0
                        }

                        Text {
                            text: {
                                const description = sensorRow.readingReason || sensorRow.modelData.tooltip || "";
                                return description.split("\n").filter(line => line.trim() !== sensorRow.modelData.name).join("\n");
                            }

                            visible: text.length > 0
                            color: Theme.muted
                            font.pixelSize: Theme.fontCaption
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                        }

                        Loader {
                            active: !!sensorRow.modelData.control
                            visible: active
                            Layout.fillWidth: true

                            sourceComponent: BrowserDeviceControls {
                                controller: sensorsPage.controller
                                modelData: sensorRow.modelData
                                onColourSubmitted: (id, colour) => sensorsPage.pendingColours[id] = colour
                            }
                        }

                        TextEdit {
                            Layout.fillWidth: true
                            text: sensorRow.modelData.id || ""
                            readOnly: true
                            selectByMouse: true
                            color: Theme.muted
                            font.pixelSize: Theme.fontLabel
                            font.family: "Consolas"
                            wrapMode: TextEdit.WrapAnywhere
                            topPadding: 8
                        }
                    }
                }
            }

            Text {
                objectName: "sensorEmptyMessage"
                anchors.centerIn: parent
                visible: sensorList.count === 0 && !sensorsPage.listLoading
                text: "No readings match these filters. Check your filters and enabled modules."
                color: Theme.muted
                font.pixelSize: Theme.fontCaption
            }
        }

        ColumnLayout {
            objectName: "sensorLoading"
            anchors.centerIn: parent
            visible: sensorList.count === 0 && sensorsPage.listLoading

            BusyIndicator {
                objectName: "sensorLoadingSpinner"
                Layout.alignment: Qt.AlignHCenter
                palette.dark: Theme.accent
                running: visible && Theme.animationsEnabled
                visible: Theme.animationsEnabled
            }

            Text {
                text: "Loading Sensors…"
                color: Theme.muted
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true

        Text {
            text: sensorsPage.status.selected + " shared"
            color: Theme.accent
            font.pixelSize: Theme.fontSmall
        }

        Text {
            text: "Click a row to reveal its controls"
            color: Theme.muted
            font.pixelSize: Theme.fontSmall
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignRight
        }
    }

    Text {
        objectName: "beacnCommandResult"
        text: controller.beacn.last_command || ""
        visible: controller.beacn.enabled === true && text.length > 0
        color: Theme.muted
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: Theme.fontSmall
    }

    component Caption: Text {
        color: Theme.muted
        font.pixelSize: Theme.fontBadge
        font.letterSpacing: 0.7
    }
}
