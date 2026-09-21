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
    width: Theme.dialogWidth
    height: Math.min(implicitHeight, parent ? parent.height - 2 * Theme.dialogMargin : implicitHeight)
    anchors.centerIn: parent
    visible: App.messageVisible

    contentItem: Label {
        wrapMode: Text.WordWrap
        text: App.messageText
        color: App.messageWarning ? Theme.err : Theme.ok
    }

    footer: Item {
        implicitHeight: footerRow.implicitHeight + 2 * Theme.dialogMargin
        RowLayout {
            id: footerRow
            anchors.fill: parent
            anchors.margins: Theme.dialogMargin

            Item {
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("OK")
                onClicked: App.dismissMessage()
            }
        }
    }

    onClosed: if (App.messageVisible) App.dismissMessage()
}
