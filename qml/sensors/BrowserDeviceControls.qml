import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

Flow {
    id: devices

    required property var controller
    required property var modelData
    signal colourSubmitted(string id, string colour)
    spacing: Theme.spaceM

    BrowserButton {
        text: "Run Command"
        visible: modelData.control === "custom"
        enabled: modelData.available === true
        help: "Run this saved command on this PC. Its output and last result are shown on the Commands page."
        onClicked: controller.runCustomAction(modelData.id)
    }

    // Create the matching editor only while its sensor row is expanded.
    Loader {
        active: modelData.control === "wallpaper"
        visible: active
        width: devices.width

        sourceComponent: BrowserWallpaper {
            controller: devices.controller
        }
    }

    Loader {
        id: beacnLoader

        active: modelData.control === "beacn"
        visible: active
        property var row: modelData
        width: active ? Math.min(440, devices.width) : 0

        sourceComponent: Component {
            BrowserBeacn {
                controller: devices.controller
                control: beacnLoader.row.beacn
            }
        }
    }

    BrowserButton {
        checkable: true
        text: checked ? "On" : "Off"
        visible: modelData.control === "toggle"
        enabled: modelData.available === true
        checked: modelData.value === true
        Accessible.name: modelData.name
        ToolTip.visible: hovered
        ToolTip.text: "Change this setting locally in iCUE"
        onClicked: controller.localIcueCommand("icue_property", {
            device: modelData.icueDevice,
            property: modelData.icueProperty,
            value: checked
        })
    }

    Loader {
        id: displayLoader

        active: modelData.control === "display"
        visible: active
        property var row: modelData

        sourceComponent: Component {
            RowLayout {
                BrowserChoice {
                    objectName: "displayMode_" + displayLoader.row.monitor
                    implicitWidth: 235
                    model: displayLoader.row.options
                    textRole: "name"
                    valueRole: "id"
                    enabled: displayLoader.row.available
                    currentIndex: (displayLoader.row.options || []).findIndex(o => o.id === displayLoader.row.value)

                    onActivated: {
                        let parts = currentValue.split(":");
                        controller.localDisplayCommand({
                            operation: parts[0],
                            monitor: displayLoader.row.monitor,
                            source: parts.length > 1 ? parts[1] : ""
                        });
                    }
                }

                BrowserButton {
                    ToolTip.text: "Make this monitor the primary Windows display. Confirm the resulting arrangement to keep it."
                    text: displayLoader.row.primary ? "Primary" : "Make primary"
                    enabled: displayLoader.row.available
                        && displayLoader.row.desktopEnabled
                        && !displayLoader.row.primary
                    onClicked: controller.localDisplayCommand({
                        operation: "primary",
                        monitor: displayLoader.row.monitor
                    })
                }
            }
        }
    }

    Loader {
        id: presetLoader

        active: modelData.control === "preset"
        visible: active
        property var row: modelData

        sourceComponent: Component {
            BrowserChoice {
                enabled: presetLoader.row.available === true
                model: ["Preset 1", "Preset 2", "Preset 3", "Preset 4", "Preset 5"]
                currentIndex: Number(presetLoader.row.value) - 1
                implicitWidth: 115
                Accessible.name: presetLoader.row.name
                onActivated: controller.localIcueCommand("icue_property", {
                    device: presetLoader.row.icueDevice,
                    property: presetLoader.row.icueProperty,
                    value: currentIndex + 1
                })
            }
        }
    }

    Loader {
        id: automationLoader

        active: modelData.control === "automation"
        visible: active
        property var row: modelData

        sourceComponent: Component {
            BrowserChoice {
                objectName: "automationPreset_" + automationLoader.row.automationId
                enabled: automationLoader.row.available === true
                model: automationLoader.row.options || []
                textRole: "name"
                valueRole: "id"
                implicitWidth: 220
                Accessible.name: automationLoader.row.name
                currentIndex: {
                    const choices = automationLoader.row.options || [];
                    return choices.findIndex(o => o.id === automationLoader.row.value);
                }

                displayText: currentIndex >= 0 ? currentText : "Choose preset…"

                onActivated: {
                    const chosen = currentValue;
                    controller.localIcueCommand("icue_automation", {
                        control: automationLoader.row.automationId,
                        option: chosen
                    });
                    // Restore the binding after ComboBox changes currentIndex itself.
                    // Coalescing may skip intermediate telemetry; show confirmed state.
                    currentIndex = Qt.binding(function () {
                        const choices = automationLoader.row.options || [];
                        return choices.findIndex(o => o.id === automationLoader.row.value);
                    });
                }

                ToolTip.visible: hovered
                ToolTip.text: automationLoader.row.tooltip || ""
            }
        }
    }

    Loader {
        id: lightingLoader

        active: modelData.control === "lightingChoice"
        visible: active
        property var row: modelData

        sourceComponent: Component {
            RowLayout {
                BrowserChoice {
                    objectName: "lightingChoice_" + lightingLoader.row.lightingParameter
                    enabled: lightingLoader.row.available === true
                    model: lightingLoader.row.options || []
                    textRole: "name"
                    valueRole: "id"
                    implicitWidth: 190
                    currentIndex: (lightingLoader.row.options || []).findIndex(o => o.id === lightingLoader.row.value)
                    displayText: currentIndex >= 0 ? currentText : "No override"

                    onActivated: {
                        if (lightingLoader.row.lightingParameter === "effect")
                            controller.localIcueCommand("icue_lighting", {
                                effect: currentValue
                            });
                        else {
                            var parameters = {};
                            parameters[lightingLoader.row.lightingParameter] = lightingLoader.row.lightingParameter === "RandomColor"
                                ? currentValue === "1"
                                : Number(currentValue);
                            controller.localIcueCommand("icue_lighting", {
                                expected_effect: lightingLoader.row.lightingEffect,
                                parameters: parameters
                            });
                        }
                    }
                }

                BrowserButton {
                    text: "Stop Override"
                    visible: lightingLoader.row.lightingParameter === "effect"
                    enabled: lightingLoader.row.available
                    onClicked: controller.localIcueCommand("icue_lighting", {
                        stop: true
                    })
                    ToolTip.visible: hovered
                    ToolTip.text: "Stop the global Lighting Link override; let the iCUE profile control lighting"
                }
            }
        }
    }

    Loader {
        active: modelData.control === "rgb"
        visible: active
        width: devices.width

        sourceComponent: BrowserColour {
            controller: devices.controller
            row: devices.modelData
            onColourSubmitted: (id, colour) => devices.colourSubmitted(id, colour)
        }
    }

    BrowserButton {
        visible: modelData.control === "release"
        enabled: modelData.available === true
        text: "Clear Lighting Override"
        implicitHeight: 32
        ToolTip.visible: hovered
        ToolTip.text: "Remove WHAT's colour overlay so this device's normal iCUE lighting shows again"
        onClicked: controller.localIcueCommand("icue_release", {
            device: modelData.icueDevice
        })
    }
}
