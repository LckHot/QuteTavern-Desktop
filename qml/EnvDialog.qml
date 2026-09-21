// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QuteTavern

// Startup environment check (node/git). The download itself happens in
// EnvCheck (C++); this dialog only mirrors its state. Closing the dialog
// without finishing terminates the application - same contract as before.
Dialog {
    id: dialog

    title: qsTr("Environment check")
    modal: true
    width: Theme.dialogWidth
    height: Math.min(implicitHeight, parent ? parent.height - 2 * Theme.dialogMargin : implicitHeight)
    anchors.centerIn: parent

    // Background from the application palette's "window" role instead of the QML
    // style theme (see AppController::windowColor).
    background: Rectangle { color: App.windowColor }
    visible: App.env.visible

    onClosed: if (App.env.visible && !App.env.done) App.env.quit()

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: App.env.body
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: text.length > 0
            text: App.env.status
        }
        ProgressBar {
            Layout.fillWidth: true
            visible: App.env.busy
            from: 0
            to: 100
            indeterminate: App.env.progress < 0
            value: Math.max(0, App.env.progress)
        }
    }

    footer: Item {
        implicitHeight: footerRow.implicitHeight + 2 * Theme.dialogMargin
        RowLayout {
            id: footerRow
            anchors.fill: parent
            anchors.margins: Theme.dialogMargin
            spacing: 8

            Button {
                text: App.env.exitLabel
                onClicked: App.env.quit()
            }
            Item {
                Layout.fillWidth: true
            }
            Button {
                text: App.env.downloadLabel
                enabled: App.env.canDownload && !App.env.busy
                onClicked: App.env.done ? App.env.accept() : App.env.download()
            }
        }
    }
}
