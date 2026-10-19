import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: banner

    required property string title
    property string detail: ""
    property bool critical: false
    property bool informational: false
    property bool compact: false
    readonly property int verticalPadding: compact ? Theme.spaceS : 14
    property string actionText: ""
    property string actionHelp: ""
    property bool inlineAction: false
    default property alias contents: bannerCopy.data
    readonly property color ink: critical ? Theme.error : informational ? Theme.accent : Theme.warning
    readonly property bool hasInlineAction: inlineAction && actionText.length > 0
    signal actionTriggered

    function htmlText(text) {
        return text.replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;");
    }

    Layout.fillWidth: true
    Layout.minimumWidth: 0
    implicitHeight: bannerCopy.implicitHeight + verticalPadding * 2
    color: critical ? Theme.errorSurface : informational ? Theme.surface : Theme.warningSurface
    border.color: critical ? Theme.errorOutline : informational ? Theme.outline : Theme.warningOutline

    data: [
        Rectangle {
            width: 3
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 10
            color: banner.ink
        },

        ColumnLayout {
            id: bannerCopy

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 24
            anchors.rightMargin: 24 + (actionButton.visible ? actionButton.width : 0)
            anchors.topMargin: banner.verticalPadding
            spacing: 5

            Label {
                text: banner.title
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: banner.ink
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            Label {
                id: detailLabel

                text: banner.hasInlineAction
                    ? banner.htmlText(banner.detail) + " <a href='action'>" + banner.htmlText(banner.actionText) + "</a>"
                    : banner.detail
                textFormat: banner.hasInlineAction ? Text.StyledText : Text.PlainText
                visible: text.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: banner.ink
                linkColor: activeFocus ? Theme.ink : banner.ink
                font.pixelSize: Theme.fontCaption
                lineHeight: 1.2
                activeFocusOnTab: banner.hasInlineAction
                Accessible.role: banner.hasInlineAction ? Accessible.Link : Accessible.StaticText
                Accessible.name: banner.detail + (banner.hasInlineAction ? " " + banner.actionText : "")
                Accessible.description: banner.hasInlineAction ? banner.actionHelp : ""
                onLinkActivated: banner.actionTriggered()
                Keys.onReturnPressed: if (banner.hasInlineAction) banner.actionTriggered()
                Keys.onEnterPressed: if (banner.hasInlineAction) banner.actionTriggered()
                ToolTip.text: banner.actionHelp
                ToolTip.visible: (activeFocus || hoveredLink.length > 0) && banner.actionHelp.length > 0
                ToolTip.delay: Theme.toolTipDelay
                ToolTip.timeout: Theme.toolTipTimeout

                HoverHandler {
                    cursorShape: detailLabel.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                }
            }
        },

        QuietButton {
            id: actionButton

            visible: banner.actionText.length > 0 && !banner.inlineAction
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 8
            width: Math.max(32, implicitWidth)
            height: 32
            text: banner.actionText
            Accessible.name: banner.actionHelp || banner.actionText
            ToolTip.text: banner.actionHelp
            onClicked: banner.actionTriggered()
        }
    ]
}
