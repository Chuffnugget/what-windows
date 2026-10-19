import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../controls"

GlassDialog {
    id: releaseBrowser

    required property var updater
    signal installRequested()

    objectName: "releaseBrowser"
    title: "Choose a WHAT Release"
    modal: true
    anchors.centerIn: parent
    width: Math.min(720, releaseBrowser.parent.width - 40)
    height: Math.min(600, releaseBrowser.parent.height - 60)
    standardButtons: Dialog.Close
    property string selectedVersion: ""
    property string selectedNotes: ""

    function releaseLabel(release) {
        const parts = ["WHAT " + release.version, release.beta ? "Beta" : "Stable", release.date];
        if (release.current)
            parts.push("Installed");
        else if (release.downgrade)
            parts.push("Downgrade");
        return parts.join("  ·  ");
    }

    Connections {
        target: releaseBrowser.updater

        function onChanged() {
            const info = releaseBrowser.updater.info;
            if (!releaseBrowser.opened || releaseBrowser.selectedVersion || info.busy)
                return;
            for (const entry of info.releases || []) {
                if (entry.version === info.available) {
                    releaseBrowser.selectedVersion = entry.version;
                    releaseBrowser.selectedNotes = entry.notes || "No release notes were published.";
                    break;
                }
            }
        }
    }

    onOpened: {
        selectedVersion = "";
        selectedNotes = "";
        releaseBrowser.updater.browseReleases();
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceM

        HintLabel {
            text: releaseBrowser.updater.info.betaUpdates
                ? "Stable and beta releases, newest first"
                : "Stable releases, newest first"
            Layout.fillWidth: true
        }

        ListView {
            objectName: "releaseList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(200, Math.max(88, count * 48))
            clip: true
            spacing: 6
            model: releaseBrowser.updater.info.releases

            ScrollBar.vertical: WideScrollBar {}

            delegate: ItemDelegate {
                required property var modelData
                width: ListView.view.width - 18
                enabled: !releaseBrowser.updater.info.busy
                highlighted: releaseBrowser.selectedVersion === modelData.version
                palette.highlightedText: Theme.ink
                text: releaseBrowser.releaseLabel(modelData)

                onClicked: {
                    releaseBrowser.selectedVersion = modelData.version;
                    releaseBrowser.selectedNotes = modelData.notes || "No release notes were published.";
                    if (!modelData.current)
                        releaseBrowser.updater.selectRelease(modelData.version);
                }
            }
        }

        DragScrollView {
            id: releaseNotesScroll

            objectName: "releaseNotesScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 72
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            rightPadding: 22

            ScrollBar.vertical: WideScrollBar {
                parent: releaseNotesScroll
                x: releaseNotesScroll.width - width
                height: releaseNotesScroll.availableHeight
            }

            clip: true

            TextArea {
                objectName: "releaseNotesText"
                width: releaseNotesScroll.availableWidth
                readOnly: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                text: releaseBrowser.selectedNotes || "Select a version to read its release notes."
                color: Theme.ink

                background: Rectangle {
                    color: Theme.inset
                }
            }
        }

        HintLabel {
            text: releaseBrowser.updater.info.status
            color: releaseBrowser.updater.info.error ? Theme.error : Theme.muted
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        ProgressBar {
            palette.dark: Theme.brand
            palette.midlight: Theme.outline
            visible: releaseBrowser.updater.info.busy
            Layout.fillWidth: true
            indeterminate: true
        }

        HintLabel {
            visible: !releaseBrowser.updater.info.canInstall
            text: releaseBrowser.updater.info.installBlockedReason
            color: Theme.muted
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        ActionButton {
            ToolTip.text: "Review the selected release before downloading and running its installer. An older release replaces the current version."
            text: releaseBrowser.updater.info.downgrade ? "Install older version…" : "Install selected release…"
            primary: true
            enabled: releaseBrowser.updater.info.canInstall && releaseBrowser.selectedVersion === releaseBrowser.updater.info.available

            onClicked: {
                releaseBrowser.close();
                releaseBrowser.installRequested();
            }
        }
    }
}
