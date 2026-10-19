import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

ModuleCard {
    id: hwinfoSettings

    moduleKey: "hwinfo"
    heading: "HWiNFO"
    signal setupRequested

    HintLabel {
        text: "Recommended: run either HWiNFO or Core Temp to avoid duplicate hardware polling and possible compatibility problems. WHAT reads their shared memory; switching this module off does not stop HWiNFO itself."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    summary: "Hardware temperatures, clocks, load and power"
    keywords: "hwinfo setup hardware temperature cpu gpu sensor shared memory licence license edition pro free polling interval"

    ActionButton {
        objectName: "hwinfoSetupButton"
        text: "HWiNFO Setup"
        ToolTip.text: "Show the steps for enabling HWiNFO shared memory so WHAT can read its sensors."
        onClicked: hwinfoSettings.setupRequested()
    }

    RowLayout {
        Layout.fillWidth: true

        HintLabel {
            text: "Shared-Memory Check Interval"
            Layout.fillWidth: true
        }

        TimingSlider {
            input: hwPolling
            minimum: 200
            maximum: 2000
            label: "Shared-Memory Check Interval"
        }

        PollingInput {
            id: hwPolling

            controller: hwinfoSettings.controller
            moduleKey: "hwinfo"
            from: 100
            to: 10000
            stepSize: 100
        }
    }

    RowLayout {
        HintLabel {
            text: "Your HWiNFO Edition"
            Layout.fillWidth: true
        }

        ChoiceBox {
            ToolTip.text: "Record your HWiNFO licence edition. This controls licence reminders; it does not change the HWiNFO licence."
            hoverEnabled: true
            ToolTip.visible: hovered
            ToolTip.delay: Theme.toolTipDelay
            ToolTip.timeout: Theme.toolTipTimeout
            id: edition

            currentIndex: hwinfoSettings.controller.info.license === "pro"
                ? 2
                : hwinfoSettings.controller.info.license === "free"
                ? 1
                : 0
            model: ["Not sure", "Free", "Pro / paid"]
            implicitWidth: 155
            onActivated: controller.setLicense(["unknown", "free", "pro"][currentIndex])
        }
    }

    HintLabel {
        text: "Shared memory does not reliably identify Free versus Pro. Choosing Pro hides the licence reminders."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    HintLabel {
        text: "HWiNFO Free limits shared memory to 12 hours. Re-enable sharing in HWiNFO when it expires, or use HWiNFO Pro."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }
}
