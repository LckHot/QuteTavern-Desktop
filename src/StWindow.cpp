// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StWindow.h"

#include "Settings.h"

#include <QAction>
#include <QCloseEvent>
#include <QEvent>
#include <QPoint>
#include <QSize>
#include <QWebEngineFullScreenRequest>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>

StWindow::StWindow(const QUrl &url, Settings *settings, QWidget *parent)
    : QMainWindow(parent)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("SillyTavern"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/icon.png")));

    m_view = new QWebEngineView(this);
    setCentralWidget(m_view);

    // Qt WebEngine keeps the Fullscreen API switched off for embedders and
    // expects the application to answer every request explicitly. Character
    // card front ends call element.requestFullscreen(); without both steps
    // Chromium rejects it with "Fullscreen is not supported".
    m_view->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);

    // Browsers consume Escape while a page is fullscreen; Qt WebEngine leaves
    // that to the application. The shortcut is armed only while the page
    // actually holds a fullscreen element.
    m_exitFullScreenAction = new QAction(this);
    m_exitFullScreenAction->setShortcut(Qt::Key_Escape);
    m_exitFullScreenAction->setShortcutContext(Qt::WindowShortcut);
    m_exitFullScreenAction->setEnabled(false);
    connect(m_exitFullScreenAction, &QAction::triggered, this,
            [this] { m_view->page()->triggerAction(QWebEnginePage::ExitFullScreen); });
    addAction(m_exitFullScreenAction);

    connect(m_view->page(), &QWebEnginePage::fullScreenRequested, this,
            [this](QWebEngineFullScreenRequest request) {
                // Accept both directions: the page cannot reject its own exit,
                // and accepting keeps the page's fullscreenElement in sync.
                request.accept();
                if (m_closing)
                    return;
                if (request.toggleOn()) {
                    // A second request while the window is already fullscreen
                    // (another element) must not overwrite the restore state.
                    if (!isFullScreen()) {
                        m_stateBeforeFullScreen =
                            windowState() & (Qt::WindowMaximized | Qt::WindowMinimized);
                        showFullScreen();
                    }
                    m_pageFullScreen = true;
                    m_exitFullScreenAction->setEnabled(true);
                } else {
                    // Clear the flags first: the state changes below emit
                    // WindowStateChange events that must not trigger changeEvent.
                    m_pageFullScreen = false;
                    m_exitFullScreenAction->setEnabled(false);
                    if (m_stateBeforeFullScreen.testFlag(Qt::WindowMaximized))
                        showMaximized();
                    else
                        showNormal();
                }
            });

    m_view->load(url);

    if (settings->autoMaximize) {
        showMaximized();
    } else {
        if (settings->hasWindowState) {
            resize(settings->windowState.w, settings->windowState.h);
            move(settings->windowState.x, settings->windowState.y);
        } else {
            resize(1280, 850);
        }
        show();
    }
    m_view->setFocus();
}

void StWindow::closeEvent(QCloseEvent *event)
{
    // Requests emitted while Chromium tears the page down must not touch the
    // window again: showNormal()/showMaximized() could resurrect a closing
    // window. A fullscreen geometry must not be written back either, or the
    // next launch opens fullscreen-sized but without fullscreen.
    m_closing = true;
    if (m_settings->rememberWindowState && !isMaximized() && !isFullScreen()) {
        m_settings->windowState = {x(), y(), width(), height()};
        m_settings->hasWindowState = true;
        m_settings->save();
    }
    event->accept();
}

// The window manager can pull the window out of fullscreen behind the page's
// back (e.g. a WM shortcut). Chromium would keep believing it is in fullscreen,
// so forward the exit and let the request handler restore the window.
void StWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange && m_pageFullScreen && !isFullScreen()
        && !isMinimized() && m_view && m_view->page()) {
        m_pageFullScreen = false;
        m_exitFullScreenAction->setEnabled(false);
        m_view->page()->triggerAction(QWebEnginePage::ExitFullScreen);
    }
    QMainWindow::changeEvent(event);
}
