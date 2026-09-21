// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import QuteTavern

// Install a new copy from GitHub: pick a parent directory, then clone/checkout/
// npm install on a worker thread. Progress comes from App.installPhase.
Dialog {
    id: dialog

    title: qsTr("Install SillyTavern from GitHub")
    modal: true
    width: Theme.dialogWidth
    height: Math.min(implicitHeight, parent ? parent.height - 2 * Theme.dialogMargin : implicitHeight)
    anchors.centerIn: parent

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Clones the SillyTavern release branch into the directory you pick, "
                       + "checks out the latest release tag and installs the dependencies.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: parentField
                Layout.fillWidth: true
                text: App.homePath
            }
            Button {
                text: qsTr("Choose directory...")
                onClicked: folderDialog.open()
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            wrapMode: Text.WordWrap
            color: Theme.dim
            text: App.installPhase
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
                text: qsTr("Start installation")
                enabled: !App.installRunning
                onClicked: App.installNew(parentField.text)
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

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose the parent directory for the installation")
        currentFolder: "file://" + App.homePath
        onAccepted: parentField.text = App.fromUrl(selectedFolder)
    }
}
