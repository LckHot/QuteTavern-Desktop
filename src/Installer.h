// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QString>

#include <functional>
#include <optional>

namespace Installer {

// Install SillyTavern from GitHub into targetParent/SillyTavern:
// git clone of the release branch -> checkout the latest tag -> npm install.
// Blocking; must run on a worker thread. Returns the installation directory on
// success.
std::optional<QString> install(const QString &targetParent,
                               const std::function<void(const QString &)> &onPhase,
                               const std::function<void(const QString &)> &log,
                               QString *err);

} // namespace Installer