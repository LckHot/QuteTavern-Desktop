// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppController.h"

#include "EnvCheck.h"
#include "Installer.h"
#include "Settings.h"
#include "Updater.h"
#include "Util.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_settings(std::make_unique<Settings>(Settings::load()))
    , m_backend(new Backend(this))
    , m_env(new EnvCheck(this))
{
    connect(m_backend, &Backend::logLine, this, &AppController::onLog);
    connect(m_backend, &Backend::stateChanged, this, &AppController::onStateChanged);
    connect(m_backend, &Backend::foreignInstanceFound, this, &AppController::onForeignInstance);
    connect(m_env, &EnvCheck::quitRequested, this, &AppController::quitReady);

    refreshAll();
}

AppController::~AppController()
{
    // Wait for the worker threads to finish (on timeout they are abandoned;
    // every thread callback is QPointer-guarded and never touches destroyed objects)
    waitForWorkers(2000);
    if (m_shutdownConn)
        disconnect(m_shutdownConn);
}

// ---------- properties ----------

QString AppController::appVersion() const
{
    return QCoreApplication::applicationVersion();
}

QString AppController::homePath() const
{
    return QDir::homePath();
}

QString AppController::stRoot() const
{
    return m_settings->stRoot;
}

QString AppController::extraBackendArgs() const
{
    return m_settings->extraBackendArgs.join(QLatin1Char(' '));
}

QString AppController::fromUrl(const QUrl &url) const
{
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

bool AppController::stWindowVisible() const
{
    return m_stWindowVisible;
}

QString AppController::stUrl() const
{
    return m_backend->url();
}

bool AppController::stAutoMaximize() const
{
    return m_settings->autoMaximize;
}

bool AppController::stRememberWindowState() const
{
    return m_settings->rememberWindowState;
}

// The size the ST window must fall back to when it is neither maximized nor
// fullscreen. Seeding it before the window is first maximized/fullscreened is
// what keeps the platform from restoring a "normal size" the window never had
// (a fresh top-level window would remember its 640x480 default).
QVariantMap AppController::stNormalGeometry() const
{
    const WindowGeometry g = m_settings->hasWindowState ? m_settings->windowState
                                                        : WindowGeometry{};
    return QVariantMap{{QStringLiteral("x"), g.x},
                       {QStringLiteral("y"), g.y},
                       {QStringLiteral("w"), g.w},
                       {QStringLiteral("h"), g.h}};
}

// ---------- invokables: wizard / settings ----------

QVariantMap AppController::validateRoot(const QString &path) const
{
    const QString trimmed = path.trimmed();
    QVariantMap out{{QStringLiteral("ok"), false},
                    {QStringLiteral("version"), QString()},
                    {QStringLiteral("error"), QString()}};
    if (trimmed.isEmpty())
        return out;
    const auto check = Util::validateRoot(trimmed);
    out[QStringLiteral("ok")] = check.ok;
    out[QStringLiteral("version")] = check.version;
    out[QStringLiteral("error")] = check.error;
    return out;
}

QString AppController::bindRoot(const QString &path)
{
    const QString trimmed = path.trimmed();
    const auto check = Util::validateRoot(trimmed);
    if (!check.ok)
        return check.error;
    m_settings->stRoot = trimmed;
    m_settings->save();
    m_backend->log(QStringLiteral("Bound installation directory: %1").arg(trimmed));
    refreshAll();
    return QString();
}

QString AppController::saveSettings(const QString &root, const QString &extraArgs,
                                    bool autoMaximize, bool rememberWindowState)
{
    const QString trimmed = root.trimmed();
    const auto check = Util::validateRoot(trimmed);
    if (!check.ok)
        return check.error.isEmpty() ? QStringLiteral("Invalid installation directory") : check.error;
    m_settings->stRoot = trimmed;
    m_settings->extraBackendArgs = extraArgs.split(' ', Qt::SkipEmptyParts);
    m_settings->autoMaximize = autoMaximize;
    m_settings->rememberWindowState = rememberWindowState;
    m_settings->save();
    refreshAll();
    return QString();
}

void AppController::showMessage(const QString &title, const QString &text, bool warning)
{
    m_messageTitle = title;
    m_messageText = text;
    m_messageWarning = warning;
    m_messageVisible = true;
    emit changed();
}

void AppController::dismissMessage()
{
    if (!m_messageVisible)
        return;
    m_messageVisible = false;
    emit changed();
}

// ---------- invokables: backend control ----------

void AppController::start()
{
    m_backend->start();
}

void AppController::stop()
{
    m_backend->stop();
}

void AppController::installDepsThenStart()
{
    m_backend->installDepsThenStart();
}

void AppController::openStWindow()
{
    if (m_stWindowVisible) {
        // A page-fullscreen window must not be forced back to windowed: the page
        // still holds a fullscreen element and the QML window keeps both states
        // in sync, so only bring it back to the front.
        emit raiseStWindow();
        return;
    }
    if (m_backend->url().isEmpty())
        return;
    m_stWindowVisible = true;
    emit openStWindowRequested();
    emit changed();
}

void AppController::onStWindowClosed()
{
    // The user closed the ST window: the backend keeps running and the window
    // can be reopened from the management window.
    if (!m_stWindowVisible)
        return;
    m_stWindowVisible = false;
    emit changed();
}

void AppController::saveStWindowGeometry(int x, int y, int w, int h, bool maximized)
{
    // A maximized geometry is not a useful "normal" size and must not be written
    // back, or the next launch would open maximized-sized but un-maximized.
    // (The window is never put into a platform fullscreen state, so there is no
    // fullscreen geometry to guard against.)
    if (!m_settings->rememberWindowState || maximized)
        return;
    if (w <= 0 || h <= 0)
        return;
    m_settings->windowState = {x, y, w, h};
    m_settings->hasWindowState = true;
    m_settings->save();
    emit changed();
}

void AppController::resyncInputMethod()
{
    // Wayland input methods re-arm on focus changes only; after a window state
    // change (fullscreen in/out) the text input target can be left behind.
    // Forcing a query re-sends the input state to the input method.
    if (QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
}

void AppController::openDataDir()
{
    Util::openPath(Util::stDataDir());
}

// ---------- invokables: update / install ----------

void AppController::checkUpdates()
{
    if (m_updateRunning || m_settings->stRoot.isEmpty())
        return;
    m_updateRunning = true;
    m_updateCanUpdate = false;
    m_updateStatus = QStringLiteral("Checking for updates (git fetch --tags)...");
    emit changed();

    const QString root = m_settings->stRoot;
    const QPointer<AppController> self(this);
    const QPointer<Backend> backend(m_backend);
    startWorker([self, backend, root] {
        const auto r = Updater::check(root, [backend](const QString &l) {
            QMetaObject::invokeMethod(qApp, [backend, l] {
                if (backend)
                    backend->log(l);
            });
        });
        QMetaObject::invokeMethod(qApp, [self, r] {
            if (!self)
                return;
            self->m_updateRunning = false;
            self->m_updateTag = (!r.upToDate && !r.error.isEmpty()) || r.upToDate ? QString() : r.latest;
            self->m_updateCanUpdate = !r.upToDate && !r.latest.isEmpty();
            self->m_updateStatus =
                !r.error.isEmpty()
                    ? QStringLiteral("✕ %1").arg(r.error)
                    : (r.upToDate
                           ? QStringLiteral("✓ Already up to date (v%1)").arg(r.current)
                           : QStringLiteral("Current v%1 → latest v%2 (press \"Update\" to start)")
                                 .arg(r.current, r.latest));
            self->emit changed();
        });
    });
}

void AppController::performUpdate()
{
    if (m_updateRunning || m_updateTag.isEmpty())
        return;
    const QString tag = m_updateTag;
    const QString root = m_settings->stRoot;
    m_updateRunning = true;
    m_updateCanUpdate = false;
    m_updateStatus = QStringLiteral("Updating... (detailed output in the launcher log)");
    emit changed();

    const QPointer<AppController> self(this);
    const QPointer<Backend> backend(m_backend);
    startWorker([self, backend, root, tag] {
        QString err;
        const bool ok = Updater::perform(
            root, tag,
            [backend](const QString &l) {
                QMetaObject::invokeMethod(qApp, [backend, l] {
                    if (backend)
                        backend->log(l);
                });
            },
            [backend] {
                // Stopping the backend has to happen on the main thread
                // (state machine and timers live there)
                if (!backend)
                    return;
                QMetaObject::invokeMethod(qApp, [backend] {
                    if (backend)
                        backend->stop();
                });
                backend->waitStopped(8000); // block until the child is gone (atomic read only)
            },
            &err);
        QMetaObject::invokeMethod(qApp, [self, ok, err] {
            if (!self)
                return;
            self->m_updateRunning = false;
            if (!ok)
                self->m_updateCanUpdate = true;
            self->m_updateStatus =
                ok ? QStringLiteral("✓ Update finished. The backend is stopped; press "
                                    "\"Start SillyTavern\" to run the new version.")
                   : QStringLiteral("✕ Update failed: %1 (see the launcher log)").arg(err);
            self->emit changed();
        });
    });
}

void AppController::installNew(const QString &parentDir)
{
    const QString parent = parentDir.trimmed();
    if (parent.isEmpty() || !QDir(parent).exists()) {
        showMessage(QStringLiteral("Installation"),
                    QStringLiteral("Please choose an existing directory first."), true);
        return;
    }
    if (QDir(parent + QStringLiteral("/SillyTavern")).exists()) {
        showMessage(QStringLiteral("Installation"),
                    QStringLiteral("%1/SillyTavern already exists. Delete it or choose another "
                                   "directory to reinstall.")
                        .arg(parent),
                    true);
        return;
    }
    if (m_installRunning)
        return;
    m_installRunning = true;
    m_installPhase = QStringLiteral("Preparing...");
    emit changed();

    const QPointer<AppController> self(this);
    const QPointer<Backend> backend(m_backend);
    startWorker([self, backend, parent] {
        QString err;
        const auto dir = Installer::install(
            parent,
            [self](const QString &p) {
                QMetaObject::invokeMethod(qApp, [self, p] {
                    if (self) {
                        self->m_installPhase = p;
                        self->emit changed();
                    }
                });
            },
            [backend](const QString &l) {
                QMetaObject::invokeMethod(qApp, [backend, l] {
                    if (backend)
                        backend->log(l);
                });
            },
            &err);
        QMetaObject::invokeMethod(qApp, [self, dir, err] {
            if (!self)
                return;
            self->m_installRunning = false;
            if (dir.has_value()) {
                self->m_installPhase = QStringLiteral("✅ Installation finished");
                self->m_settings->stRoot = *dir;
                self->m_settings->save();
                self->refreshAll();
                self->showMessage(
                    QStringLiteral("Installation successful"),
                    QStringLiteral("✅ SillyTavern was installed successfully.\n\nLocation: %1\n\n"
                                   "You can now press \"▶  Start SillyTavern\".")
                        .arg(*dir),
                    false);
            } else {
                self->m_installPhase = QStringLiteral("Installation failed");
                self->showMessage(QStringLiteral("Installation failed"), err, true);
            }
            self->emit changed();
        });
    });
}

// ---------- invokables: quit contract ----------

// Window contract (DESIGN section 2): closing the management window stops the
// backend (SIGTERM -> 5s -> SIGKILL; Windows: kill immediately) and quits.
// Returns false while the stop is in flight; quitReady() then asks the QML
// window to close for real. A 7 second fallback forces the quit.
bool AppController::requestCloseWindow()
{
    const auto st = m_backend->status();
    const bool active = st == Backend::Status::Starting || st == Backend::Status::Running
                        || st == Backend::Status::Stopping;
    if (active && !m_shutdownReady) {
        m_shutdownReady = true;
        m_backend->stop();
        m_shutdownConn = connect(m_backend, &Backend::stateChanged, this, [this] {
            const auto s = m_backend->status();
            if (s == Backend::Status::Stopped || s == Backend::Status::Error)
                emit quitReady();
        });
        QTimer::singleShot(7000, this, [this] {
            if (m_shutdownReady)
                emit quitReady();
        });
        // During probe/npm there is no child process yet, so stop() finished synchronously
        if (m_backend->status() == Backend::Status::Stopped
            || m_backend->status() == Backend::Status::Error)
            emit quitReady();
        return false;
    }
    if (m_shutdownConn) {
        disconnect(m_shutdownConn);
        m_shutdownConn = {};
    }
    waitForWorkers(2000);
    return true;
}

// ---------- workers ----------

// Register and start a worker thread: it removes itself from the list and
// deletes itself once finished. Cross-thread callbacks inside the thread body
// must be posted to qApp with a QPointer guard so that nothing touches objects
// that were destroyed while the window is closing.
QThread *AppController::startWorker(const std::function<void()> &body)
{
    auto *t = QThread::create(body);
    m_workers.append(t);
    connect(t, &QThread::finished, this, [this, t] {
        m_workers.removeAll(t);
        t->deleteLater();
    });
    t->start();
    return t;
}

void AppController::waitForWorkers(int timeoutMs)
{
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    const auto workers = m_workers; // copy: the finished handler modifies the original list
    for (const QPointer<QThread> &t : workers) {
        if (!t || !t->isRunning())
            continue;
        t->requestInterruption();
        const qint64 left = deadline - QDateTime::currentMSecsSinceEpoch();
        if (left <= 0)
            break;
        t->wait(ulong(left));
    }
}

// ---------- backend state ----------

void AppController::onLog(const QString &line)
{
    m_logText += line;
    m_logText += QLatin1Char('\n');
    // Live log panel: keep the same 2000 line window the widget version used
    constexpr int kMaxLines = 2000;
    int lines = m_logText.count(QLatin1Char('\n'));
    if (lines > kMaxLines) {
        // drop whole leading lines until the window fits again
        int drop = lines - kMaxLines;
        int pos = 0;
        while (drop > 0) {
            const int nl = m_logText.indexOf(QLatin1Char('\n'), pos);
            if (nl < 0)
                break;
            pos = nl + 1;
            --drop;
        }
        m_logText.remove(0, pos);
    }
    emit changed();
}

void AppController::onStateChanged()
{
    // A new start clears the log panel
    if (m_backend->status() == Backend::Status::Starting
        && m_prevStatus != Backend::Status::Starting) {
        m_logText.clear();
    }

    // ST window lifecycle: open when entering Running, close when leaving it
    if (m_prevStatus != Backend::Status::Running
        && m_backend->status() == Backend::Status::Running
        && !m_backend->url().isEmpty()) {
        if (!m_stWindowVisible) {
            m_stWindowVisible = true;
            emit openStWindowRequested();
        }
    } else if (m_prevStatus == Backend::Status::Running
               && m_backend->status() != Backend::Status::Running) {
        // Closing the ST window does not stop the backend (its lifecycle is
        // driven by the backend state alone)
        if (m_stWindowVisible) {
            m_stWindowVisible = false;
            emit closeStWindowRequested();
        }
    }
    m_prevStatus = m_backend->status();
    refreshAll();
}

void AppController::onForeignInstance(int port)
{
    m_foreignVisible = true;
    m_foreignText =
        QStringLiteral("Port %1 already has a SillyTavern instance running (it was not started by "
                       "this launcher).\n\nIts output does not belong to the launcher and cannot "
                       "be shown in the backend output panel.")
            .arg(port);
    m_foreignPort = port;
    emit changed();
}

void AppController::foreignTakeover()
{
    m_foreignVisible = false;
    emit changed();
    m_backend->foreignTakeover();
}

void AppController::foreignAttach()
{
    m_foreignVisible = false;
    emit changed();
    m_backend->foreignAttach(m_foreignPort);
}

void AppController::foreignCancel()
{
    m_foreignVisible = false;
    emit changed();
    m_backend->foreignCancel();
}

// ---------- derived UI state ----------

void AppController::refreshAll()
{
    m_bound = !m_settings->stRoot.isEmpty();

    if (!m_bound) {
        m_rootText = QStringLiteral("Installation: not bound");
        m_versionText.clear();
    } else {
        const auto check = Util::validateRoot(m_settings->stRoot);
        const QString ver = check.version.isEmpty() ? QStringLiteral("unknown version")
                                                    : QStringLiteral("v%1").arg(check.version);
        const QString git =
            check.isGitRepo ? QString() : QStringLiteral(" (not a git repository, updates unavailable)");
        m_versionText = QStringLiteral("SillyTavern %1%2").arg(ver, git);
        m_rootText = QStringLiteral("Installation: %1").arg(m_settings->stRoot);
    }

    switch (m_backend->status()) {
    case Backend::Status::Stopped:
        m_statusText = QStringLiteral("Stopped");
        m_statusColor = QStringLiteral("#9a8fb0");
        break;
    case Backend::Status::Starting:
        m_statusText = QStringLiteral("Starting...");
        m_statusColor = QStringLiteral("#facc15");
        break;
    case Backend::Status::Stopping:
        m_statusText = QStringLiteral("Stopping...");
        m_statusColor = QStringLiteral("#facc15");
        break;
    case Backend::Status::Running:
        m_statusText = QStringLiteral("Running");
        m_statusColor = QStringLiteral("#4ade80");
        break;
    case Backend::Status::Error:
        m_statusText = QStringLiteral("Error");
        m_statusColor = QStringLiteral("#f87171");
        break;
    }

    m_urlText = (m_backend->status() == Backend::Status::Running && !m_backend->url().isEmpty())
                    ? m_backend->url()
                    : QString();
    m_errorText = (m_backend->status() == Backend::Status::Error && !m_backend->lastError().isEmpty())
                      ? QStringLiteral("✕ %1").arg(m_backend->lastError())
                      : QString();
    m_needsInstallDeps =
        m_backend->status() == Backend::Status::Error && m_backend->needsNpmInstall();

    m_canStart = m_bound
                 && (m_backend->status() == Backend::Status::Stopped
                     || m_backend->status() == Backend::Status::Error);
    m_canOpen = m_backend->status() == Backend::Status::Running;
    m_canStop = m_backend->status() == Backend::Status::Running
                || m_backend->status() == Backend::Status::Starting;

    emit changed();
}
