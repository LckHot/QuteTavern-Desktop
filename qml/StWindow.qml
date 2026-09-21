// SPDX-License-Identifier: AGPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtWebEngine

// The SillyTavern front end window (Qt WebEngine / Chromium).
//
// Fullscreen contract (DESIGN section 2):
//  - the page's Fullscreen API is enabled; every request is accepted, so the
//    requesting element fills the view (that is all Qt WebEngine does by itself)
//  - entering fullscreen makes *this* window fullscreen, leaving restores the
//    visibility and the size it had before - like a browser
//  - Escape leaves fullscreen, but only while the page actually holds a
//    fullscreen element (otherwise Escape keeps reaching the page/input method)
//  - when the window system pulls the window out of fullscreen (compositor
//    shortcut), the page is told through the official fullScreenCancelled() API
//  - a fullscreen geometry is never written back to Settings
//
// Why the size handling is explicit: on Wayland the client cannot place its own
// window and the "normal size" a window is restored to is partly remembered by
// the platform. A window that was never shown at a normal size (start maximized
// -> fullscreen) would be restored to its default (640x480) instead of the size
// the user had. Seeding the size before maximizing and writing the pre-fullscreen
// size at the moment fullscreen is entered (below) keeps a known-good value.
Window {
    id: stWindow

    title: qsTr("SillyTavern")

    // Set by Main.qml: the window is destroyed when it closes
    signal closed()

    // The page currently holds a fullscreen element
    property bool pageFullScreen: false
    // Visibility the window returns to when the page leaves fullscreen
    property int baseVisibility: Window.Windowed

    Component.onCompleted: {
        // Seed the seeded normal size *before* any maximize/fullscreen: this is
        // what keeps a "you decide" restore from landing on a default size.
        const g = App.stNormalGeometry
        if (g.w > 0 && g.h > 0) {
            width = g.w
            height = g.h
        }
        baseVisibility = App.stAutoMaximize ? Window.Maximized : Window.Windowed
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
            // Accept both directions: the page cannot reject its own exit, and
            // accepting keeps the page's fullscreenElement in sync.
            request.accept()
            if (request.toggleOn)
                stWindow.enterPageFullScreen()
            else
                stWindow.leavePageFullScreen()
        }
    }

    // A browser consumes Escape while a page is fullscreen. The shortcut is
    // scoped to this window and armed only while the page holds a fullscreen
    // element, so it cannot swallow Escape from inputs or the input method.
    Shortcut {
        sequence: "Escape"
        enabled: stWindow.pageFullScreen
        onActivated: view.triggerWebAction(WebEngineView.ExitFullScreen)
    }

    // ---------- the only place window state is changed ----------
    function enterPageFullScreen() {
        if (pageFullScreen)
            return
        pageFullScreen = true
        if (visibility !== Window.FullScreen && visibility !== Window.Minimized) {
            baseVisibility = visibility === Window.Maximized ? Window.Maximized : Window.Windowed
            // Remember the size to come back to - here, at the last moment where
            // it is known to be a normal (not fullscreen) size.
            if (baseVisibility === Window.Windowed)
                App.saveStWindowGeometry(x, y, width, height, false, false)
        }
        visibility = Window.FullScreen
    }

    function leavePageFullScreen() {
        if (!pageFullScreen)
            return
        pageFullScreen = false
        visibility = baseVisibility
        if (baseVisibility === Window.Windowed)
            restoreNormalSize()
        // Hand focus and the input method back to the page: the Wayland text
        // input object is re-armed on focus changes, not on window state changes.
        view.forceActiveFocus()
        App.resyncInputMethod()
    }

    function restoreNormalSize() {
        const g = App.stNormalGeometry
        if (g.w > 0 && g.h > 0) {
            width = g.w
            height = g.h
        }
    }

    // The window manager can pull the window out of fullscreen behind the page's
    // back (e.g. a compositor shortcut). Chromium would keep believing it is in
    // fullscreen, so tell it - no state guessing on our side.
    onVisibilityChanged: function (vis) {
        if (pageFullScreen && vis !== Window.FullScreen && vis !== Window.Minimized) {
            pageFullScreen = false
            baseVisibility = vis === Window.Maximized ? Window.Maximized : Window.Windowed
            view.fullScreenCancelled()
            view.forceActiveFocus()
            App.resyncInputMethod()
        } else if (!pageFullScreen && (vis === Window.Windowed || vis === Window.Maximized)) {
            baseVisibility = vis
        }
    }

    // "Open ST window" while it already exists: bring it to the front without
    // forcing a fullscreen window back to normal.
    Connections {
        target: App
        function onRaiseStWindow() {
            if (stWindow.visibility === Window.Minimized)
                stWindow.visibility = stWindow.pageFullScreen ? Window.FullScreen : stWindow.baseVisibility
            stWindow.raise()
            stWindow.requestActivate()
        }
    }

    function saveGeometry() {
        App.saveStWindowGeometry(x, y, width, height, visibility === Window.Maximized,
                                 pageFullScreen)
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
