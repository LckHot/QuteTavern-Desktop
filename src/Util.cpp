// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Util.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
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

namespace {

// Directories taken from the user's login shell PATH, harvested when a command
// could not be found in the environment the launcher inherited. Launches from
// the desktop menu or a double click do not run a login shell, so everything
// set up only in shell startup files - Homebrew, nvm, hand-picked directories -
// is invisible to the process. The harvest runs at most once per session, only
// after a lookup has already failed, and its entries sit behind the inherited
// PATH, so users whose commands resolve directly are not affected at all.
QMutex &shellPathMutex()
{
    static QMutex mutex;
    return mutex;
}
QStringList g_shellPathDirs;
bool g_shellPathTried = false;

QStringList builtinRuntimeDirs()
{
    QStringList prefixes;
    const QString nodeBin = Util::nodeBinDir();
    const QString gitBin = Util::gitBinDir();
    if (QFileInfo::exists(nodeBin + QStringLiteral("/node"))
        || QFileInfo::exists(nodeBin + QStringLiteral("/node.exe")))
        prefixes << nodeBin;
    if (QFileInfo::exists(gitBin + QStringLiteral("/git.exe")))
        prefixes << gitBin;
    return prefixes;
}

QStringList harvestLoginShellPath()
{
#ifdef Q_OS_WIN
    return {}; // the session there already inherits the PATH from the registry
#else
    const QString shellEnv = qEnvironmentVariable("SHELL");
    QFileInfo shellInfo(shellEnv);
    QString program = shellInfo.isFile() && shellInfo.isExecutable()
                          ? shellEnv : QStringLiteral("/bin/bash");
    if (!QFileInfo(program).isExecutable())
        return {};

    // fish needs its own syntax to print a colon separated PATH
    const bool isFish = QFileInfo(program).fileName() == QStringLiteral("fish");
    const QString command = isFish ? QStringLiteral("string join : $PATH")
                                   : QStringLiteral("printf %s \"$PATH\"");

    QProcess shell;
    shell.setProgram(program);
    shell.setArguments({QStringLiteral("-l"), QStringLiteral("-i"),
                        QStringLiteral("-c"), command});
    shell.setProcessChannelMode(QProcess::SeparateChannels);
    shell.setStandardInputFile(QProcess::nullDevice());
    shell.start();
    if (!shell.waitForStarted(1000))
        return {};
    // startup files can do anything - never let them stall the launch
    if (!shell.waitForFinished(3000)) {
        shell.kill();
        shell.waitForFinished(1000);
        return {};
    }
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0)
        return {};

    // PATH itself is one line; startup files may print noise around it
    const QStringList lines = QString::fromLocal8Bit(shell.readAllStandardOutput())
                                  .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty())
        return {};

    QStringList dirs;
    const auto entries = lines.last().split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        // keep existing absolute directories only - anything else is noise
        if (!entry.startsWith(QLatin1Char('/')))
            continue;
        if (!QDir(entry).exists() || dirs.contains(entry))
            continue;
        if (dirs.size() >= 128) // pathological PATHs do not grow without bound
            break;
        dirs << entry;
    }
    return dirs;
#endif
}

const QStringList &shellPathDirs()
{
    QMutexLocker lock(&shellPathMutex());
    if (!g_shellPathTried) {
        g_shellPathTried = true;
        g_shellPathDirs = harvestLoginShellPath();
    }
    return g_shellPathDirs;
}

// The child process PATH must never trigger the harvest on its own
QStringList shellPathDirsIfKnown()
{
    QMutexLocker lock(&shellPathMutex());
    return g_shellPathDirs;
}

} // namespace

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
    // SillyTavern resolves its global data directory with env-paths
    // (suffix: ''), so these paths mirror that library exactly:
    //   Windows  %LOCALAPPDATA%\SillyTavern\Data   (never the roaming %APPDATA%)
    //   macOS    ~/Library/Application Support/SillyTavern
    //   Linux    $XDG_DATA_HOME/SillyTavern, default ~/.local/share/SillyTavern
#if defined(Q_OS_WIN)
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) // env-paths falls back to the home directory too
        base = QDir::homePath() + QStringLiteral("/AppData/Local");
    return base + QStringLiteral("/SillyTavern/Data");
#elif defined(Q_OS_MAC)
    return QDir::homePath() + QStringLiteral("/Library/Application Support/SillyTavern");
#else
    QString base = qEnvironmentVariable("XDG_DATA_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/share");
    return base + QStringLiteral("/SillyTavern");
#endif
}

int readConfigPort(const QString &configYamlPath)
{
    QFile f(configYamlPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine());
        if (line.startsWith(QStringLiteral("port:"))) {
            bool ok = false;
            const int port = line.mid(5).trimmed().toInt(&ok);
            if (ok && port > 0 && port < 65536)
                return port;
        }
    }
    return 0;
}

int detectPort()
{
    const int port = readConfigPort(stDataDir() + QStringLiteral("/config.yaml"));
    return port > 0 ? port : 8000;
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
    // Resolve the program to an absolute path first: QProcess locates bare
    // program names through the PATH of the *parent* process, so the login
    // shell PATH and the built-in components that exist only in the child
    // environment we pass would otherwise never be used.
    QString prog = findCommand(program);
    if (prog.isEmpty())
        prog = program; // let QProcess report the failure
    QStringList realArgs = args;
#ifdef Q_OS_WIN
    // batch scripts have to be started through cmd
    if (prog.endsWith(QStringLiteral(".cmd"), Qt::CaseInsensitive)
        || prog.endsWith(QStringLiteral(".bat"), Qt::CaseInsensitive)) {
        realArgs = QStringList{QStringLiteral("/c"), prog} + args;
        prog = QStringLiteral("cmd");
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
    auto search = [&candidates](const QStringList &dirs) -> QString {
        for (const QString &dir : dirs) {
            for (const QString &c : candidates) {
                const QString full = dir + QStringLiteral("/") + c;
                if (QFileInfo::exists(full) && QFileInfo(full).isExecutable())
                    return full;
            }
        }
        return QString();
    };

    // 1) the environment the launcher itself inherited
    const QStringList inherited = QProcessEnvironment::systemEnvironment()
                                      .value(QStringLiteral("PATH"))
                                      .split(QDir::listSeparator(), Qt::SkipEmptyParts);
    const QString direct = search(inherited);
    if (!direct.isEmpty())
        return direct;

    // 2) the login shell's PATH - consulted only after 1) failed, so users
    //    whose commands resolve directly are completely unaffected
    const QString fromShell = search(shellPathDirs());
    if (!fromShell.isEmpty())
        return fromShell;

    // 3) the built-in downloaded components
    return search(builtinRuntimeDirs());
}

QProcessEnvironment Util::commandEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // the shell directories are appended only once they are known (the harvest
    // itself must never be triggered from here - spawning processes would pay
    // for it on every call)
    QStringList prefixes = shellPathDirsIfKnown();
    prefixes << builtinRuntimeDirs();
    if (!prefixes.isEmpty()) {
        // Everything here is a fallback: appended behind the inherited PATH so
        // that system components always win, and without duplicating it
        const QString old = env.value(QStringLiteral("PATH"));
        const QStringList seen = old.split(QDir::listSeparator(), Qt::SkipEmptyParts);
        QStringList fresh;
        for (const QString &dir : prefixes)
            if (!seen.contains(dir) && !fresh.contains(dir))
                fresh << dir;
        if (!fresh.isEmpty())
            env.insert(QStringLiteral("PATH"),
                       old + QDir::listSeparator() + fresh.join(QDir::listSeparator()));
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