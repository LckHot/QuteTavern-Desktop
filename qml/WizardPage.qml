// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import QuteTavern

// First run: bind an existing SillyTavern directory (or install a new copy).
Item {
    id: page

    // The install dialog is owned by Main.qml (one instance for both pages)
    signal installRequested()

    // { ok, error, version } - kept in one property so the label stays a binding
    property var check: ({
            "ok": false,
            "error": "",
            "version": ""
        })

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 10

        Label {
            text: qsTr("Welcome to QuteTavern")
            font.pointSize: 18
            font.bold: true
        }
        Label {
            text: qsTr("To get started, bind the SillyTavern installation directory\n"
                       + "(the folder that contains server.js and package.json)")
            color: Theme.dim
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: pathField
                placeholderText: qsTr("/home/you/SillyTavern")
                Layout.fillWidth: true
                onTextChanged: validateTimer.restart()
            }
            Button {
                text: qsTr("Browse...")
                onClicked: folderDialog.open()
            }
        }

        Label {
            id: checkLabel
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: page.check.ok ? Theme.ok : Theme.err
            text: {
                if (page.check.ok)
                    return qsTr("✓ Found SillyTavern v%1").arg(page.check.version)
                return page.check.error.length > 0 ? "✕ " + page.check.error : ""
            }
        }

        Item {
            Layout.fillHeight: true
        }

        Button {
            text: qsTr("Bind and continue")
            enabled: page.check.ok
            Layout.fillWidth: true
            onClicked: {
                const error = App.bindRoot(pathField.text)
                if (error.length > 0)
                    App.showMessage(qsTr("Binding failed"), error, true)
            }
        }
        Button {
            text: qsTr("Install a new copy from GitHub...")
            flat: true
            Layout.fillWidth: true
            onClicked: page.installRequested()
        }
    }

    // Same 300 ms debounce the widget version used before validating
    Timer {
        id: validateTimer
        interval: 300
        onTriggered: page.check = App.validateRoot(pathField.text)
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Select the SillyTavern installation directory")
        currentFolder: "file://" + App.homePath
        onAccepted: pathField.text = App.fromUrl(selectedFolder)
    }
}
