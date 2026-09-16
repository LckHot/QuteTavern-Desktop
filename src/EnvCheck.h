// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

class QWidget;

namespace EnvCheck {

// Startup environment check: when node/git are missing, the user can choose
// between quitting and downloading portable copies into the built-in
// directory. Returns false when the user chose to quit, which means the
// application should terminate.
bool ensureEnvironment(QWidget *parent);

} // namespace EnvCheck