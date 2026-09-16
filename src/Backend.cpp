// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Backend.h"

#include "Settings.h"
#include "Util.h"

#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QPointer>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QThread>

#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/prctl.h>
#endif

static constexpr int kStartTimeoutMs = 90'000;
static constexpr int kKillTimeoutMs = 5'000;
static constexpr int kNpmTimeoutMs = 10 * 60'000;
static constexpr qsizetype kMaxLineBufBytes = 1024 * 1024; // guard against endless lines

Backend::Backend(QObject *parent)
    : QObject(parent)
{
    m_startTimer.setSingleShot(true);
    m_killTimer.setSingleShot(true);
    m_npmTimer.setSingleShot(true);
    connect(&m_startTimer, &QTimer::timeout, this, &Backend::onStartTimeout);
    connect(&m_killTimer, &QTimer::timeout, this, &Backend::onKillTimeout);
    connect(&m_npmTimer, &QTimer::timeout, this, &Backend::onNpmTimeout);
    m_probeTimer.setSingleShot(true);
}

Backend::~Backend()
{
    // The takeover cleanup thread blocks for up to 3s; wait for it so that no
    // thread keeps running after this object is gone
    if (m_takeoverThread && m_takeoverThread->isRunning())
        m_takeoverThread->wait(4000);
}

QString Backend::stRoot() const
{
    return Settings::load().stRoot;
}

bool Backend::waitStopped(int timeoutMs) const
{
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (m_nodeAlive.load()) {
        if (QDateTime::currentMSecsSinceEpoch() >= deadline)
            return false;
        QThread::msleep(100);
    }
    return true;
}

void Backend::log(const QString &line)
{
    m_tail.append(line);
    if (m_tail.size() > 400)
        m_tail.removeFirst();
    emit logLine(line);
}

void Backend::setStatus(Status s)
{
    m_status = s;
    emit stateChanged();
}

void Backend::start()
{
    if (m_status == Status::Running || m_status == Status::Starting
        || m_status == Status::Stopping)
        return;
    if (stRoot().isEmpty()) {
        setError(QStringLiteral("No SillyTavern installation bound yet - please bind one first"),
                 false);
        return;
    }

    m_url.clear();
    m_lastError.clear();
    m_needsNpm = false;
    m_stopping = false;
    m_timeoutAbort = false;
    m_npmTimeout = false;
    m_lineBuf.clear(); // drop a half line left over from a previous run
    m_tail.clear();    // exit classification only looks at this run
    setStatus(Status::Starting);
    beginProbe();
}

void Backend::beginProbe()
{
    cleanupProbe();
    m_probeBuf.clear();
    m_probe = new QTcpSocket(this);
    connect(m_probe, &QTcpSocket::connected, this, &Backend::onProbeConnected);
    connect(m_probe, &QTcpSocket::readyRead, this, &Backend::onProbeReadyRead);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(m_probe, &QTcpSocket::errorOccurred, this, [this] { onProbeFailed(); });
#endif
    connect(&m_probeTimer, &QTimer::timeout, this, &Backend::onProbeFailed);
    m_probeTimer.start(1500);
    m_probe->connectToHost(QHostAddress::LocalHost, quint16(Util::detectPort()));
}

void Backend::cleanupProbe()
{
    m_probeTimer.stop();
    disconnect(&m_probeTimer, nullptr, nullptr, nullptr);
    if (m_probe) {
        // Disconnect before abort() so that a signal emitted during teardown
        // cannot re-enter probeDone -> afterProbeContinue
        disconnect(m_probe, nullptr, nullptr, nullptr);
        m_probe->abort();
        m_probe->deleteLater();
        m_probe = nullptr;
    }
    m_probeBuf.clear();
}

void Backend::onProbeConnected()
{
    if (!m_probe)
        return;
    m_probe->write("HEAD /csrf-token HTTP/1.0\r\nHost: localhost\r\n\r\n");
}

void Backend::onProbeReadyRead()
{
    if (!m_probe)
        return;
    // Accumulate a full status line: the first TCP segment may carry only a few
    // bytes, so assuming that 8 bytes have arrived is not safe
    m_probeBuf += m_probe->readAll();
    int nl = m_probeBuf.indexOf("\r\n");
    if (nl < 0)
        nl = m_probeBuf.indexOf('\n');
    if (nl < 0) {
        if (m_probeBuf.size() > 4096) // guard: do not buffer an endless reply
            probeDone(false);
        return;
    }
    probeDone(m_probeBuf.left(nl).startsWith("HTTP/"));
}

void Backend::onProbeFailed()
{
    if (!m_probe || m_status != Status::Starting)
        return;
    probeDone(false);
}

void Backend::probeDone(bool alive)
{
    cleanupProbe();
    if (m_status != Status::Starting)
        return; // cancelled in the meantime
    if (alive) {
        const int port = Util::detectPort();
        log(QStringLiteral("Port %1 is already serving a SillyTavern instance "
                           "(not started by this launcher).")
                .arg(port));
        emit foreignInstanceFound(port);
        return;
    }
    afterProbeContinue();
}

void Backend::afterProbeContinue()
{
    const Settings s = Settings::load();
    if (s.stRoot.isEmpty()) {
        setError(QStringLiteral("No SillyTavern installation bound yet"), false);
        return;
    }
    // Dependency self-check: install them when missing (same flags as start.sh)
    if (!QDir(s.stRoot + "/node_modules/express").exists()) {
        startNpmInstall();
        return;
    }
    spawnNode();
}

void Backend::startNpmInstall()
{
    log(QStringLiteral("Installing dependencies (npm install --omit=dev), "
                       "this may take a few minutes..."));

    const QString root = stRoot();
    m_npm = new QProcess(this);
    m_npm->setProcessChannelMode(QProcess::MergedChannels);
    m_npm->setWorkingDirectory(root);
    QProcessEnvironment env = Util::commandEnv();
    env.insert("NODE_ENV", "production");
    m_npm->setProcessEnvironment(env);
    // npm is resolved to an absolute path for the same reason as node
    const QString npm = Util::findCommand(QStringLiteral("npm"));
    if (npm.isEmpty()) {
        m_npm->deleteLater();
        m_npm = nullptr;
        setError(QStringLiteral("npm executable not found. Please make sure Node.js (>= 20) "
                                "is installed and on PATH."),
                 true);
        return;
    }
#ifdef Q_OS_WIN
    // npm is a batch script on Windows and has to be started through cmd
    m_npm->setProgram("cmd");
    m_npm->setArguments(QStringList{"/c", npm} + Util::npmInstallArgs());
#else
    m_npm->setProgram(npm);
    m_npm->setArguments(Util::npmInstallArgs());
#endif
    connect(m_npm, &QProcess::readyReadStandardOutput, this, &Backend::onNpmReadyRead);
    connect(m_npm, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &Backend::onNpmFinished);
    connect(m_npm, &QProcess::errorOccurred, this, &Backend::onNpmError);
    m_npm->start();
    m_npmTimer.start(kNpmTimeoutMs);
}

void Backend::onNpmReadyRead()
{
    auto *np = qobject_cast<QProcess *>(sender());
    if (!np || np != m_npm)
        return;
    const auto lines = QString::fromUtf8(np->readAllStandardOutput()).split('\n');
    for (const QString &l : lines)
        if (!l.trimmed().isEmpty())
            log(Util::ansiStrip(l));
}

void Backend::onNpmFinished(int code, QProcess::ExitStatus)
{
    auto *np = qobject_cast<QProcess *>(sender());
    if (np) {
        np->deleteLater();
        if (np != m_npm)
            return; // a superseded npm process: recycle it, keep the state machine alone
    }
    m_npm = nullptr;
    m_npmTimer.stop();

    // A stop already ran to completion (stop() finishes synchronously when npm
    // was the only child); recycling npm must not re-announce Stopped
    if (m_stopping || m_status != Status::Starting)
        return;
    if (m_npmTimeout) {
        m_npmTimeout = false;
        setError(QStringLiteral("Dependency installation timed out (%1 minutes) and was "
                                "terminated. Retry later or run npm install manually.")
                     .arg(kNpmTimeoutMs / 60000),
                 true);
        return;
    }
    if (code != 0) {
        setError(QStringLiteral("Automatic dependency installation failed "
                                "(npm install exit code %1). You can use "
                                "\"Install dependencies and retry\".")
                     .arg(code),
                 true);
        return;
    }
    log(QStringLiteral("Dependencies installed, starting the backend."));
    spawnNode();
}

void Backend::onNpmError(QProcess::ProcessError e)
{
    auto *np = qobject_cast<QProcess *>(sender());
    if (e != QProcess::FailedToStart || !np || np != m_npm)
        return;
    m_npm = nullptr;
    m_npmTimer.stop();
    np->deleteLater();
    log(QStringLiteral("Could not start npm: %1").arg(np->errorString()));
    if (m_stopping || m_status != Status::Starting)
        return; // a stop already completed, see onNpmFinished
    setError(QStringLiteral("npm could not be started (not found or not executable). "
                            "Please make sure Node.js/npm are installed and on PATH."),
             true);
}

void Backend::onNpmTimeout()
{
    if (!m_npm || m_status != Status::Starting)
        return;
    log(QStringLiteral("Dependency installation is taking longer than %1 minutes, "
                       "terminating npm...")
            .arg(kNpmTimeoutMs / 60000));
    m_npmTimeout = true;
    m_npm->kill();
}

void Backend::spawnNode()
{
    if (m_status != Status::Starting || m_stopping)
        return;

    // Resolve node to an absolute path: QProcess resolves bare names through
    // the parent's PATH, which does not contain the login shell PATH or the
    // built-in components we append to the child environment.
    const QString node = Util::findCommand(QStringLiteral("node"));
    if (node.isEmpty()) {
        setError(QStringLiteral("node executable not found. Please make sure Node.js (>= 20) "
                                "is installed and on PATH."),
                 false);
        return;
    }

    const Settings s = Settings::load();
    m_lineBuf.clear();
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    m_proc->setWorkingDirectory(s.stRoot);
    QProcessEnvironment env = Util::commandEnv();
    env.insert("NODE_ENV", "production");
    m_proc->setProcessEnvironment(env);
    m_proc->setProgram(node);
    QStringList args{"server.js", "--global", "--browserLaunchEnabled=false"};
    args += s.extraBackendArgs;
    m_proc->setArguments(args);

    connect(m_proc, &QProcess::readyReadStandardOutput, this, &Backend::onProcReadyRead);
    connect(m_proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &Backend::onProcFinished);
    connect(m_proc, &QProcess::errorOccurred, this, &Backend::onProcError);
    // The PID is only valid once the child really started
    connect(m_proc, &QProcess::started, this, [this] {
        auto *p = qobject_cast<QProcess *>(sender());
        if (p && p == m_proc)
            log(QStringLiteral("Backend started (node server.js --global, PID %1)")
                    .arg(QString::number(p->processId())));
    });

    // Tie the backend's life to the launcher: PR_SET_PDEATHSIG makes the kernel
    // SIGTERM the child when this process dies - including a SIGKILL, where no
    // cleanup handler runs and the child would otherwise be orphaned. macOS and
    // Windows have no equivalent here (see the README platform differences).
#ifdef Q_OS_LINUX
    m_proc->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
#endif

    // A failed start (FailedToStart) is reported asynchronously through
    // errorOccurred (see onProcError); checking error() right here is not reliable
    m_proc->start();
    m_nodeAlive = true;
    m_startTimer.start(kStartTimeoutMs);
}

void Backend::onProcReadyRead()
{
    auto *p = qobject_cast<QProcess *>(sender());
    if (!p || p != m_proc)
        return;
    m_lineBuf += p->readAllStandardOutput();
    if (m_lineBuf.size() > kMaxLineBufBytes && m_lineBuf.indexOf('\n') < 0) {
        // guard: do not let output without any line break grow without bound
        log(Util::ansiStrip(QString::fromUtf8(m_lineBuf)));
        m_lineBuf.clear();
    }
    int idx;
    while ((idx = m_lineBuf.indexOf('\n')) >= 0) {
        QByteArray raw = m_lineBuf.left(idx);
        m_lineBuf.remove(0, idx + 1);
        if (raw.endsWith('\r'))
            raw.chop(1);
        const QString line = Util::ansiStrip(QString::fromUtf8(raw));
        if (line.isEmpty())
            continue;
        log(line);
        static const QRegularExpression re("Go to: (https?://\\S+)");
        const auto m = re.match(line);
        if (m.hasMatch()) {
            m_startTimer.stop();
            setRunning(m.captured(1));
        }
    }
}

void Backend::setRunning(const QString &url)
{
    m_url = url;
    m_lastError.clear();
    m_needsNpm = false;
    setStatus(Status::Running);
}

void Backend::onProcFinished(int code, QProcess::ExitStatus)
{
    auto *p = qobject_cast<QProcess *>(sender());
    if (!p)
        return;
    if (p != m_proc) {
        p->deleteLater(); // the pointer was replaced by a newer process: only recycle this one
        return;
    }
    m_proc = nullptr;
    p->deleteLater();

    m_startTimer.stop();
    m_killTimer.stop();
    m_nodeAlive = false;

    if (m_timeoutAbort) {
        m_timeoutAbort = false;
        m_stopping = false;
        setError(QStringLiteral("Startup timed out (not ready within 90 seconds). "
                                "Check the log for what got stuck."),
                 false);
        return;
    }
    if (m_stopping) {
        finishStop();
        return;
    }
    classifyExit(code);
}

void Backend::onProcError(QProcess::ProcessError e)
{
    auto *p = qobject_cast<QProcess *>(sender());
    if (e != QProcess::FailedToStart || !p || p != m_proc)
        return;
    m_proc = nullptr;
    p->deleteLater();
    m_startTimer.stop();
    m_nodeAlive = false; // the process never started; do not make waitStopped spin
    if (m_stopping) {
        finishStop();
        return;
    }
    if (m_status != Status::Starting)
        return;
    setError(QStringLiteral("node executable not found. Please make sure Node.js (>= 20) "
                            "is installed and on PATH."),
             false);
}

void Backend::classifyExit(int code)
{
    const QString joined = m_tail.join('\n');
    if (joined.contains("already in use") || joined.contains("EADDRINUSE")) {
        setError(QStringLiteral("Port already in use: another SillyTavern instance may be "
                                "running. Stop it or change the port."),
                 false);
        return;
    }
    if (joined.contains("Cannot find module") || joined.contains("ERR_MODULE_NOT_FOUND")) {
        setError(QStringLiteral("Backend dependencies are not installed (node_modules "
                                "missing). You can use \"Install dependencies and retry\"."),
                 true);
        return;
    }
    setError(QStringLiteral("Backend process exited (exit code %1), see the log.").arg(code),
             false);
}

void Backend::stop()
{
    if (m_status == Status::Stopped || m_status == Status::Error)
        return;
    m_stopping = true;
    setStatus(Status::Stopping);

    cleanupProbe();
    m_npmTimer.stop();
    if (m_npm) {
        m_npm->kill();
    }
    if (m_proc) {
        m_proc->terminate();
        m_killTimer.start(kKillTimeoutMs);
    } else {
        finishStop();
    }
}

void Backend::finishStop()
{
    m_killTimer.stop();
    m_stopping = false;
    m_nodeAlive = false;
    m_url.clear();
    setStatus(Status::Stopped);
}

void Backend::onStartTimeout()
{
    if (!m_proc || m_status != Status::Starting)
        return;
    log(QStringLiteral("Startup timed out, terminating the backend process..."));
    m_timeoutAbort = true;
    m_proc->terminate();
    QTimer::singleShot(3000, m_proc, &QProcess::kill);
}

void Backend::onKillTimeout()
{
    if (m_proc)
        m_proc->kill();
}

void Backend::setError(const QString &msg, bool needsNpm)
{
    m_lastError = msg;
    m_needsNpm = needsNpm;
    setStatus(Status::Error);
}

// ---------- Foreign instance decisions ----------

void Backend::foreignAttach(int port)
{
    if (m_status != Status::Starting)
        return;
    const QString url = QStringLiteral("http://127.0.0.1:%1").arg(port);
    log(QStringLiteral("Connecting directly to the existing instance on port %1: %2 "
                       "(it was not started by this launcher, its log is unavailable)")
            .arg(port)
            .arg(url));
    setRunning(url);
}

void Backend::foreignTakeover()
{
    if (m_takeoverThread)
        return; // already in progress
    const int port = Util::detectPort();
    // The cleanup can block for 3 seconds, so run it on a worker thread. The
    // result is posted to qApp and guarded by a QPointer: during application
    // shutdown the callback is simply dropped when this object is already gone.
    auto *t = QThread::create([self = QPointer<Backend>(this), port] {
        const int n = Util::killForeignBackends(port);
        QMetaObject::invokeMethod(qApp, [self, n] {
            if (!self)
                return;
            self->log(QStringLiteral("Terminated %1 foreign SillyTavern process(es); "
                                     "taking over.")
                          .arg(n));
            if (self->m_status == Status::Starting)
                self->afterProbeContinue();
        });
    });
    m_takeoverThread = t;
    connect(t, &QThread::finished, this, [this, t] {
        if (m_takeoverThread == t)
            m_takeoverThread = nullptr;
        t->deleteLater();
    });
    t->start();
}

void Backend::foreignCancel()
{
    log(QStringLiteral("Startup cancelled."));
    finishStop();
}

void Backend::installDepsThenStart()
{
    if (m_status != Status::Stopped && m_status != Status::Error)
        return; // same re-entrancy guard as start(): never while running or transitioning
    m_url.clear();
    m_lastError.clear();
    m_stopping = false;
    m_timeoutAbort = false;
    m_npmTimeout = false;
    m_lineBuf.clear();
    m_tail.clear();
    setStatus(Status::Starting);
    startNpmInstall();
}