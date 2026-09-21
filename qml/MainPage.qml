// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QuteTavern

// Control centre: start/stop the backend, open the ST window, live log.
Item {
    id: page

    signal preferencesRequested()
    signal updateRequested()
    signal installRequested()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        // ---------- header ----------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ColumnLayout {
                spacing: 2
                Label {
                    text: qsTr("QuteTavern")
                    font.pointSize: 15
                    font.bold: true
                }
                Label {
                    text: App.versionText
                    color: Theme.dim
                }
            }

            Item {
                Layout.fillWidth: true
            }

            ColumnLayout {
                spacing: 2
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: 6
                    Label {
                        text: "●"
                        color: App.statusColor
                    }
                    Label {
                        text: App.statusText
                    }
                }
                Label {
                    Layout.alignment: Qt.AlignRight
                    text: App.urlText
                    color: Theme.dim
                    font.family: Theme.mono
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: App.rootText
            color: Theme.dim
            elide: Text.ElideMiddle
        }

        Label {
            Layout.fillWidth: true
            visible: App.errorText.length > 0
            text: App.errorText
            color: Theme.err
            wrapMode: Text.WordWrap
        }

        // ---------- primary actions ----------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                text: qsTr("▶  Start SillyTavern")
                enabled: App.canStart
                Layout.fillWidth: true
                onClicked: App.start()
            }
            Button {
                text: qsTr("Open ST window")
                enabled: App.canOpen
                Layout.fillWidth: true
                onClicked: App.openStWindow()
            }
            Button {
                text: qsTr("■  Stop backend")
                enabled: App.canStop
                Layout.fillWidth: true
                onClicked: App.stop()
            }
        }

        // ---------- secondary actions ----------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                text: qsTr("Check for updates")
                Layout.fillWidth: true
                onClicked: page.updateRequested()
            }
            Button {
                text: qsTr("Data directory")
                Layout.fillWidth: true
                onClicked: App.openDataDir()
            }
            Button {
                text: qsTr("Preferences")
                Layout.fillWidth: true
                onClicked: page.preferencesRequested()
            }
            Button {
                text: qsTr("Install a new copy...")
                Layout.fillWidth: true
                onClicked: page.installRequested()
            }
        }

        Button {
            Layout.fillWidth: true
            visible: App.needsInstallDeps
            text: qsTr("Install dependencies and retry")
            onClicked: App.installDepsThenStart()
        }

        Label {
            text: qsTr("Backend output (live)")
            color: Theme.dim
        }

        // ---------- live log ----------
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: logArea
                readOnly: true
                text: App.logText
                font.family: Theme.mono
                font.pointSize: 9
                wrapMode: TextArea.NoWrap
                // follow the tail of the log
                onTextChanged: if (length > 0) cursorPosition = length
            }
        }
    }
}
