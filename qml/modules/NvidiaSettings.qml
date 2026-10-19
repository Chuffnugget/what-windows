import QtQuick
import QtQuick.Layouts
import "../controls"

ModuleCard {
    id: nvidiaSettings

    moduleKey: "nvidia"
    heading: "NVIDIA"
    summary: "GPU readings directly from the NVIDIA driver"
    keywords: "nvidia nvml graphics gpu temperature load utilisation utilization memory vram power fan clocks polling"

    HintLabel {
        text: nvidiaSettings.controller.info.nvidiaStatus || "Disabled"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }

    RowLayout {
        Layout.fillWidth: true

        HintLabel {
            text: "GPU Polling Interval"
            Layout.fillWidth: true
        }

        TimingSlider {
            input: nvPolling
            minimum: 1000
            maximum: 10000
            increment: 500
            label: "GPU Polling Interval"
        }

        PollingInput {
            id: nvPolling

            controller: nvidiaSettings.controller
            moduleKey: "nvidia"
            from: 500
            to: 60000
            stepSize: 500
        }
    }

    HintLabel {
        text: "Read-only NVML API. Requires an NVIDIA driver, not HWiNFO or CUDA. Availability varies by GPU and driver; unsupported readings are not shown as zero. Polling can wake a sleeping laptop GPU."
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }
}
