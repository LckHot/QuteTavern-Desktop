# QuteTavern

> Full design document: [DESIGN.md](DESIGN.md).

QuteTavern (a play on *Qt* and *cute tavern*) is an unofficial, community-built
desktop launcher for [SillyTavern](https://github.com/SillyTavern/SillyTavern).
Like a barkeep who keeps the tavern running, it takes care of the boring parts:
it manages the SillyTavern backend as a child process and shows the front end in
a separate window rendered by Qt WebEngine (Chromium), on Linux, Windows and
macOS alike.


## Architecture

```
+-- Management window (Qt Widgets, always present) --------------+
|  Bind / change directory . Install a new copy from GitHub      |
|  Start/stop . Status . Live backend log panel                  |
|  Check for updates . Data directory . Preferences              |
+----------------------------------------------------------------+
        | state driven (opened when entering Running, closed when leaving it)
        v
+-- ST window (Qt WebEngine / Chromium) --------------------------+
|  SillyTavern front end                                          |
+-----------------------------------------------------------------+
        | 127.0.0.1:8000
        v
   node server.js --global (QProcess, SIGTERM -> 5s -> SIGKILL; Windows: kill immediately)
   data/config -> global data directory (Linux: ~/.local/share/SillyTavern)
```

## Features

- **Management window (native Qt Widgets)**: starts instantly; native title bar
  and global menu integration on KDE
- **Install from GitHub**: pick a parent directory, clone the release branch,
  check out the latest tag, run npm install, bind automatically
- **Separate ST window (Chromium engine)**: closing it does not stop the
  backend; the management window can reopen it. Closing the management window
  stops the backend gracefully and quits
- **Live log panel**: backend output plus all npm/git output, streaming, last
  2000 lines kept
- **Foreign instance handling**: when the port is already taken, the launcher
  offers to terminate and take over, to connect directly, or to cancel
- **Automatic dependency bootstrap**, **attach to an existing instance**,
  **error classification**, **update check** (`fetch --tags`, checkout of the
  latest tag, `npm install`, dirty working tree protection)
- **Graceful shutdown**: SIGTERM -> up to 5s -> SIGKILL; on Windows the backend
  is hard-killed immediately (console processes have no graceful signal)
- **Single instance**: starting the app again focuses the existing window
- **Cross-platform**: Linux / Windows / macOS (paths, file managers and process
  termination are adapted per platform)

## Building

Linux dependencies (Fedora):

```bash
sudo dnf install cmake gcc-c++ qt6-qtbase-devel qt6-qtwebengine-devel qt6-qtdeclarative-devel
```

Linux dependencies (Debian/Ubuntu):

```bash
sudo apt install cmake g++ qt6-base-dev qt6-webengine-dev qt6-declarative-dev
```

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/qutetavern
```

On Linux, `./install.sh` builds the binary and installs a launcher script,
icons and a `.desktop` entry into the user's home directory (no root needed).

macOS and Windows builds are produced by the GitHub Actions workflow in
[.github/workflows/build.yml](.github/workflows/build.yml), which also publishes
ready-to-run packages:

| Platform | Package |
| --- | --- |
| Linux | `QuteTavern-<version>-x86_64.AppImage` |
| Debian / Ubuntu | `QuteTavern-<version>-amd64.deb` |
| Fedora / RHEL / openSUSE | `QuteTavern-<version>-x86_64.rpm` |
| Windows | `QuteTavern-<version>-Setup.exe` (NSIS installer) |
| macOS | `QuteTavern-<version>-universal.pkg` (Intel + Apple silicon) |

Packages are built when a `v*` tag is pushed - which also publishes the release
if every platform passes - and on pull requests, so one release never builds the
same tree twice. Plain pushes to main do not start a build.

The AppImage carries its own current Qt. The `.deb` and the `.rpm` come from one
build on Ubuntu 22.04, linked against its Qt 6.2 (the oldest Qt 6 with a complete
WebEngine), and declare the distribution's Qt as a dependency instead of
bundling it - so Qt updates arrive through the system package manager.

## Distribution support

| Package | Requirements | Runs on |
| --- | --- | --- |
| `.deb` | Qt 6.2+, glibc 2.35+ | Ubuntu 22.04 LTS and newer, Debian 12 and newer |
| `.rpm` | Qt 6.2+, glibc 2.35+ | Fedora 36 and newer, RHEL 10 and newer, openSUSE Leap 15.6 and newer |
| `.AppImage` | glibc 2.35+, no system Qt | any distribution meeting the glibc floor (Fedora 36+, Ubuntu 22.04+, Debian 12+, …) |
| Windows installer | - | Windows 10 1803 or newer |
| macOS package | - | macOS 12 or newer, Intel and Apple silicon |

Those floors are not assumptions: CI builds and installs the `.deb` on Ubuntu
22.04, and installs, starts and removes the `.rpm` inside a Fedora container, so
the oldest supported release is a tested configuration. RHEL 9 is deliberately
not covered - it ships glibc 2.34, below what the build requires.

## Window semantics

| Window | Closing it |
| --- | --- |
| ST window | The backend keeps running; "Open ST window" in the management window brings it back |
| Management window | Stops the backend gracefully and quits the application |

When the backend crashes the ST window closes automatically and the error plus
the log are shown in the management window.

## File locations

| What | Linux | Windows | macOS |
| --- | --- | --- | --- |
| Launcher configuration | `~/.config/QuteTavern/config.json` | `%LOCALAPPDATA%\QuteTavern\config.json` | `~/Library/Preferences/QuteTavern/config.json` |
| Downloaded components (Node.js, MinGit) | `~/.local/share/QuteTavern/runtime` | `%APPDATA%\QuteTavern\runtime` | `~/Library/Application Support/QuteTavern/runtime` |
| SillyTavern data (global mode) | `~/.local/share/SillyTavern` | `%LOCALAPPDATA%\SillyTavern\Data` | `~/Library/Application Support/SillyTavern` |

## Data model (important)

The launcher runs the backend in its **global mode**
(`node server.js --global`), so data lives in `~/.local/share/SillyTavern`
(or `$XDG_DATA_HOME/SillyTavern`; Windows: `%LOCALAPPDATA%\SillyTavern\Data`;
macOS: `~/Library/Application Support/SillyTavern`) - the same locations
SillyTavern itself uses. This is a **separate data world**
from running `node server.js` (standalone mode) inside the repository.

## Known platform differences

- Windows: a console process cannot be signalled gracefully, so the backend is
  hard-killed immediately (backend statistics may not be flushed); leftover
  foreign instances are stopped with `taskkill /F`
- Linux: the backend is killed together with a force-killed launcher
  (`PR_SET_PDEATHSIG` -> SIGTERM); macOS and Windows have no equivalent, so
  force-killing the launcher leaves the backend running there

## Runtime requirements

The launcher needs `node` (>= 20, the floor SillyTavern declares in its
`engines` field; the portable download always fetches the latest release) and
`git` at runtime. Commands are resolved
from the environment the launcher was started with; if that fails, the PATH of
your login shell is consulted once (so installations set up in shell startup
files - Homebrew, nvm, custom directories - work even when the launcher is
started from the desktop menu). Nothing is written to your system, and commands
that already resolve keep their priority. Missing components can
be downloaded as portable copies into the launcher's application data directory
(the latest Node.js release, and MinGit on Windows); they never modify the system
environment - system components always take priority. On macOS git comes with
the Xcode command line tools (`xcode-select --install`), on Linux it has to be
installed from the distribution.

## License

The QuteTavern is free software licensed under the
**GNU Affero General Public License v3.0** - see [LICENSE](LICENSE). This is the
same license the SillyTavern project uses, from which the application icon
originates; every file carries an `SPDX-License-Identifier` tag.

- Qt 6 (Widgets, WebEngine) is used under LGPL-3.0 and shipped unmodified; the
  required notices and the replacement options are listed in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- SillyTavern itself is only started as a child process and is never modified.
- This is an unofficial community project, not affiliated with or endorsed by
  the SillyTavern project; the name is used to describe what the launcher
  starts.