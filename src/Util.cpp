// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Util.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QThread>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Util {

RootCheck validateRoot(const QString &path)
{
    RootCheck c;
    const QDir d(path.trimmed());
    if (path.trimmed().isEmpty()) {
        c.error = "Please enter the SillyTavern installation path";
        return c;
    }
    if (!d.exists()) {
        c.error = "Path does not exist or is not a directory";
        return c;
    }
    if (!QFileInfo::exists(d.absoluteFilePath("server.js"))) {
        c.error = "No server.js in this directory - is this a SillyTavern installation?";
        return c;
    }
    const QString pkgPath = d.absoluteFilePath("package.json");
    QFile f(pkgPath);
    if (!f.open(QIODevice::ReadOnly) || !QFileInfo::exists(pkgPath)) {
        c.error = "No package.json in this directory - is this a SillyTavern installation?";
        return c;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        c.error = "package.json could not be parsed";
        return c;
    }
    const auto obj = doc.object();
    if (obj.value("name").toString() != QStringLiteral("sillytavern")) {
        c.error = "package.json name is not \"sillytavern\" - please check the directory";
        return c;
    }
    c.ok = true;
    c.version = obj.value("version").toString();
    c.isGitRepo = QFileInfo::exists(d.absoluteFilePath(".git"));
    return c;
}

QString stDataDir()
{
#if defined(Q_OS_WIN)
    return qEnvironmentVariable("APPDATA") + "/SillyTavern";
#elif defined(Q_OS_MAC)
    return QDir::homePath() + "/Library/Application Support/SillyTavern";
#else
    return QDir::homePath() + "/.local/share/SillyTavern";
#endif
}

int detectPort()
{
    QFile f(stDataDir() + "/config.yaml");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine());
            if (line.startsWith(QStringLiteral("port:"))) {
                bool ok = false;
                const int port = line.mid(5).trimmed().toInt(&ok);
                if (ok && port > 0 && port < 65536)
                    return port;
            }
        }
    }
    return 8000;
}

QString ansiStrip(const QString &s)
{
    QString out;
    out.reserve(s.size());
    bool inEsc = false; // inside an ESC[ sequence
    for (const QChar ch : s) {
        if (inEsc) {
            if (ch.isLetter())
                inEsc = false;
        } else if (ch == QChar(u'\x1b')) {
            inEsc = true;
        } else {
            out.append(ch);
        }
    }
    return out;
}

void openPath(const QString &path)
{
    QDir().mkpath(path);
#ifdef Q_OS_WIN
    QProcess::startDetached("explorer", {QDir::toNativeSeparators(path)});
#elif defined(Q_OS_MAC)
    QProcess::startDetached("open", {path});
#else
    QProcess::startDetached("xdg-open", {path});
#endif
}

#if defined(Q_OS_LINUX)
namespace {

// Inodes of the sockets listening on the given port, taken from
// /proc/net/tcp{,6} (an empty set means "no filtering possible")
QSet<QString> listeningInodes(int port)
{
    QSet<QString> inodes;
    const QString needle =
        QStringLiteral(":%1").arg(port, 4, 16, QLatin1Char('0')).toUpper();
    const QString paths[] = {QStringLiteral("/proc/net/tcp"), QStringLiteral("/proc/net/tcp6")};
    for (const QString &path : paths) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        f.readLine(); // header
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.isEmpty())
                continue;
            // sl local_address rem_address st tx:rx tr:when retrnsmt uid timeout inode
            const QStringList cols =
                line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (cols.size() < 10)
                continue;
            if (!cols.at(1).endsWith(needle))
                continue;
            if (cols.at(3) != QStringLiteral("0A")) // TCP_LISTEN
                continue;
            inodes.insert(cols.at(9));
        }
    }
    return inodes;
}

// Does the process hold one of the listening sockets above?
bool holdsAnyInode(qint64 pid, const QSet<QString> &inodes)
{
    if (inodes.isEmpty())
        return false;
    QDir fdDir(QStringLiteral("/proc/%1/fd").arg(pid));
    // No entry type filter: these fds are dangling symlinks to sockets and
    // would be dropped by a file/dir filter
    const auto entries = fdDir.entryList(QDir::NoDotAndDotDot);
    for (const QString &e : entries) {
        const QString target = QFile::symLinkTarget(fdDir.absoluteFilePath(e));
        if (!target.startsWith(QStringLiteral("socket:[")))
            continue;
        if (inodes.contains(target.mid(8, target.size() - 9)))
            return true;
    }
    return false;
}

} // namespace
#endif

QList<qint64> findStPids(int port)
{
    QList<qint64> pids;
#if defined(Q_OS_LINUX)
    QDir proc("/proc");
    const auto entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : entries) {
        bool ok = false;
        const qint64 pid = name.toLongLong(&ok);
        if (!ok)
            continue;
        QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray raw = f.readAll();
        const QList<QByteArray> args = raw.split('\0');
        if (args.isEmpty() || args.first().isEmpty())
            continue;
        const QString exe = QString::fromUtf8(args.first());
        const QString base = exe.section('/', -1);
        if (base != QStringLiteral("node"))
            continue;
        bool hasServerJs = false;
        for (int i = 1; i < args.size(); ++i)
            if (args.at(i) == "server.js")
                hasServerJs = true;
        if (hasServerJs)
            pids.append(pid);
    }
    // Port cross-check: scanning the command line cannot tell instances on
    // other ports or other checkouts apart, so only narrow the candidates down
    // when the processes listening on the target port were identified
    const QSet<QString> inodes = listeningInodes(port);
    if (!inodes.isEmpty() && !pids.isEmpty()) {
        QList<qint64> matched;
        for (qint64 pid : pids)
            if (holdsAnyInode(pid, inodes))
                matched.append(pid);
        if (!matched.isEmpty())
            pids = matched;
    }
#elif defined(Q_OS_MAC)
    QProcess pgrep;
    pgrep.start("pgrep", {"-fl", "server.js"});
    pgrep.waitForFinished(3000);
    const auto lines = QString::fromUtf8(pgrep.readAllStandardOutput()).split('\n');
    for (const QString &line : lines) {
        const QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 2)
            continue;
        if (!line.contains("node") || !line.contains("server.js"))
            continue;
        bool ok = false;
        const qint64 pid = parts.first().toLongLong(&ok);
        if (ok)
            pids.append(pid);
    }
#else
    // Windows: find the PID listening on the port via netstat
    QProcess netstat;
    netstat.start("netstat", {"-ano", "-p", "tcp"});
    netstat.waitForFinished(5000);
    const auto lines = QString::fromUtf8(netstat.readAllStandardOutput()).split('\n');
    const QString needle = QStringLiteral(":%1 ").arg(port);
    for (const QString &line : lines) {
        if (!line.contains(needle) || !line.contains("LISTENING", Qt::CaseInsensitive))
            continue;
        const QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;
        bool ok = false;
        const qint64 pid = parts.last().toLongLong(&ok);
        if (ok && pid > 0)
            pids.append(pid);
    }
#endif
    return pids;
}

int killForeignBackends(int port)
{
    QList<qint64> pids = findStPids(port);
    if (pids.isEmpty())
        return 0;

    int signalled = 0; // processes we actually sent a termination request to
    for (qint64 pid : pids) {
#ifdef Q_OS_WIN
        if (QProcess::execute("taskkill", {"/PID", QString::number(pid), "/F"}) == 0)
            ++signalled;
        else
            qWarning("taskkill failed for PID %lld (already gone or not permitted)",
                     static_cast<long long>(pid));
#else
        if (kill(pid_t(pid), SIGTERM) == 0)
            ++signalled;
        else
            qWarning("SIGTERM failed for PID %lld (already gone or not permitted)",
                     static_cast<long long>(pid));
#endif
    }

#ifdef Q_OS_UNIX
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 3000;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (findStPids(port).isEmpty())
            break;
        QThread::msleep(100);
    }
    for (qint64 pid : findStPids(port))
        kill(pid_t(pid), SIGKILL);
#endif
    return signalled;
}

std::optional<int> runStreaming(const QString &program,
                                const QStringList &args,
                                const QString &workDir,
                                qint64 timeoutMs,
                                const std::function<void(const QString &)> &onLine,
                                QString *errMsg)
{
    QString prog = program;
    QStringList realArgs = args;
#ifdef Q_OS_WIN
    // npm is a batch script on Windows and has to be started through cmd
    if (prog == "npm") {
        prog = "cmd";
        realArgs = QStringList{"/c", "npm"} + args;
    }
#endif

    QProcess proc;
    proc.setProgram(prog);
    proc.setArguments(realArgs);
    if (!workDir.isEmpty())
        proc.setWorkingDirectory(workDir);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.setProcessEnvironment(Util::commandEnv());

    proc.start();
    if (!proc.waitForStarted(5000)) {
        if (errMsg)
            *errMsg = QStringLiteral("Could not start %1: %2").arg(prog, proc.errorString());
        return std::nullopt;
    }

    auto emitChunk = [&onLine](const QByteArray &chunk) {
        for (const QByteArray &line : chunk.split('\n')) {
            QByteArray cleaned = line;
            if (cleaned.endsWith('\r'))
                cleaned.chop(1);
            if (!cleaned.isEmpty())
                onLine(QString::fromUtf8(cleaned));
        }
    };

    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (true) {
        if (proc.waitForReadyRead(200))
            emitChunk(proc.readAllStandardOutput());
        if (proc.state() != QProcess::Running)
            break;
        if (QDateTime::currentMSecsSinceEpoch() >= deadline) {
            proc.kill();
            proc.waitForFinished(3000);
            if (errMsg)
                *errMsg = QStringLiteral("%1 was killed after timing out").arg(prog);
            return std::nullopt;
        }
    }
    proc.waitForFinished(3000);
    emitChunk(proc.readAllStandardOutput()); // final chunk before exit, often the error
    return proc.exitCode();
}

} // namespace Util

// ---------- Built-in runtime components ----------

QString Util::runtimeDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/runtime");
}

QString Util::nodeBinDir()
{
#ifdef Q_OS_WIN
    return runtimeDir() + QStringLiteral("/node-dist");
#else
    return runtimeDir() + QStringLiteral("/node-dist/bin");
#endif
}

QString Util::gitBinDir()
{
    return runtimeDir() + QStringLiteral("/mingit/cmd");
}

QString Util::findCommand(const QString &name)
{
    QStringList candidates;
#ifdef Q_OS_WIN
    candidates << name + QStringLiteral(".exe");
    if (!name.endsWith(QStringLiteral(".exe")))
        candidates << name + QStringLiteral(".cmd") << name + QStringLiteral(".bat");
#else
    candidates << name;
#endif
    // commandEnv PATH: system directories first, built-in components last
    const QStringList prefixes = commandEnv()
                                     .value(QStringLiteral("PATH"))
                                     .split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString &dir : prefixes) {
        for (const QString &c : candidates) {
            const QString full = dir + QStringLiteral("/") + c;
            if (QFileInfo::exists(full) && QFileInfo(full).isExecutable())
                return full;
        }
    }
    return QString();
}

QProcessEnvironment Util::commandEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList prefixes;
    const QString nodeBin = nodeBinDir();
    const QString gitBin = gitBinDir();
    if (QFileInfo::exists(nodeBin + QStringLiteral("/node"))
        || QFileInfo::exists(nodeBin + QStringLiteral("/node.exe")))
        prefixes << nodeBin;
    if (QFileInfo::exists(gitBin + QStringLiteral("/git.exe")))
        prefixes << gitBin;
    if (!prefixes.isEmpty()) {
        // Built-in directories are a fallback: appended to PATH so that system
        // components always win
        const QString old = env.value(QStringLiteral("PATH"));
        env.insert(QStringLiteral("PATH"),
                   old + QDir::listSeparator() + prefixes.join(QDir::listSeparator()));
    }
    return env;
}

bool Util::extractArchive(const QString &archive,
                          const QString &destDir,
                          int stripComponents,
                          QString *err)
{
    QDir().mkpath(destDir);
    QProcess tar;
    tar.setProgram(QStringLiteral("tar"));
    QStringList args{QStringLiteral("-xf"), archive, QStringLiteral("-C"), destDir};
    if (stripComponents > 0)
        args << QStringLiteral("--strip-components=%1").arg(stripComponents);
    tar.setArguments(args);
    tar.start();
    if (!tar.waitForStarted(5000)) {
        if (err)
            *err = QStringLiteral("Could not start tar (it ships with Windows 10 1803+, "
                                  "macOS and Linux)");
        return false;
    }
    if (!tar.waitForFinished(180'000) || tar.exitStatus() != QProcess::NormalExit
        || tar.exitCode() != 0) {
        if (err)
            *err = QStringLiteral("Extraction failed (exit code %1)").arg(tar.exitCode());
        return false;
    }
    return true;
}