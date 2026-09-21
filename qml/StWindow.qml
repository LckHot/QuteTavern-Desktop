// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtWebEngine

// The SillyTavern front end window (Qt WebEngine / Chromium).
//
// Fullscreen contract (DESIGN section 2): "fullscreen" means the page fills this
// window's content area. The window itself is never resized and never switched
// into a platform fullscreen state.
//
// Why: on Wayland the window system - not the application - decides a window's
// size and position. The fullscreen/maximized/normal flip was what left the
// window restored to a stale default size and the input method without a focus
// target (the Wayland text input object re-arms on focus changes only). None of
// that is needed: Qt WebEngine makes the requesting element fill the view as soon
// as the request is accepted, and the view *is* this window's content area.
//
// - every page request is accepted; nothing else happens to the window
// - Escape exits the page fullscreen - armed exactly while the page holds a
//   fullscreen element (WebEngineView.isFullScreen is the single source of truth)
// - a borderless real fullscreen stays available from the window system
//   (compositor shortcut); the page does not care and nothing here fights it
// - only maximized geometries are kept out of Settings, and the window is seeded
//   with its remembered normal size before it is first maximized, so
//   un-maximizing never lands on the platform's default 640x480
Window {
    id: stWindow

    title: qsTr("SillyTavern")

    // Set by Main.qml: the window is destroyed when it closes
    signal closed()

    Component.onCompleted: {
        // Seed the remembered normal size before any maximize: a window that was
        // never shown at a normal size is otherwise restored to a default one.
        const g = App.stNormalGeometry
        if (g.w > 0 && g.h > 0) {
            width = g.w
            height = g.h
        }
        if (App.stAutoMaximize)
            visibility = Window.Maximized
        view.url = App.stUrl
        view.forceActiveFocus()
    }

    WebEngineView {
        id: view
        anchors.fill: parent

        // Qt WebEngine keeps the Fullscreen API switched off for embedders and
        // expects the host to answer every request explicitly.
        settings.fullScreenSupportEnabled: true

        onFullScreenRequested: function (request) {
            // Accepting is all that is needed: the element fills the view, which
            // is this window's content area. The window stays exactly as it is.
            request.accept()
            if (!request.toggleOn) {
                // Leaving the page's fullscreen: keep the keyboard and the input
                // method on the page.
                view.forceActiveFocus()
                App.resyncInputMethod()
            }
        }
    }

    // A browser consumes Escape while a page is fullscreen. The shortcut only
    // exists while the page actually holds a fullscreen element, so Escape keeps
    // reaching inputs and the input method the rest of the time.
    Shortcut {
        sequence: "Escape"
        enabled: view.isFullScreen
        onActivated: view.triggerWebAction(WebEngineView.ExitFullScreen)
    }

    // "Open ST window" while it already exists: bring it back to the front
    // without changing anything else about it.
    Connections {
        target: App
        function onRaiseStWindow() {
            if (stWindow.visibility === Window.Minimized)
                stWindow.visibility = App.stAutoMaximize ? Window.Maximized : Window.Windowed
            stWindow.raise()
            stWindow.requestActivate()
        }
    }

    function saveGeometry() {
        App.saveStWindowGeometry(x, y, width, height, visibility === Window.Maximized)
    }

    onClosing: function (close) {
        saveGeometry()
        close.accepted = true
        stWindow.closed()
        App.onStWindowClosed()
    }

    // The window is also destroyed when the backend leaves Running
    Component.onDestruction: saveGeometry()
}
