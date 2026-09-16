// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <QUrl>

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

private:
    Settings *m_settings;
};