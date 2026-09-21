// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import QuteTavern

// Preferences: installation directory (+ validation), extra backend arguments
// and the ST window behaviour. Saving validates exactly like the widget version.
Dialog {
    id: dialog

    title: qsTr("Preferences")
    modal: true
    width: Theme.dialogWidth
    height: Math.min(implicitHeight, parent ? parent.height - 2 * Theme.dialogMargin : implicitHeight)
    anchors.centerIn: parent

    // Background from the application palette's "window" role instead of the QML
    // style theme (see AppController::windowColor).
    background: Rectangle { color: App.windowColor }

    property var check: ({
            "ok": false,
            "error": "",
            "version": ""
        })
    property string saveError: ""

    onOpened: {
        rootField.text = App.stRoot
        argsField.text = App.extraBackendArgs
        maxCheck.checked = App.stAutoMaximize
        saveError = ""
        validateTimer.restart()
    }

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            text: qsTr("SillyTavern installation directory (restart the backend to apply)")
            color: Theme.dim
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: rootField
                Layout.fillWidth: true
                onTextChanged: validateTimer.restart()
            }
            Button {
                text: qsTr("Browse...")
                onClicked: folderDialog.open()
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: dialog.check.ok ? Theme.ok : Theme.err
            text: {
                if (dialog.check.ok)
                    return qsTr("✓ SillyTavern v%1").arg(dialog.check.version)
                return dialog.check.error.length > 0 ? "✕ " + dialog.check.error : ""
            }
        }

        Label {
            text: qsTr("Extra backend arguments (space separated; --global and friends are "
                       + "added by the launcher)")
            color: Theme.dim
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        TextField {
            id: argsField
            Layout.fillWidth: true
        }

        CheckBox {
            id: maxCheck
            text: qsTr("Maximize the ST window when it opens")
        }

        Label {
            Layout.fillWidth: true
            visible: dialog.saveError.length > 0
            text: dialog.saveError
            color: Theme.err
            wrapMode: Text.WordWrap
        }
    }

    footer: Item {
        implicitHeight: footerRow.implicitHeight + 2 * Theme.dialogMargin
        RowLayout {
            id: footerRow
            anchors.fill: parent
            anchors.margins: Theme.dialogMargin
            spacing: 8

            Item {
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            Button {
                text: qsTr("Save")
                enabled: dialog.check.ok
                onClicked: {
                    const error = App.saveSettings(rootField.text, argsField.text,
                                                   maxCheck.checked)
                    if (error.length > 0) {
                        dialog.saveError = error
                        return
                    }
                    dialog.close()
                }
            }
        }
    }

    Timer {
        id: validateTimer
        interval: 300
        onTriggered: dialog.check = App.validateRoot(rootField.text)
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Select the SillyTavern installation directory")
        currentFolder: "file://" + App.homePath
        onAccepted: rootField.text = App.fromUrl(selectedFolder)
    }
}
