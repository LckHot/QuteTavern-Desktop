// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QuteTavern

// Check for updates / perform the update. The worker runs in C++; this dialog
// only mirrors App.updateStatus and App.updateRunning.
Dialog {
    id: dialog

    title: qsTr("Check for updates")
    modal: true
    width: Theme.dialogWidth
    height: Math.min(implicitHeight, parent ? parent.height - 2 * Theme.dialogMargin : implicitHeight)
    anchors.centerIn: parent

    onOpened: App.checkUpdates()

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: App.updateStatus
        }
        Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            wrapMode: Text.WordWrap
            color: Theme.dim
            text: qsTr("Updating means: git fetch --tags + checkout of the latest tag + "
                       + "npm install.\n"
                       + "Characters and chats live in the global data directory and are not "
                       + "affected. Detailed output is shown in the launcher log.")
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
                text: qsTr("Check again")
                enabled: !App.updateRunning
                onClicked: App.checkUpdates()
            }
            Button {
                text: qsTr("Update")
                enabled: App.updateCanUpdate && !App.updateRunning
                onClicked: App.performUpdate()
            }
            Item {
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("Close")
                onClicked: dialog.close()
            }
        }
    }
}
