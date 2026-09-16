// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Installer.h"

#include "Updater.h"
#include "Util.h"

#include <QDir>

namespace Installer {

std::optional<QString> install(const QString &targetParent,
                               const std::function<void(const QString &)> &onPhase,
                               const std::function<void(const QString &)> &log,
                               QString *err)
{
    const QDir parent(targetParent);
    if (!parent.exists()) {
        if (err)
            *err = QStringLiteral("Directory does not exist: %1").arg(targetParent);
        return std::nullopt;
    }
    const QString dir = parent.absoluteFilePath("SillyTavern");
    if (QDir(dir).exists()) {
        if (err)
            *err = QStringLiteral("%1 already exists. Delete it or pick another directory "
                                  "to reinstall.")
                       .arg(dir);
        return std::nullopt;
    }

    // 1. Clone the release branch (a full clone brings all tags along)
    onPhase(QStringLiteral("Cloning the release branch from GitHub..."));
    log(QStringLiteral("[install] git clone -> %1").arg(dir));
    const auto clone = Util::runStreaming(
        "git", {"clone", "--branch", "release",
                "https://github.com/SillyTavern/SillyTavern.git", dir},
        QString(), 1'800'000, log);
    if (!clone || *clone != 0) {
        if (err)
            *err = QStringLiteral("git clone failed, see the launcher log.");
        return std::nullopt;
    }

    // 2. Check out the latest tag - the same policy as the update check: only
    //    plain numeric version tags count, pre-releases are skipped
    onPhase(QStringLiteral("Checking out the latest release tag..."));
    const QString latest = Updater::latestVersionTag(dir);
    if (latest.isEmpty()) {
        if (err)
            *err = QStringLiteral("The repository has no version tags.");
        return std::nullopt;
    }
    log(QStringLiteral("[install] checking out latest tag v%1").arg(latest));
    const auto checkout = Util::runStreaming("git", {"checkout", latest}, dir, 60'000, log);
    if (!checkout || *checkout != 0) {
        if (err)
            *err = QStringLiteral("git checkout %1 failed.").arg(latest);
        return std::nullopt;
    }

    // 3. Install dependencies (same flags as start.sh)
    onPhase(QStringLiteral("Installing dependencies (npm install)..."));
    const auto npm = Util::runStreaming("npm", Util::npmInstallArgs(), dir, 600'000, log);
    if (!npm || *npm != 0) {
        if (err)
            *err = QStringLiteral("npm install failed, see the launcher log.");
        return std::nullopt;
    }

    onPhase(QStringLiteral("Installation finished (v%1)").arg(latest));
    log(QStringLiteral("[install] finished: %1 (v%2)").arg(dir, latest));
    return dir;
}

} // namespace Installer