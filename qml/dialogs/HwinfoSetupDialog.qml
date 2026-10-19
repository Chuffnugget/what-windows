import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: setupDialog

    required property var controller
    property bool showLicenseReminder: true

    objectName: "hwinfoSetupDialog"
    anchors.centerIn: parent
    width: 690
    modal: true
    padding: 24
    title: "Set Up HWiNFO Shared Memory"
    standardButtons: Dialog.Close

    contentItem: ColumnLayout {
        spacing: 18

        Label {
            text: "WHAT is free. No WHAT licence or payment is required."
            font.weight: Font.DemiBold
            color: Theme.accent
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        Step {
            number: "1"
            heading: "Open HWiNFO's startup window → Settings"
            detail: "If HWiNFO skips its startup window, exit HWiNFO using its tray icon, then launch it again to access Settings."
        }

        Step {
            number: "2"
            heading: "General / User Interface → Shared Memory Support"
            detail: "Tick Shared Memory Support, then click OK. This is HWiNFO's general settings, not the Sensors window's sensor-customisation gear."
        }

        Step {
            number: "3"
            heading: "Start Sensor Monitoring"
            detail: "Select Sensors-only and click Start / Run in the startup window, or click Sensors in HWiNFO's main window. Leave HWiNFO running; the Sensors window can be minimised."
        }

        Step {
            number: "4"
            heading: "Return to WHAT → Sensors"
            detail: "Readings appear automatically. Check the sensors you want to share. If another reader already works, consult Diagnostics; a reader error does not mean you must buy a licence."
        }

        WarningBanner {
            visible: setupDialog.showLicenseReminder
                && setupDialog.controller.info.hwinfoEnabled
                && setupDialog.controller.info.license !== "pro"
            title: "HWiNFO's free edition limits sharing to 12 hours"
            detail: "Manually re-enable Shared Memory Support, or use HWiNFO Pro for continuous sharing. This is a limit of HWiNFO's free edition, not WHAT."
        }

        ActionButton {
            visible: setupDialog.showLicenseReminder
                && setupDialog.controller.info.hwinfoEnabled
                && setupDialog.controller.info.license !== "pro"
            text: "Open HWiNFO's Licence Information ↗"
            onClicked: Qt.openUrlExternally("https://www.hwinfo.com/licenses/")
        }
    }

    component Step: RowLayout {
        property string number
        property string heading
        property string detail
        spacing: Theme.spaceM

        Rectangle {
            Layout.alignment: Qt.AlignTop
            width: 27
            height: 27

            color: Theme.selected

            Label {
                anchors.centerIn: parent
                text: number
                color: Theme.accent
                font.bold: true
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceXs

            Label {
                text: heading
                color: Theme.ink
                font.weight: Font.DemiBold
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            HintLabel {
                text: detail
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: 1.25
            }
        }
    }
}
