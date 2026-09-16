// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

// Platform helpers and general utilities
namespace Util {

struct RootCheck {
    bool ok = false;
    QString version;
    bool isGitRepo = false;
    QString error;
};

// Validate a SillyTavern install directory
// (server.js + package.json with name == sillytavern)
RootCheck validateRoot(const QString &path);

// SillyTavern global data directory (per platform conventions)
QString stDataDir();

// Read the top-level "port:" entry of the global config.yaml, default 8000
int detectPort();

QString ansiStrip(const QString &s);

// Open a directory with the system handler (xdg-open / open / explorer)
void openPath(const QString &path);

// Find foreign SillyTavern processes (node server.js). On Linux the candidates
// are cross-checked against the listening port when possible, so unrelated
// instances on the same machine are not killed; if no listening socket can be
// identified, all candidates are returned.
QList<qint64> findStPids(int port);

// Terminate foreign instances (SIGTERM -> 3s -> SIGKILL / taskkill on Windows).
// Returns how many processes were actually signalled.
int killForeignBackends(int port);

// Run a command and stream its output line by line (stdout+stderr merged).
// Returns the exit code, or nullopt when the process could not be started or
// had to be killed on timeout (errMsg explains why).
std::optional<int> runStreaming(const QString &program,
                                const QStringList &args,
                                const QString &workDir,
                                qint64 timeoutMs,
                                const std::function<void(const QString &)> &onLine,
                                QString *errMsg = nullptr);

} // namespace Util

// ---------- Built-in runtime components (downloaded node / MinGit) ----------
namespace Util {

// Component root directory: <AppData>/runtime
QString runtimeDir();
// bin directory of the downloaded node distribution (the directory itself on Windows)
QString nodeBinDir();
// git executable directory of MinGit (Windows)
QString gitBinDir();
// Look up a command. Resolution order: the PATH the launcher inherited, then
// (only if that failed) the PATH of the user's login shell, then the built-in
// downloaded components. Returns an empty string when nothing was found.
QString findCommand(const QString &name);
// Environment for all QProcess instances: the login shell PATH (once known)
// and the built-in directories are appended to the inherited PATH, in that
// order, so that already-resolvable components keep their priority.
QProcessEnvironment commandEnv();
// Extract an archive with the system tar (.tar.gz/.zip; bsdtar ships with
// Windows 10+). With stripComponents > 0 the top-level directory of the archive
// is stripped - the official node archives carry one node-vX-<os>-<arch>/.
bool extractArchive(const QString &archive,
                    const QString &destDir,
                    int stripComponents,
                    QString *err);

} // namespace Util