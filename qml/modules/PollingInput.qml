import QtQuick
import QtQuick.Controls.Basic
import "../controls"

MillisInput {
    id: polling

    ToolTip.text: "Time between collection attempts, in milliseconds. Lower values collect more often and may use more CPU. Changes save automatically."
    required property var controller
    required property string moduleKey
    objectName: moduleKey + "Polling"
    property int pendingValue: value
    readonly property int storedValue: moduleKey === "coretemp"
        ? controller.info.coretempPollMs
        : moduleKey === "nvidia"
        ? controller.info.nvidiaPollMs
        : moduleKey === "hwinfo"
        ? controller.info.hwinfoCheckMs
        : controller.info.sampleSeconds * 1000
    Component.onCompleted: value = storedValue
    onStoredValueChanged: if (!activeFocus && !pollingSave.running)
        value = storedValue

    onValueModified: {
        pendingValue = value;
        pollingSave.restart();
    }

    Timer {
        id: pollingSave

        interval: 500
        onTriggered: controller.setPollingInterval(polling.moduleKey, polling.pendingValue)
    }
}
