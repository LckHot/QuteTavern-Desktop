// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QuteTavern

// A SillyTavern instance that the launcher did not start is occupying the port:
// take over (recommended), attach without a log, or cancel the start.
Dialog {
    id: dialog

    title: qsTr("Existing instance detected")
    modal: true
    width: Theme.dialogWidth
    height: Math.min(implicitHeight, parent ? parent.height - 2 * Theme.dialogMargin : implicitHeight)
    anchors.centerIn: parent

    // Background from the application palette's "window" role instead of the QML
    // style theme (see AppController::windowColor).
    background: Rectangle { color: App.windowColor }
    visible: App.foreignVisible

    contentItem: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: App.foreignText
    }

    footer: Item {
        implicitHeight: footerRow.implicitHeight + 2 * Theme.dialogMargin
        RowLayout {
            id: footerRow
            anchors.fill: parent
            anchors.margins: Theme.dialogMargin
            spacing: 8

            Button {
                text: qsTr("Terminate and take over (recommended)")
                onClicked: App.foreignTakeover()
            }
            Button {
                text: qsTr("Connect directly (no log)")
                onClicked: App.foreignAttach()
            }
            Item {
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("Cancel")
                onClicked: App.foreignCancel()
            }
        }
    }
}
