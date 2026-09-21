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
    width: 560
    anchors.centerIn: parent
    visible: App.foreignVisible

    contentItem: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: App.foreignText
    }

    footer: RowLayout {
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
