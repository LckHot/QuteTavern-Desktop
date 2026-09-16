// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StWindow.h"

#include "Settings.h"

#include <QCloseEvent>
#include <QPoint>
#include <QSize>
#include <QWebEngineView>

StWindow::StWindow(const QUrl &url, Settings *settings, QWidget *parent)
    : QMainWindow(parent)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("SillyTavern"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/icon.png")));

    auto *view = new QWebEngineView(this);
    view->load(url);
    setCentralWidget(view);

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
    view->setFocus();
}

void StWindow::closeEvent(QCloseEvent *event)
{
    if (m_settings->rememberWindowState && !isMaximized()) {
        m_settings->windowState = {x(), y(), width(), height()};
        m_settings->hasWindowState = true;
        m_settings->save();
    }
    event->accept();
}
