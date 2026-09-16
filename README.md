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
   node server.js --global (QProcess, SIGTERM -> 5s -> SIGKILL)
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
- **Graceful shutdown**: SIGTERM -> up to 5s -> SIGKILL (`taskkill` on Windows)
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

Release packages are built automatically when a `v*` tag is pushed; the same
workflow also runs on pull requests to verify that all platforms build.

The AppImage carries its own current Qt. The `.deb` is built against Ubuntu
22.04's Qt 6.2 and the `.rpm` against the Qt in AlmaLinux 9 + EPEL (Qt 6.5) -
the oldest environments that still provide a complete Qt WebEngine. Both declare
the distribution's Qt as a dependency instead of bundling it, so Qt updates come
from the system package manager; CI installs and starts them in fresh
environments of exactly those distributions.

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
| SillyTavern data (global mode) | `~/.local/share/SillyTavern` | `%APPDATA%\SillyTavern` | `~/Library/Application Support/SillyTavern` |

## Data model (important)

The launcher runs the backend in its **global mode**
(`node server.js --global`), so data lives in `~/.local/share/SillyTavern`
(Windows: `%APPDATA%\SillyTavern`; macOS:
`~/Library/Application Support/SillyTavern`). This is a **separate data world**
from running `node server.js` (standalone mode) inside the repository.

## Known platform differences

- Windows: the backend is stopped with `taskkill` (no SIGTERM), so backend
  statistics may not be flushed
- macOS: there is no PDEATHSIG equivalent, so force-killing the launcher leaves
  the backend running

## Runtime requirements

The launcher needs `node` (>= 22) and `git` at runtime. Missing components can
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