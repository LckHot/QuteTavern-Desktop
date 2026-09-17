// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <QUrl>

class QAction;
class QEvent;
class QWebEngineView;
struct Settings;

// SillyTavern front end window (Qt WebEngine / Chromium).
// Closing this window does not affect the backend; the geometry is written back
// to Settings.
class StWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit StWindow(const QUrl &url, Settings *settings, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    Settings *m_settings;
    QWebEngineView *m_view = nullptr;
    // Escape while the page holds a fullscreen element; disabled otherwise so
    // that Escape keeps reaching the page (inputs, overlays).
    QAction *m_exitFullScreenAction = nullptr;
    // Mirrors whether the page currently has a fullscreen element, i.e. whether
    // the window is expected to be fullscreen because of the page.
    bool m_pageFullScreen = false;
    // Set once the window starts closing: Chromium emits fullscreen requests
    // while it tears the page down, and those must not touch the window again.
    bool m_closing = false;
    // Window state before the page went fullscreen (maximized or normal), so
    // that leaving fullscreen restores it the way a browser would.
    Qt::WindowStates m_stateBeforeFullScreen = Qt::WindowNoState;
};
