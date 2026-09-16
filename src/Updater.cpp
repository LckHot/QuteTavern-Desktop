// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Updater.h"

#include "Util.h"

#include <QDir>
#include <QRegularExpression>

namespace Updater {

namespace {

struct Captured {
    std::optional<int> code;
    QString out;
    QString err;
};

// Version tags must be plain numbers (e.g. 1.12.14); pre-release and other
// special tags are skipped so that the launcher never "updates" to one of them
const QRegularExpression &versionTagRe()
{
    static const QRegularExpression re(QStringLiteral("^[0-9][0-9.]*$"));
    return re;
}

// Synchronously capture the output (used for the lightweight git queries)
Captured run(const QString &prog, const QStringList &args, const QString &workDir, int timeoutMs)
{
    Captured c;
    QString err;
    c.code = Util::runStreaming(
        prog, args, workDir, timeoutMs,
        [&c](const QString &l) { c.out += l + '\n'; }, &err);
    if (!c.code)
        c.err = err;
    return c;
}

} // namespace

QString pickLatestVersionTag(const QStringList &tagLines)
{
    for (const QString &t : tagLines) {
        const QString v = t.trimmed();
        if (versionTagRe().match(v).hasMatch())
            return v;
    }
    return QString();
}

QString latestVersionTag(const QString &stRoot)
{
    const Captured tags = run(QStringLiteral("git"),
                              {QStringLiteral("tag"), QStringLiteral("--list"),
                               QStringLiteral("--sort=-v:refname")},
                              stRoot, 10'000);
    return pickLatestVersionTag(tags.out.split('\n', Qt::SkipEmptyParts));
}

CheckResult check(const QString &stRoot, const std::function<void(const QString &)> &log)
{
    CheckResult r;
    QDir root(stRoot);
    if (!root.exists(".git")) {
        r.isGitRepo = false;
        r.error = QStringLiteral("This installation is not a git repository, so updates "
                                 "cannot be checked (only git clones are supported)");
        return r;
    }

    log(QStringLiteral("Running git fetch --tags origin..."));
    Captured f = run("git", {"fetch", "--tags", "origin"}, stRoot, 60'000);
    if (!f.code || *f.code != 0) {
        r.error = QStringLiteral("git fetch failed: %1 (check your network)").arg(f.err);
        return r;
    }

    const Captured tags = run("git", {"tag", "--list", "--sort=-v:refname"}, stRoot, 10'000);
    // Take the first valid version tag in sort order (skipping pre-releases etc.)
    r.latest = pickLatestVersionTag(tags.out.split('\n', Qt::SkipEmptyParts));

    Captured cur = run("git", {"describe", "--tags", "--exact-match"}, stRoot, 10'000);
    if (!cur.code || *cur.code != 0 || cur.out.trimmed().isEmpty())
        cur = run("git", {"rev-parse", "--short", "HEAD"}, stRoot, 10'000);
    r.current = cur.out.section('\n', 0, 0).trimmed();

    r.upToDate = !r.current.isEmpty() && r.current == r.latest;
    return r;
}

bool perform(const QString &stRoot,
             const QString &tag,
             const std::function<void(const QString &)> &log,
             const std::function<void()> &stopBackend,
             QString *err)
{
    // Version tags must be plain numbers (check() filters by the same rule)
    if (!versionTagRe().match(tag).hasMatch()) {
        if (err)
            *err = QStringLiteral("Invalid version: %1").arg(tag);
        return false;
    }

    // Stop the backend first (blocks until it is gone)
    if (stopBackend)
        stopBackend();

    // Dirty check: only modifications of tracked files (untracked ones do not
    // interfere with a checkout)
    const Captured st = run("git", {"status", "--porcelain", "--untracked-files=no"}, stRoot, 10'000);
    if (!st.out.trimmed().isEmpty()) {
        if (err)
            *err = QStringLiteral("The SillyTavern working tree has local changes, update "
                                  "aborted. Commit or stash them and retry.");
        return false;
    }

    log(QStringLiteral("Checking out v%1...").arg(tag));
    const auto checkout = Util::runStreaming(
        "git", {"checkout", tag}, stRoot, 120'000, log);
    if (!checkout || *checkout != 0) {
        if (err)
            *err = QStringLiteral("git checkout %1 failed, see the launcher log.").arg(tag);
        return false;
    }

    log(QStringLiteral("Installing dependencies (npm install)..."));
    const auto npm = Util::runStreaming("npm", Util::npmInstallArgs(), stRoot, 600'000, log);
    if (!npm || *npm != 0) {
        if (err)
            *err = QStringLiteral("npm install failed, see the launcher log.");
        return false;
    }

    log(QStringLiteral("Update finished: now on v%1. Press \"Start\" in the main window "
                       "to run the new version.")
            .arg(tag));
    return true;
}

} // namespace Updater