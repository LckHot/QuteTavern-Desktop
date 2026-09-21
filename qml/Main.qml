// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QuteTavern

// Management window: wizard page / main page + every dialog. The ST window is
// created lazily by the Loader below, so "backend running <=> ST window exists"
// stays true (unless the user closed it - then it is reopened from the button).
ApplicationWindow {
    id: appWindow

    width: 520
    height: 720
    minimumWidth: 440
    minimumHeight: 480
    title: qsTr("QuteTavern")
    visible: true

    StackLayout {
        anchors.fill: parent
        currentIndex: App.wizardVisible ? 0 : 1

        WizardPage {
            onInstallRequested: installDialog.open()
        }
        MainPage {
            onPreferencesRequested: preferencesDialog.open()
            onUpdateRequested: updateDialog.open()
            onInstallRequested: installDialog.open()
        }
    }

    // ---------- dialogs ----------
    EnvDialog {
        id: envDialog
    }

    PreferencesDialog {
        id: preferencesDialog
    }

    UpdateDialog {
        id: updateDialog
    }

    InstallDialog {
        id: installDialog
    }

    ForeignDialog {
        id: foreignDialog
    }

    MessageDialog {
        id: messageDialog
    }

    // ---------- ST window ----------
    // The ST window is created on demand and destroyed when it closes, so
    // "backend running <=> ST window exists" stays true (unless the user closed
    // it - then it is reopened from the button). It is instantiated from C++
    // driven signals instead of a Loader: a Window is not an Item.
    property var stWindowInstance: null

    Component {
        id: stWindowComponent
        StWindow {
            onClosed: {
                if (appWindow.stWindowInstance) {
                    appWindow.stWindowInstance.destroy()
                    appWindow.stWindowInstance = null
                }
            }
        }
    }

    Connections {
        target: App
        function onOpenStWindowRequested() {
            if (!appWindow.stWindowInstance)
                appWindow.stWindowInstance = stWindowComponent.createObject(null)
        }
        function onCloseStWindowRequested() {
            if (appWindow.stWindowInstance)
                appWindow.stWindowInstance.close()
        }
    }

    // ---------- shortcuts ----------
    Shortcut {
        sequences: [StandardKey.Preferences]
        onActivated: preferencesDialog.open()
    }

    // ---------- window contract ----------
    // Closing the management window stops the backend (SIGTERM -> 5s -> SIGKILL;
    // Windows: kill immediately) and quits. requestCloseWindow() returns false
    // while the stop is running; quitReady() then closes for real.
    onClosing: function (close) {
        if (!App.requestCloseWindow())
            close.accepted = false
    }

    Connections {
        target: App
        function onQuitReady() {
            Qt.quit()
        }
    }
}
