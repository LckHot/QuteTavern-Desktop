// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QuteTavern

// Generic information dialog (install result, binding errors, ...). Replaces the
// QMessageBox calls of the widget version.
Dialog {
    id: dialog

    title: App.messageTitle
    modal: true
    width: 480
    anchors.centerIn: parent
    visible: App.messageVisible

    contentItem: Label {
        wrapMode: Text.WordWrap
        text: App.messageText
        color: App.messageWarning ? Theme.err : Theme.ok
    }

    footer: RowLayout {
        Item {
            Layout.fillWidth: true
        }
        Button {
            text: qsTr("OK")
            onClicked: App.dismissMessage()
        }
    }

    onClosed: if (App.messageVisible) App.dismissMessage()
}
