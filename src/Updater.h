// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QString>

#include <functional>

namespace Updater {

struct CheckResult {
    bool upToDate = false;
    bool isGitRepo = true;
    QString current;
    QString latest;
    QString error;
};

// Newest plain-numeric version tag of a `git tag --list --sort=-v:refname`
// listing (pre-releases and other special tags are skipped); empty when
// nothing matches. Shared by the update check and the installer so both apply
// the same policy; exposed for unit tests.
QString pickLatestVersionTag(const QStringList &tagLines);

// Latest plain-numeric version tag of the checkout in stRoot, empty when the
// repository has none. Blocking; must run on a worker thread.
QString latestVersionTag(const QString &stRoot);

// Check for updates (git fetch --tags). Blocking; must run on a worker thread.
CheckResult check(const QString &stRoot, const std::function<void(const QString &)> &log);

// Perform the update: stop the backend, dirty check, checkout tag, npm install.
// stopBackend is called first and has to block until the backend has stopped.
// Blocking; must run on a worker thread.
bool perform(const QString &stRoot,
             const QString &tag,
             const std::function<void(const QString &)> &log,
             const std::function<void()> &stopBackend,
             QString *err);

} // namespace Updater