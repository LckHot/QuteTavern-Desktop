// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QVariantMap>

#include <functional>
#include <memory>

#include "Backend.h"

class EnvCheck;
struct Settings;

// Everything the QML UI (qml/Main.qml and friends) can see or trigger.
//
// This class replaces the widget-based MainWindow: it owns the settings, the
// backend and the ST window lifecycle, and exposes them as QML properties and
// invokables. No QWidget is involved anywhere; the UI layer is QML, the logic
// layer (Backend/Installer/Updater/Util/Settings) is unchanged C++.
//
// Window contracts (unchanged from the widget version, DESIGN section 2):
//  - closing the management window stops the backend and quits (requestCloseWindow)
//  - closing the ST window leaves the backend running (it can be reopened)
//  - "backend running <=> ST window exists" (unless the user closed it)
class AppController : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    Q_PROPERTY(QString homePath READ homePath CONSTANT)
    // EnvCheck runs at startup: announces itself through its own `visible`.
    Q_PROPERTY(QObject *env READ env CONSTANT)

    Q_PROPERTY(bool wizardVisible READ wizardVisible NOTIFY changed)
    Q_PROPERTY(bool bound READ bound NOTIFY changed)
    Q_PROPERTY(QString stRoot READ stRoot NOTIFY changed)
    Q_PROPERTY(QString extraBackendArgs READ extraBackendArgs NOTIFY changed)
    Q_PROPERTY(QString versionText READ versionText NOTIFY changed)
    Q_PROPERTY(QString rootText READ rootText NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString statusColor READ statusColor NOTIFY changed)
    Q_PROPERTY(QString urlText READ urlText NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(bool needsInstallDeps READ needsInstallDeps NOTIFY changed)
    Q_PROPERTY(bool canStart READ canStart NOTIFY changed)
    Q_PROPERTY(bool canStop READ canStop NOTIFY changed)
    Q_PROPERTY(bool canOpen READ canOpen NOTIFY changed)
    Q_PROPERTY(QString logText READ logText NOTIFY changed)

    // ST window (instantiated by Main.qml from open/closeStWindowRequested)
    Q_PROPERTY(bool stWindowVisible READ stWindowVisible NOTIFY changed)
    Q_PROPERTY(QString stUrl READ stUrl NOTIFY changed)
    Q_PROPERTY(bool stAutoMaximize READ stAutoMaximize NOTIFY changed)
    Q_PROPERTY(bool stRememberWindowState READ stRememberWindowState NOTIFY changed)
    Q_PROPERTY(QVariantMap stNormalGeometry READ stNormalGeometry NOTIFY changed)

    // Foreign instance dialog
    Q_PROPERTY(bool foreignVisible READ foreignVisible NOTIFY changed)
    Q_PROPERTY(QString foreignText READ foreignText NOTIFY changed)

    // Update dialog
    Q_PROPERTY(QString updateStatus READ updateStatus NOTIFY changed)
    Q_PROPERTY(bool updateRunning READ updateRunning NOTIFY changed)
    Q_PROPERTY(bool updateCanUpdate READ updateCanUpdate NOTIFY changed)

    // Install dialog
    Q_PROPERTY(QString installPhase READ installPhase NOTIFY changed)
    Q_PROPERTY(bool installRunning READ installRunning NOTIFY changed)

    // Generic information dialog (install result, binding errors, ...)
    Q_PROPERTY(bool messageVisible READ messageVisible NOTIFY changed)
    Q_PROPERTY(QString messageTitle READ messageTitle NOTIFY changed)
    Q_PROPERTY(QString messageText READ messageText NOTIFY changed)
    Q_PROPERTY(bool messageWarning READ messageWarning NOTIFY changed)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    QString appVersion() const;
    QString homePath() const;
    QObject *env() const { return reinterpret_cast<QObject *>(m_env); }

    bool wizardVisible() const { return !m_bound; }
    bool bound() const { return m_bound; }
    QString stRoot() const;
    QString extraBackendArgs() const;
    QString versionText() const { return m_versionText; }
    QString rootText() const { return m_rootText; }
    QString statusText() const { return m_statusText; }
    QString statusColor() const { return m_statusColor; }
    QString urlText() const { return m_urlText; }
    QString errorText() const { return m_errorText; }
    bool needsInstallDeps() const { return m_needsInstallDeps; }
    bool canStart() const { return m_canStart; }
    bool canStop() const { return m_canStop; }
    bool canOpen() const { return m_canOpen; }
    QString logText() const { return m_logText; }

    bool stWindowVisible() const;
    QString stUrl() const;
    bool stAutoMaximize() const;
    bool stRememberWindowState() const;
    QVariantMap stNormalGeometry() const;

    bool foreignVisible() const { return m_foreignVisible; }
    QString foreignText() const { return m_foreignText; }

    QString updateStatus() const { return m_updateStatus; }
    bool updateRunning() const { return m_updateRunning; }
    bool updateCanUpdate() const { return m_updateCanUpdate; }

    QString installPhase() const { return m_installPhase; }
    bool installRunning() const { return m_installRunning; }

    bool messageVisible() const { return m_messageVisible; }
    QString messageTitle() const { return m_messageTitle; }
    QString messageText() const { return m_messageText; }
    bool messageWarning() const { return m_messageWarning; }

    // Wizard / settings
    Q_INVOKABLE QVariantMap validateRoot(const QString &path) const;
    Q_INVOKABLE QString bindRoot(const QString &path); // empty = success
    Q_INVOKABLE QString saveSettings(const QString &root, const QString &extraArgs,
                                     bool autoMaximize, bool rememberWindowState);
    // QML dialogs hand over file:// URLs; the settings and the backend want paths
    Q_INVOKABLE QString fromUrl(const QUrl &url) const;
    Q_INVOKABLE void showMessage(const QString &title, const QString &text, bool warning);
    Q_INVOKABLE void dismissMessage();

    // Backend control
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void installDepsThenStart();
    Q_INVOKABLE void openStWindow();
    Q_INVOKABLE void onStWindowClosed();
    Q_INVOKABLE void saveStWindowGeometry(int x, int y, int w, int h, bool maximized,
                                          bool fullScreen);
    Q_INVOKABLE void resyncInputMethod();
    Q_INVOKABLE void openDataDir();

    // Update / install
    Q_INVOKABLE void checkUpdates();
    Q_INVOKABLE void performUpdate();
    Q_INVOKABLE void installNew(const QString &parentDir);

    // Quit contract: false = the stop is running, quitReady() follows
    Q_INVOKABLE bool requestCloseWindow();

    // Foreign instance decision
    Q_INVOKABLE void foreignTakeover();
    Q_INVOKABLE void foreignAttach();
    Q_INVOKABLE void foreignCancel();

signals:
    void changed();
    void openStWindowRequested();  // create the ST window (it does not exist yet)
    void closeStWindowRequested(); // close the ST window (backend left Running)
    void raiseStWindow();          // "Open ST window" while it already exists
    void quitReady();              // the management window may close now

private:
    QThread *startWorker(const std::function<void()> &body);
    void waitForWorkers(int timeoutMs);
    void refreshAll();
    void onLog(const QString &line);
    void onStateChanged();
    void onForeignInstance(int port);

    std::unique_ptr<Settings> m_settings;
    Backend *m_backend;
    EnvCheck *m_env;
    Backend::Status m_prevStatus = Backend::Status::Stopped;

    QList<QPointer<QThread>> m_workers; // running worker threads (install/update)
    bool m_shutdownReady = false;       // close intercepted, waiting for the backend to stop
    QMetaObject::Connection m_shutdownConn;

    bool m_bound = false;
    QString m_versionText;
    QString m_rootText;
    QString m_statusText;
    QString m_statusColor;
    QString m_urlText;
    QString m_errorText;
    bool m_needsInstallDeps = false;
    bool m_canStart = false;
    bool m_canStop = false;
    bool m_canOpen = false;
    QString m_logText;

    bool m_stWindowVisible = false;

    bool m_foreignVisible = false;
    QString m_foreignText;
    int m_foreignPort = 0;

    QString m_updateStatus;
    QString m_updateTag; // newest tag found by the update check
    bool m_updateRunning = false;
    bool m_updateCanUpdate = false;

    QString m_installPhase = QStringLiteral("Waiting to start...");
    bool m_installRunning = false;

    bool m_messageVisible = false;
    QString m_messageTitle;
    QString m_messageText;
    bool m_messageWarning = false;
};
