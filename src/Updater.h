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