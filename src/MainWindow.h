// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QThread>

#include <functional>
#include <memory>

#include "Backend.h"

class QCloseEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class StWindow;
struct Settings;

// Management window: binding wizard / install / start control / live log /
// settings / update check. The ST window lifecycle is driven by the backend
// state (opened when entering Running, closed when leaving Running).
// Closing this window stops the backend gracefully and quits the application
// (see closeEvent).
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onLog(const QString &line);
    void onStateChanged();
    void onForeignInstance(int port);
    void onWizardValidate();

private:
    void buildUi();
    QWidget *buildWizardPage();
    QWidget *buildMainPage();
    void applyState();
    void refreshInfo();
    void bindRoot(const QString &path);
    void openStWindow(const QString &url);
    void closeStWindow();
    void openSettingsDialog();
    void openUpdateDialog();
    void openInstallDialog();
    QThread *startWorker(const std::function<void()> &body);
    void waitForWorkers(int timeoutMs);

    std::unique_ptr<Settings> m_settings;
    Backend *m_backend;
    StWindow *m_stWindow = nullptr;
    Backend::Status m_prevStatus = Backend::Status::Stopped;

    QList<QPointer<QThread>> m_workers; // running worker threads (install/update)
    bool m_shutdownReady = false;       // close intercepted, waiting for the backend to stop
    QMetaObject::Connection m_shutdownConn;
    QMetaObject::Connection m_stWindowConn; // ST window destruction notification

    QStackedWidget *m_stack = nullptr;
    QTimer *m_wizTimer = nullptr;
    // Wizard page
    QLineEdit *m_wizPath = nullptr;
    QLabel *m_wizCheck = nullptr;
    QPushButton *m_wizBind = nullptr;
    // Main page
    QLabel *m_versionLbl = nullptr;
    QLabel *m_rootLbl = nullptr;
    QLabel *m_statusDot = nullptr;
    QLabel *m_statusText = nullptr;
    QLabel *m_urlLbl = nullptr;
    QLabel *m_errorLbl = nullptr;
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_openBtn = nullptr;
    QPushButton *m_stopBtn = nullptr;
    QPushButton *m_installDepsBtn = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QString m_updateTag; // newest tag found by the update check
};