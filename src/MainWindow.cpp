// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MainWindow.h"

#include "Installer.h"
#include "Settings.h"
#include "StWindow.h"
#include "Updater.h"
#include "Util.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QDialog>
#include <QFileDialog>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_settings(std::make_unique<Settings>(Settings::load()))
    , m_backend(new Backend(this))
{
    buildUi();
    connect(m_backend, &Backend::logLine, this, &MainWindow::onLog);
    connect(m_backend, &Backend::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_backend, &Backend::foreignInstanceFound, this, &MainWindow::onForeignInstance);

    if (!m_settings->stRoot.isEmpty())
        m_stack->setCurrentIndex(1);
    refreshInfo();
    applyState();
}

MainWindow::~MainWindow()
{
    // Wait for the worker threads to finish (on timeout they are abandoned;
    // every thread callback is QPointer-guarded and never touches destroyed objects)
    waitForWorkers(2000);
    if (m_shutdownConn)
        disconnect(m_shutdownConn);
}

// Window contract (DESIGN section 2): closing the management window stops the
// backend (SIGTERM -> 5s -> SIGKILL; Windows: kill immediately) and quits.
// The stop is driven by the main event loop, so the close event is intercepted
// first and the window closes once the state machine reaches a final state.
// A 7 second fallback forces the close so the user can always quit.
void MainWindow::closeEvent(QCloseEvent *event)
{
    const auto st = m_backend->status();
    const bool active = st == Backend::Status::Starting || st == Backend::Status::Running
                        || st == Backend::Status::Stopping;
    if (active && !m_shutdownReady) {
        event->ignore();
        m_shutdownReady = true;
        m_backend->stop();
        m_shutdownConn = connect(m_backend, &Backend::stateChanged, this, [this] {
            const auto s = m_backend->status();
            if (s == Backend::Status::Stopped || s == Backend::Status::Error)
                close();
        });
        QTimer::singleShot(7000, this, [this] {
            if (m_shutdownReady && isVisible())
                close();
        });
        // During probe/npm there is no child process yet, so stop() finished synchronously
        if (m_backend->status() == Backend::Status::Stopped
            || m_backend->status() == Backend::Status::Error)
            close();
        return;
    }
    if (m_shutdownConn) {
        disconnect(m_shutdownConn);
        m_shutdownConn = {};
    }
    waitForWorkers(2000);
    event->accept();
}

// Register and start a worker thread: it removes itself from the list and
// deletes itself once finished. Cross-thread callbacks inside the thread body
// must be posted to qApp with a QPointer guard so that nothing touches objects
// that were destroyed while the window is closing.
QThread *MainWindow::startWorker(const std::function<void()> &body)
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

void MainWindow::waitForWorkers(int timeoutMs)
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

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("QuteTavern"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/icon.png")));
    resize(520, 720);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(buildWizardPage());
    m_stack->addWidget(buildMainPage());
    setCentralWidget(m_stack);
}

QWidget *MainWindow::buildWizardPage()
{
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(24, 24, 24, 24);
    lay->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("Welcome to QuteTavern"), page);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto *sub = new QLabel(
        QStringLiteral("To get started, bind the SillyTavern installation directory\n"
                       "(the folder that contains server.js and package.json)"),
        page);
    sub->setStyleSheet("color: #888;");

    auto *row = new QHBoxLayout();
    m_wizPath = new QLineEdit(page);
    m_wizPath->setPlaceholderText(QStringLiteral("/home/you/SillyTavern"));
    auto *browse = new QPushButton(QStringLiteral("Browse..."), page);
    row->addWidget(m_wizPath, 1);
    row->addWidget(browse);

    m_wizCheck = new QLabel(page);
    m_wizCheck->setTextFormat(Qt::RichText);

    m_wizBind = new QPushButton(QStringLiteral("Bind and continue"), page);
    m_wizBind->setEnabled(false);

    auto *installBtn = new QPushButton(QStringLiteral("Install a new copy from GitHub..."), page);
    installBtn->setStyleSheet("color: #7c6cf0;");

    auto *spacer = new QWidget(page);
    lay->addWidget(title);
    lay->addWidget(sub);
    lay->addLayout(row);
    lay->addWidget(m_wizCheck);
    lay->addWidget(spacer, 1);
    lay->addWidget(m_wizBind);
    lay->addWidget(installBtn);

    m_wizTimer = new QTimer(this);
    m_wizTimer->setSingleShot(true);
    m_wizTimer->setInterval(300);
    connect(m_wizTimer, &QTimer::timeout, this, &MainWindow::onWizardValidate);
    connect(m_wizPath, &QLineEdit::textChanged, this,
            [this] { m_wizTimer->start(); });

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select the SillyTavern installation directory"), QDir::homePath());
        if (!dir.isEmpty())
            m_wizPath->setText(dir);
    });
    connect(m_wizBind, &QPushButton::clicked, this,
            [this] { bindRoot(m_wizPath->text().trimmed()); });
    connect(installBtn, &QPushButton::clicked, this, [this] { openInstallDialog(); });

    return page;
}

QWidget *MainWindow::buildMainPage()
{
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(8);

    // Header: title + version | status
    auto *header = new QHBoxLayout();
    auto *titleBox = new QVBoxLayout();
    auto *title = new QLabel(QStringLiteral("QuteTavern"), page);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    title->setFont(titleFont);
    m_versionLbl = new QLabel(page);
    m_versionLbl->setStyleSheet("color: #888;");
    titleBox->addWidget(title);
    titleBox->addWidget(m_versionLbl);

    auto *statusBox = new QVBoxLayout();
    auto *statusRow = new QHBoxLayout();
    statusRow->setAlignment(Qt::AlignRight);
    m_statusDot = new QLabel(page);
    m_statusDot->setTextFormat(Qt::RichText);
    m_statusText = new QLabel(QStringLiteral("Stopped"), page);
    statusRow->addWidget(m_statusDot);
    statusRow->addWidget(m_statusText);
    m_urlLbl = new QLabel(page);
    m_urlLbl->setAlignment(Qt::AlignRight);
    m_urlLbl->setStyleSheet("color: #888; font-family: monospace;");
    statusBox->addLayout(statusRow);
    statusBox->addWidget(m_urlLbl);

    header->addLayout(titleBox, 1);
    header->addLayout(statusBox);

    m_rootLbl = new QLabel(page);
    m_rootLbl->setStyleSheet("color: #888;");
    m_errorLbl = new QLabel(page);
    m_errorLbl->setWordWrap(true);
    m_errorLbl->setStyleSheet("color: #f87171;");
    m_errorLbl->hide();

    auto *actions = new QHBoxLayout();
    m_startBtn = new QPushButton(QStringLiteral("▶  Start SillyTavern"), page);
    m_openBtn = new QPushButton(QStringLiteral("Open ST window"), page);
    m_stopBtn = new QPushButton(QStringLiteral("■  Stop backend"), page);
    actions->addWidget(m_startBtn, 1);
    actions->addWidget(m_openBtn, 1);
    actions->addWidget(m_stopBtn, 1);

    auto *secondary = new QHBoxLayout();
    auto *updateBtn = new QPushButton(QStringLiteral("Check for updates"), page);
    auto *dataBtn = new QPushButton(QStringLiteral("Data directory"), page);
    auto *settingsBtn = new QPushButton(QStringLiteral("Preferences"), page);
    auto *installNewBtn =
        new QPushButton(QStringLiteral("Install a new copy..."), page);
    installNewBtn->setToolTip(QStringLiteral(
        "Clone a fresh SillyTavern from GitHub (release branch + latest tag) "
        "and bind it automatically"));
    secondary->addWidget(updateBtn, 1);
    secondary->addWidget(dataBtn, 1);
    secondary->addWidget(settingsBtn, 1);
    secondary->addWidget(installNewBtn, 1);

    m_installDepsBtn = new QPushButton(QStringLiteral("Install dependencies and retry"), page);
    m_installDepsBtn->hide();

    auto *logTitle = new QLabel(QStringLiteral("Backend output (live)"), page);
    logTitle->setStyleSheet("color: #888;");
    m_log = new QPlainTextEdit(page);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    QFont mono;
    mono.setFamilies({QStringLiteral("monospace")});
    mono.setPointSize(9);
    m_log->setFont(mono);

    lay->addLayout(header);
    lay->addWidget(m_rootLbl);
    lay->addWidget(m_errorLbl);
    lay->addLayout(actions);
    lay->addLayout(secondary);
    lay->addWidget(m_installDepsBtn);
    lay->addWidget(logTitle);
    lay->addWidget(m_log, 1);

    connect(m_startBtn, &QPushButton::clicked, this, [this] { m_backend->start(); });
    connect(m_openBtn, &QPushButton::clicked, this, [this] {
        if (!m_backend->url().isEmpty())
            openStWindow(m_backend->url());
    });
    connect(m_stopBtn, &QPushButton::clicked, this, [this] { m_backend->stop(); });
    connect(m_installDepsBtn, &QPushButton::clicked, this,
            [this] { m_backend->installDepsThenStart(); });
    connect(updateBtn, &QPushButton::clicked, this, [this] { openUpdateDialog(); });
    connect(dataBtn, &QPushButton::clicked, this,
            [this] { Util::openPath(Util::stDataDir()); });
    connect(settingsBtn, &QPushButton::clicked, this, [this] { openSettingsDialog(); });
    connect(installNewBtn, &QPushButton::clicked, this, [this] { openInstallDialog(); });

    return page;
}

void MainWindow::onLog(const QString &line)
{
    m_log->appendPlainText(line);
}

void MainWindow::refreshInfo()
{
    if (m_settings->stRoot.isEmpty()) {
        m_rootLbl->setText(QStringLiteral("Installation: not bound"));
        m_versionLbl->clear();
        return;
    }
    const auto check = Util::validateRoot(m_settings->stRoot);
    const QString ver =
        check.version.isEmpty() ? QStringLiteral("unknown version")
                                : QStringLiteral("v%1").arg(check.version);
    const QString git =
        check.isGitRepo ? QString() : QStringLiteral(" (not a git repository, updates unavailable)");
    m_versionLbl->setText(QStringLiteral("SillyTavern %1%2").arg(ver, git));
    m_rootLbl->setText(QStringLiteral("Installation: %1").arg(m_settings->stRoot));
}

void MainWindow::bindRoot(const QString &path)
{
    const auto check = Util::validateRoot(path);
    if (!check.ok) {
        QMessageBox::warning(this, QStringLiteral("Binding failed"), check.error);
        return;
    }
    m_settings->stRoot = path;
    m_settings->save();
    m_stack->setCurrentIndex(1);
    refreshInfo();
    applyState();
    QMetaObject::invokeMethod(m_backend, [this] {
        m_backend->log(QStringLiteral("Bound installation directory: %1").arg(m_settings->stRoot));
    });
}

void MainWindow::onWizardValidate()
{
    const QString path = m_wizPath->text().trimmed();
    if (path.isEmpty()) {
        m_wizCheck->clear();
        m_wizBind->setEnabled(false);
        return;
    }
    const auto check = Util::validateRoot(path);
    if (check.ok) {
        m_wizCheck->setText(QStringLiteral("<span style='color:#4ade80'>✓ Found SillyTavern v%1</span>")
                                .arg(check.version));
        m_wizBind->setEnabled(true);
    } else {
        m_wizCheck->setText(
            QStringLiteral("<span style='color:#f87171'>✕ %1</span>").arg(check.error));
        m_wizBind->setEnabled(false);
    }
}

void MainWindow::onStateChanged()
{
    // A new start clears the log panel
    if (m_backend->status() == Backend::Status::Starting
        && m_prevStatus != Backend::Status::Starting)
        m_log->clear();

    // ST window lifecycle: open when entering Running, close when leaving it
    if (m_prevStatus != Backend::Status::Running
        && m_backend->status() == Backend::Status::Running
        && !m_backend->url().isEmpty()) {
        openStWindow(m_backend->url());
    } else if (m_prevStatus == Backend::Status::Running
               && m_backend->status() != Backend::Status::Running) {
        closeStWindow();
    }
    m_prevStatus = m_backend->status();
    applyState();
}

void MainWindow::applyState()
{
    struct {
        const char *color;
        const char *text;
    } s = {"#f87171", "Unknown"}; // a future Status value must not read uninitialized memory
    switch (m_backend->status()) {
    case Backend::Status::Stopped: s = {"#9a8fb0", "Stopped"}; break;
    case Backend::Status::Starting: s = {"#facc15", "Starting..."}; break;
    case Backend::Status::Stopping: s = {"#facc15", "Stopping..."}; break;
    case Backend::Status::Running: s = {"#4ade80", "Running"}; break;
    case Backend::Status::Error: s = {"#f87171", "Error"}; break;
    }
    m_statusDot->setText(
        QStringLiteral("<span style='color:%1'>●</span>").arg(s.color));
    m_statusText->setText(QString::fromUtf8(s.text));

    if (m_backend->status() == Backend::Status::Running && !m_backend->url().isEmpty())
        m_urlLbl->setText(m_backend->url());
    else
        m_urlLbl->clear();

    if (m_backend->status() == Backend::Status::Error && !m_backend->lastError().isEmpty()) {
        m_errorLbl->setText(QStringLiteral("✕ %1").arg(m_backend->lastError()));
        m_errorLbl->show();
    } else {
        m_errorLbl->hide();
    }
    m_installDepsBtn->setVisible(m_backend->status() == Backend::Status::Error
                                 && m_backend->needsNpmInstall());

    const bool bound = !m_settings->stRoot.isEmpty();
    m_startBtn->setEnabled(
        bound && (m_backend->status() == Backend::Status::Stopped
                  || m_backend->status() == Backend::Status::Error));
    m_openBtn->setEnabled(m_backend->status() == Backend::Status::Running);
    m_stopBtn->setEnabled(m_backend->status() == Backend::Status::Running
                          || m_backend->status() == Backend::Status::Starting);
}

void MainWindow::onForeignInstance(int port)
{
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("Existing instance detected"));
    box.setText(QStringLiteral(
        "Port %1 already has a SillyTavern instance running (it was not started by "
        "this launcher).\n\n"
        "Its output does not belong to the launcher and cannot be shown in the "
        "backend output panel.")
        .arg(port));
    QPushButton *takeover =
        box.addButton(QStringLiteral("Terminate and take over (recommended)"), QMessageBox::AcceptRole);
    QPushButton *attach =
        box.addButton(QStringLiteral("Connect directly (no log)"), QMessageBox::ActionRole);
    QPushButton *cancel =
        box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == takeover)
        m_backend->foreignTakeover();
    else if (box.clickedButton() == attach)
        m_backend->foreignAttach(port);
    else if (box.clickedButton() == cancel)
        m_backend->foreignCancel();
    else
        m_backend->foreignCancel();
}

void MainWindow::openStWindow(const QString &url)
{
    if (m_stWindow) {
        // A page fullscreen window must not be forced back to normal: the page
        // still holds a fullscreen element and StWindow keeps the two states in
        // sync, so only bring the window back to the front.
        if (m_stWindow->isFullScreen()) {
            if (m_stWindow->isMinimized())
                m_stWindow->showFullScreen();
        } else {
            m_stWindow->showNormal();
        }
        m_stWindow->raise();
        m_stWindow->activateWindow();
        return;
    }
    // Settings is owned by MainWindow and outlives the window (StWindow only borrows it)
    m_stWindow = new StWindow(QUrl(url), m_settings.get());
    m_stWindow->setAttribute(Qt::WA_DeleteOnClose);
    m_stWindowConn = connect(m_stWindow, &QObject::destroyed, this,
                             [this] { m_stWindow = nullptr; });
}
// Closing StWindow does not stop the backend (its lifecycle is driven by the
// backend state alone)
void MainWindow::closeStWindow()
{
    if (m_stWindow) {
        // Only drop our own destroyed connection; a wildcard disconnect would cut
        // Qt internal connections as well and makes Qt warn about it
        if (m_stWindowConn) {
            disconnect(m_stWindowConn);
            m_stWindowConn = {};
        }
        m_stWindow->close();
        m_stWindow = nullptr;
    }
}

// ---------- Preferences dialog ----------

void MainWindow::openSettingsDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Preferences"));
    dlg.resize(480, 380);
    auto *lay = new QVBoxLayout(&dlg);
    lay->setSpacing(8);

    auto *rootLbl = new QLabel(
        QStringLiteral("SillyTavern installation directory (restart the backend to apply)"), &dlg);
    rootLbl->setStyleSheet("color: #888;");
    auto *row = new QHBoxLayout();
    auto *edit = new QLineEdit(m_settings->stRoot, &dlg);
    auto *browse = new QPushButton(QStringLiteral("Browse..."), &dlg);
    row->addWidget(edit, 1);
    row->addWidget(browse);
    auto *checkLbl = new QLabel(&dlg);
    checkLbl->setTextFormat(Qt::RichText);

    auto validate = [edit, checkLbl] {
        const QString p = edit->text().trimmed();
        if (p.isEmpty()) {
            checkLbl->clear();
            return;
        }
        const auto c = Util::validateRoot(p);
        checkLbl->setText(c.ok
            ? QStringLiteral("<span style='color:#4ade80'>✓ SillyTavern v%1</span>").arg(c.version)
            : QStringLiteral("<span style='color:#f87171'>✕ %1</span>").arg(c.error));
    };
    connect(edit, &QLineEdit::textChanged, &dlg, validate);
    validate();
    connect(browse, &QPushButton::clicked, this, [this, edit] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select the SillyTavern installation directory"), QDir::homePath());
        if (!dir.isEmpty())
            edit->setText(dir);
    });

    auto *argsLbl = new QLabel(
        QStringLiteral("Extra backend arguments (space separated; --global and friends are "
                       "added by the launcher)"),
        &dlg);
    argsLbl->setStyleSheet("color: #888;");
    auto *argsEdit = new QLineEdit(m_settings->extraBackendArgs.join(' '), &dlg);

    auto *maxCheck = new QCheckBox(QStringLiteral("Maximize the ST window when it opens"), &dlg);
    maxCheck->setChecked(m_settings->autoMaximize);
    auto *rememberCheck = new QCheckBox(QStringLiteral("Remember the ST window size and position"), &dlg);
    rememberCheck->setChecked(m_settings->rememberWindowState);

    auto *save = new QPushButton(QStringLiteral("Save"), &dlg);

    lay->addWidget(rootLbl);
    lay->addLayout(row);
    lay->addWidget(checkLbl);
    lay->addWidget(argsLbl);
    lay->addWidget(argsEdit);
    lay->addWidget(maxCheck);
    lay->addWidget(rememberCheck);
    lay->addWidget(save);

    connect(save, &QPushButton::clicked, this, [&, this] {
        const auto c = Util::validateRoot(edit->text().trimmed());
        if (!c.ok) {
            QMessageBox::warning(this, QStringLiteral("Could not save"),
                                 c.error.isEmpty() ? QStringLiteral("Invalid installation directory")
                                                   : c.error);
            return;
        }
        m_settings->stRoot = edit->text().trimmed();
        m_settings->extraBackendArgs =
            argsEdit->text().split(' ', Qt::SkipEmptyParts);
        m_settings->autoMaximize = maxCheck->isChecked();
        m_settings->rememberWindowState = rememberCheck->isChecked();
        m_settings->save();
        refreshInfo();
        applyState();
        dlg.accept();
    });

    dlg.exec();
}


// ---------- Update check dialog ----------
// Results from the worker thread are posted back to the main thread with
// invokeMethod; the dialog is a stack object and may already be gone, so the
// widgets are guarded with QPointer.

void MainWindow::openUpdateDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Check for updates"));
    dlg.resize(580, 200);
    auto *lay = new QVBoxLayout(&dlg);
    lay->setSpacing(10);

    auto *status = new QLabel(&dlg);
    status->setWordWrap(true);
    auto *hint = new QLabel(
        QStringLiteral("Updating means: git fetch --tags + checkout of the latest tag + "
                       "npm install.\n"
                       "Characters and chats live in the global data directory and are not "
                       "affected. Detailed output is shown in the launcher log."),
        &dlg);
    hint->setStyleSheet("color: #888;");
    hint->setWordWrap(true);

    auto *recheck = new QPushButton(QStringLiteral("Check again"), &dlg);
    auto *doUpdate = new QPushButton(QStringLiteral("Update"), &dlg);
    doUpdate->setEnabled(false);
    auto *close = new QPushButton(QStringLiteral("Close"), &dlg);
    auto *bbox = new QHBoxLayout();
    bbox->addWidget(recheck);
    bbox->addWidget(doUpdate);
    bbox->addStretch(1);
    bbox->addWidget(close);
    lay->addWidget(status);
    lay->addWidget(hint, 1);
    lay->addLayout(bbox);

    const QString root = m_settings->stRoot;
    const QPointer<QLabel> statusGuard(status);
    const QPointer<QPushButton> recheckGuard(recheck), doUpdateGuard(doUpdate);

    auto runCheck = [this, root, statusGuard, recheckGuard, doUpdateGuard] {
        if (statusGuard)
            statusGuard->setText(QStringLiteral("Checking for updates (git fetch --tags)..."));
        if (recheckGuard)
            recheckGuard->setEnabled(false);
        // The thread body never captures a raw this: results are posted to qApp
        // and guarded with QPointer
        const QPointer<MainWindow> self(this);
        const QPointer<Backend> backend(m_backend);
        startWorker([self, backend, root, statusGuard, recheckGuard, doUpdateGuard] {
            const auto r = Updater::check(root, [backend](const QString &l) {
                QMetaObject::invokeMethod(qApp, [backend, l] {
                    if (backend)
                        backend->log(l);
                });
            });
            QMetaObject::invokeMethod(qApp, [self, r, statusGuard, recheckGuard, doUpdateGuard] {
                if (statusGuard)
                    statusGuard->setText(
                        !r.error.isEmpty()
                            ? QStringLiteral("✕ %1").arg(r.error)
                            : (r.upToDate
                                   ? QStringLiteral("✓ Already up to date (v%1)").arg(r.current)
                                   : QStringLiteral("Current v%1 -> latest v%2 (press \"Update\" to start)")
                                         .arg(r.current, r.latest)));
                if (self && !r.upToDate && !r.latest.isEmpty())
                    self->m_updateTag = r.latest;
                if (doUpdateGuard)
                    doUpdateGuard->setEnabled(self && !r.upToDate && !r.latest.isEmpty());
                if (recheckGuard)
                    recheckGuard->setEnabled(true);
            });
        });
    };

    connect(recheck, &QPushButton::clicked, this, [runCheck] { runCheck(); });
    connect(close, &QPushButton::clicked, this, [&dlg] { dlg.reject(); });
    connect(doUpdate, &QPushButton::clicked, this,
            [this, &dlg, root, status, recheck, doUpdate, statusGuard, recheckGuard, doUpdateGuard] {
        const QString tag = m_updateTag;
        if (tag.isEmpty())
            return;
        doUpdate->setEnabled(false);
        recheck->setEnabled(false);
        status->setText(QStringLiteral("Updating... (detailed output in the launcher log)"));
        const QPointer<MainWindow> self(this);
        const QPointer<Backend> backend(m_backend);
        startWorker([self, backend, root, tag, statusGuard, recheckGuard, doUpdateGuard] {
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
            QMetaObject::invokeMethod(
                qApp, [self, ok, err, statusGuard, recheckGuard, doUpdateGuard] {
                    if (statusGuard) {
                        statusGuard->setText(
                            ok
                                ? QStringLiteral("✓ Update finished. The backend is stopped; press "
                                                 "\"Start\" in the main window to run the new version.")
                                : QStringLiteral("✕ Update failed: %1 (see the launcher log)").arg(err));
                    }
                    if (recheckGuard)
                        recheckGuard->setEnabled(true);
                    if (doUpdateGuard && !ok)
                        doUpdateGuard->setEnabled(true);
                });
        });
    });

    runCheck();
    dlg.exec();
}

// ---------- Install dialog (install a new copy from GitHub) ----------

void MainWindow::openInstallDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Install SillyTavern from GitHub"));
    dlg.resize(540, 200);
    auto *lay = new QVBoxLayout(&dlg);
    lay->setSpacing(10);

    auto *intro = new QLabel(
        QStringLiteral("Clones the SillyTavern release branch into the directory you pick, "
                       "checks out the latest release tag and installs the dependencies."),
        &dlg);
    intro->setWordWrap(true);

    auto *row = new QHBoxLayout();
    auto *edit = new QLineEdit(QDir::homePath(), &dlg);
    auto *browse = new QPushButton(QStringLiteral("Choose directory..."), &dlg);
    row->addWidget(edit, 1);
    row->addWidget(browse);

    auto *phase = new QLabel(QStringLiteral("Waiting to start..."), &dlg);
    phase->setStyleSheet("color: #888;");
    phase->setWordWrap(true);

    auto *go = new QPushButton(QStringLiteral("Start installation"), &dlg);
    auto *close = new QPushButton(QStringLiteral("Close"), &dlg);
    auto *bbox = new QHBoxLayout();
    bbox->addWidget(go);
    bbox->addStretch(1);
    bbox->addWidget(close);

    lay->addWidget(intro);
    lay->addLayout(row);
    lay->addWidget(phase, 1);
    lay->addLayout(bbox);

    const QPointer<QLabel> phaseGuard(phase);
    const QPointer<QPushButton> goGuard(go);

    connect(browse, &QPushButton::clicked, this, [this, edit] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose the parent directory for the installation"), QDir::homePath());
        if (!dir.isEmpty())
            edit->setText(dir);
    });
    connect(close, &QPushButton::clicked, this, [&dlg] { dlg.reject(); });

    connect(go, &QPushButton::clicked, this, [this, &dlg, edit, phaseGuard, goGuard] {
        const QString parent = edit->text().trimmed();
        if (parent.isEmpty() || !QDir(parent).exists()) {
            QMessageBox::warning(this, QStringLiteral("Installation"),
                                 QStringLiteral("Please choose an existing directory first."));
            return;
        }
        if (QDir(parent + "/SillyTavern").exists()) {
            QMessageBox::warning(
                this, QStringLiteral("Installation"),
                QStringLiteral("%1/SillyTavern already exists. Delete it or choose another "
                               "directory to reinstall.").arg(parent));
            return;
        }
        if (goGuard)
            goGuard->setEnabled(false);
        if (phaseGuard)
            phaseGuard->setText(QStringLiteral("Preparing..."));

        const QPointer<MainWindow> self(this);
        const QPointer<Backend> backend(m_backend);
        startWorker([self, backend, parent, phaseGuard] {
            QString err;
            const auto dir = Installer::install(
                parent,
                [phaseGuard](const QString &p) {
                    QMetaObject::invokeMethod(qApp, [phaseGuard, p] {
                        if (phaseGuard)
                            phaseGuard->setText(p);
                    });
                },
                [backend](const QString &l) {
                    QMetaObject::invokeMethod(qApp, [backend, l] {
                        if (backend)
                            backend->log(l);
                    });
                },
                &err);
            QMetaObject::invokeMethod(qApp, [self, dir, err, phaseGuard] {
                if (phaseGuard)
                    phaseGuard->setText(dir.has_value()
                                            ? QStringLiteral("✅ Installation finished")
                                            : QStringLiteral("Installation failed"));
                if (!self)
                    return;
                if (dir.has_value()) {
                    self->m_settings->stRoot = *dir;
                    self->m_settings->save();
                    self->m_stack->setCurrentIndex(1);
                    self->refreshInfo();
                    self->applyState();
                    QMessageBox::information(
                        self, QStringLiteral("Installation successful"),
                        QStringLiteral("✅ SillyTavern was installed successfully.\n\n"
                                       "Location: %1\n\n"
                                       "You can now press \"▶  Start SillyTavern\".").arg(*dir));
                } else {
                    QMessageBox::warning(self, QStringLiteral("Installation failed"), err);
                }
            });
        });
    });

    dlg.exec();
}