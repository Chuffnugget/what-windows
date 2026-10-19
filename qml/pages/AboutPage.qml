import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../components"
import "../controls"

DragScrollView {
    id: root

    property bool animationsEnabled: true
    property int logoDuration: 1500
    property int logoPause: 180
    property int hoverDuration: 140
    required property string version
    clip: true
    contentWidth: availableWidth
    rightPadding: Theme.scrollGutter

    ScrollBar.vertical: WideScrollBar {
        id: aboutBar

        parent: root
        x: root.width - width
        y: root.topPadding
        height: root.availableHeight
    }

    component LinkRow: AbstractButton {
        id: card

        property url brandLogo
        required property string heading
        required property string detail
        required property string destination
        Layout.fillWidth: true
        implicitHeight: 64
        Layout.minimumHeight: implicitHeight
        Layout.maximumHeight: implicitHeight
        activeFocusOnTab: true
        hoverEnabled: true
        Accessible.name: heading + ": " + detail
        Accessible.role: Accessible.Link
        onClicked: Qt.openUrlExternally(destination)

        background: Item {
            Rectangle {
                anchors.fill: parent
                anchors.bottomMargin: 1

                color: card.down ? Theme.pressed : card.hovered ? Theme.hover : "transparent"
                border.color: card.visualFocus ? Theme.accent : "transparent"

                Behavior on color {
                    ColorAnimation {
                        duration: root.animationsEnabled ? root.hoverDuration : 0
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.separator
            }
        }

        contentItem: RowLayout {
            spacing: Theme.spaceL

            Image {
                source: card.brandLogo
                visible: source.toString().length > 0
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                Layout.leftMargin: 12
                fillMode: Image.PreserveAspectFit
                smooth: true
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: card.brandLogo.toString().length > 0 ? 0 : 12
                spacing: Theme.spaceXs

                Label {
                    text: card.heading
                    color: Theme.ink
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                }

                Label {
                    text: card.detail
                    color: Theme.muted
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }

            Label {
                text: "↗"
                color: Theme.accent
                font.pixelSize: Theme.fontHeading
                Layout.rightMargin: 16
            }
        }

        ToolTip.visible: hovered
        ToolTip.text: destination
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Theme.spaceXl

        Item {
            Layout.fillWidth: true
            implicitHeight: identity.implicitHeight + 24

            RowLayout {
                id: identity

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                spacing: 20

                AnimatedLogo {
                    objectName: "aboutLogo"
                    Layout.preferredWidth: 64
                    Layout.preferredHeight: 76
                    animationsEnabled: root.animationsEnabled
                    animationDuration: root.logoDuration
                    pauseDuration: root.logoPause
                }

                ColumnLayout {
                    Layout.fillWidth: true

                    Label {
                        objectName: "aboutVersion"
                        text: "WHAT v" + root.version
                        color: Theme.ink
                        font.pixelSize: Theme.fontTitle
                        font.bold: true
                    }

                    Label {
                        textFormat: Text.PlainText
                        text: "Windows Home Assistant Toolkit"
                        color: Theme.accent
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        text: "Developed by Chuffnugget"
                        color: Theme.ink
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Label {
                text: "Support Development"
                color: Theme.ink
                font.pixelSize: 17
                font.weight: Font.DemiBold
                Layout.bottomMargin: 8
            }

            Label {
                text: "If WHAT is useful to you, you can help fund its development. Support is optional; WHAT remains free and open source."
                color: Theme.muted
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            LinkRow {
                brandLogo: "qrc:/assets/brands/buy-me-a-coffee.svg"
                heading: "Buy Me a Coffee"
                detail: "A little thank-you goes a long way."
                destination: "https://buymeacoffee.com/chuffnugget"
            }

            LinkRow {
                brandLogo: "qrc:/assets/brands/paypal.svg"
                heading: "PayPal"
                detail: "Support Chuffnugget directly through PayPal."
                destination: "https://www.paypal.com/paypalme/chuffnugget"
            }

            LinkRow {
                brandLogo: "qrc:/assets/brands/github-sponsors.svg"
                heading: "GitHub Sponsors"
                detail: "Help support ongoing development."
                destination: "https://github.com/sponsors/Chuffnugget"
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Label {
                text: "Source, Issues and Licences"
                color: Theme.ink
                font.pixelSize: 17
                font.weight: Font.DemiBold
                Layout.bottomMargin: 8
            }

            LinkRow {
                heading: "Windows App on GitHub"
                detail: "Source code and documentation for the Windows app."
                destination: "https://github.com/Chuffnugget/what-windows"
            }

            LinkRow {
                heading: "Integration on GitHub"
                detail: "Source code and setup instructions for Home Assistant."
                destination: "https://github.com/Chuffnugget/what-integration"
            }

            LinkRow {
                heading: "Report an App Issue"
                detail: "Bug reports and feature requests for the Windows app."
                destination: "https://github.com/Chuffnugget/what-windows/issues"
            }

            LinkRow {
                heading: "Report an Integration Issue"
                detail: "Bug reports and feature requests for the Home Assistant integration."
                destination: "https://github.com/Chuffnugget/what-integration/issues"
            }

            Label {
                Layout.topMargin: 14
                text: "WHAT is an independent, free and open-source project, licensed under LGPL v3 or later. Built with Qt; third-party notices are included with the source and release files. Windows/Microsoft APIs, HWiNFO, NVIDIA NVML, Corsair iCUE, Wallpaper Engine, Home Assistant, Nabu Casa and Qt are third-party products, services or technologies with their own applicable licences and terms. WHAT and its developer are not affiliated with, sponsored by or endorsed by their respective owners. Names and logos belong to their respective owners. MQTT is an open protocol; MQTT libraries and broker software have their own licences. Third-party product and licence purchases do not go to Chuffnugget."
                color: Theme.muted
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }
    }
}
