# QuteTavern Design Document

Version: v2.0.0 | [Back to README](README.md)

This document records the overall design, the responsibilities of each module and
the key decisions with their rationale, for future maintenance.

## 1. Project goals

Provide a **cross-platform desktop launcher** for
[SillyTavern](https://github.com/SillyTavern/SillyTavern) (a Node.js backend
with a web front end):

- The **management window** (Qt Quick/QML) opens first; the backend child
  process is only spawned after pressing "Start"
- The SillyTavern front end is shown in a **separate browser-engine window**
  (Qt WebEngine / Chromium)
- The backend runs in **global data mode**, keeping user data completely
  separate from the code checkout
- Works out of the box: missing dependencies (Node/git) can be downloaded
  automatically as built-in fallback components

## 2. Overall architecture

One process, one toolchain (Qt 6 / C++17), one code base for all three
platforms (Linux / Windows / macOS).

```
┌─ Management window qml/Main.qml (Qt Quick, always present) ─────────────┐
│ Wizard page: bind a directory / install a new copy from GitHub          │
│ Main page: start/stop/open the ST window · status · live log panel      │
│ Dialogs: preferences / check for updates / install / environment check  │
│ AppController is the only bridge to the logic layer below (properties   │
│ and invokables); no dialog and no window chrome is built in C++         │
└──────────────┬──────────────────────────────────────────────────────────┘
               │ Backend signals (logLine / stateChanged /
               │          foreignInstanceFound)
               ▼
   Backend (QProcess async state machine, everything on the main thread)
               │ spawn: node server.js --global
               │        --browserLaunchEnabled=false
               ▼
   SillyTavern Node backend (127.0.0.1:8000)
               │ "Go to: http://…" (stdout)
               ▼
┌─ ST window qml/StWindow.qml (Window + WebEngineView) ───────────────────┐
│ Created on demand and destroyed on close, driven by the management       │
│ window's state events: entering Running → open and load the URL;         │
│ leaving Running → close                                                  │
│ User closes the ST window → backend keeps running (can be reopened)     │
└─────────────────────────────────────────────────────────────────────────┘
```

### Window semantics (the user-visible contract)

| Window | Closing behaviour |
| --- | --- |
| ST window | The backend keeps running; "Open ST window" in the management window brings it back |
| Management window | Stops the backend (SIGTERM → 5s → SIGKILL; Windows: kill immediately) and quits the application |

When the backend crashes the ST window closes automatically, and the error plus
the log are presented in the management window - **backend running ⟺ ST window
exists** (unless the user closed it), no hidden state.

The page's Fullscreen API is enabled and **answered without touching the window**:

- every request is accepted; Qt WebEngine then makes the requesting element fill
  the view — which *is* the ST window's content area. Nothing else is required.
- the launcher deliberately never switches the window into a platform fullscreen
  state: on Wayland the window system decides a window's size and position, and
  the fullscreen/maximized/normal flip is what left the window restored to a
  stale default size and the input method without a focus target.
- Escape exits the page fullscreen, armed while the page holds a fullscreen
  element (`WebEngineView.isFullScreen` is the single source of truth); focus and
  the input method are re-armed afterwards.
- a borderless *real* fullscreen stays available from the window system
  (compositor shortcut). The launcher never fights it and the page's fullscreen
  element is unaffected by it.
- only maximized geometries are kept out of Settings, and the window is seeded
  with its remembered normal size before it is first maximized, so un-maximizing
  never lands on the platform's default 640x480.

## 3. Module design

| File | Responsibility |
| --- | --- |
| `main.cpp` | QGuiApplication + QtWebEngineQuick bootstrap, single instance (QLocalServer), QML engine |
| `qml/Main.qml` (+ pages/dialogs) | The whole UI: wizard page, main page, preferences / update / install / environment check / foreign instance / message dialogs |
| `qml/StWindow.qml` | ST window (`Window` + `WebEngineView`): lifecycle, geometry, the fullscreen state machine |
| `AppController.{h,cpp}` | The only C++↔QML bridge: settings, backend and ST window lifecycle, worker threads, every action the UI can trigger |
| `EnvCheck.{h,cpp}` | Detects node/git at startup; downloader + extraction + re-check; its dialog is QML |
| `Backend.{h,cpp}` | Backend child process state machine (the core, see §4) |
| `Settings.{h,cpp}` | Launcher configuration (JSON); on Linux it falls back to the legacy configuration path |
| `Util.{h,cpp}` | Platform abstraction + general utilities (see §5); single place for command lookup |
| `Updater.{h,cpp}` | Update check / update execution (pure logic, called from a worker thread) |
| `Installer.{h,cpp}` | Install a new copy from GitHub (pure logic, called from a worker thread) |

Threading model: **all state and UI live on the main thread**; the asynchronous
signals of `QProcess`/`QNetworkAccessManager` drive the state machine. Worker
threads are only used for blocking operations (install/update/foreign instance
cleanup); their results are handed back to the main thread through
`QMetaObject::invokeMethod`. Cross-thread updates are guarded with `QPointer`:
worker callbacks are posted to `qApp` and re-check a `QPointer` before touching
the window or the backend, so they can never reach an object that was already
destroyed.

## 4. Backend state machine (the core)

States: `Stopped → Starting → Running → Stopping → Stopped`; any stage can enter
`Error`.

```
start()
  ├─ re-entrancy guard (returns while Starting/Stopping/Running)
  ├─ switch to Starting (covers every slow stage below, prevents concurrent double clicks)
  ├─ HTTP probe of 127.0.0.1:<port>   ← port read from the top-level "port:" of the global config.yaml, default 8000
  │    │alive → emit foreignInstanceFound(port) (paused, waiting for the UI decision)
  │    │        ├─ foreignTakeover(): /proc|pgrep|netstat to find the PID →
  │    │        │   SIGTERM→3s→SIGKILL (executed on a thread) → continue below
  │    │        ├─ foreignAttach(port): connect directly (no log, stdout is not ours)
  │    │        └─ foreignCancel(): back to Stopped
  │    ─no instance ↓
  ├─ dependency self-check: <st_root>/node_modules/express missing →
  │  npm install --omit=dev --ignore-scripts (same flags as the official start.sh)
  │  once it finished the state is re-checked (a pending stop request aborts the launch)
  └─ spawn: node server.js --global --browserLaunchEnabled=false [extra arguments]
       ├─ stdout line by line: strip ANSI → logLine; regex "Go to: (https?://\S+)" → Running
       ├─ 90s readiness timeout → terminate/kill → Error("startup timed out")
       └─ exit classification: already in use → port busy; Cannot find module → dependencies
                    missing (can be reinstalled); otherwise → "exit code N, see the log"

stop(): switch to Stopping → terminate() → 5s timer → kill()
        (Windows: kill() immediately; console node ignores WM_CLOSE)
        during the npm/probe stage there is no child process, the state goes straight to Stopped
```

Design notes:

- **QProcess instead of hand-rolled process management**: child reaping and
  signal delivery are handled by Qt on the main thread, so the backend is tied
  to a stable owner - a short-lived helper thread can neither orphan it nor
  kill it by exiting.
- **Foreign instances require an explicit decision**: silently attaching would
  leave the log panel with a single unexplained line; a dialog explains the
  situation and recommends taking over instead.
- **The Starting state is entered before any slow operation**: this blocks the
  concurrent npm installs / double backends that repeated clicks used to cause.
- Readiness is detected by parsing the `Go to:` line instead of hardcoding the
  port - custom ports and HTTPS are respected automatically.

## 5. Platform abstraction (Util)

| Capability | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Open a directory | xdg-open | open | explorer |
| Foreign instance discovery | /proc cmdline scan (+ listening-port cross-check) | pgrep -fl filter | netstat -ano to find the listening PID |
| Foreign instance termination | SIGTERM→3s→SIGKILL | same | taskkill /F |
| Graceful backend stop | SIGTERM (SillyTavern has cleanup hooks) | same | immediate hard kill (console processes cannot be signalled) |
| Data directory | ~/.local/share/SillyTavern (or $XDG_DATA_HOME) | ~/Library/Application Support/SillyTavern | %LOCALAPPDATA%\SillyTavern\Data |
| npm invocation | npm | npm | cmd /c npm (batch script) |
| Archive extraction | tar | tar (bsdtar) | tar (bundled since Windows 10 1803) |

**Built-in component model**: automatically downloaded Node/MinGit live in
`<AppData>/runtime/`. `Util::commandEnv()` injects PATH into **every** QProcess
(node/npm/git) - the built-in directories are **appended after** the system
PATH, so system components always win and the built-ins are only a fallback.
Deleting the runtime directory removes them completely; the system environment
is never polluted.

**Command resolution order**: inherited environment PATH → login shell PATH →
built-in components. The login shell (`$SHELL -l -i -c`, 3s timeout, fish gets
its own syntax) is consulted at most once per session and **only after the
inherited PATH failed** - desktop launches do not run a login shell, so tools
installed via Homebrew/nvm or into custom directories would otherwise be
invisible. Because the extra directories are appended and only fetched on
demand, users whose commands resolve directly are unaffected.

**One resolver for detection and execution**: every child process is started
with the **absolute path** `Util::findCommand()` returned, never with a bare
program name. QProcess resolves bare names through the PATH of the *parent*
process and ignores the environment set with `setProcessEnvironment()`, so a
bare `node` would fail exactly when the login shell PATH is what makes it
findable - detection would succeed while starting the backend failed.

## 6. Environment check and component downloads (EnvCheck)

- Detection: `Util::findCommand("node"/"git")` (the same resolution logic that
  is used at runtime).
- Sources: the newest official Node.js release (queried from
  `nodejs.org/dist/index.json` at runtime, with a pinned version as fallback;
  tar.gz for linux/mac, zip for win, selected by CPU); on Windows git comes from
  the official git-for-windows **MinGit**.
- **Strict instruction set validation**: `x86_64/amd64 → x64`,
  `arm64/aarch64 → arm64`, explicitly enumerated; unknown architectures disable
  the download and report a clear error, **never guess silently**. The dialog
  shows both the detected and the target instruction set; extraction strips the
  archive's top-level directory (`node-vX-<os>-<arch>/`) and the node executable
  is verified afterwards.
- Honest degradation per platform: on macOS git ships with the Xcode command
  line tools (suggests `xcode-select --install`); Linux has no portable git
  (points at the distribution packages; a missing git only affects
  install/update, not starting the backend).

## 7. Install / update / settings

- **Install a new copy** (available from the wizard page and the main panel):
  pick a parent directory → `git clone --branch release` (a full clone brings
  all tags along) → check out the latest tag → npm install → **bound and
  switched automatically**. An existing directory is refused so that nothing can
  be deleted by accident.
- **Check for updates**: `git fetch --tags` → `tag --list --sort=-v:refname`
  compared against `describe --tags --exact-match` (only plain numeric version
  tags are used; pre-release tags are skipped) → after confirmation: **stop the
  backend first** and wait until it exited → dirty check
  (`status --porcelain -uno`, untracked files do not block) → checkout tag →
  npm install.
- **Settings** (`<AppConfigLocation>/config.json`): `st_root`,
  `extra_backend_args`, `auto_maximize`,
  `window_state{x,y,w,h}`. Missing fields are tolerated; saving preserves the
  `window_state` that is not part of the form; on Linux a configuration file
  written by earlier launcher builds is picked up as a fallback.

## 8. Key decisions

| Decision | Rationale |
| --- | --- |
| Global data mode (`--global`) | Data is separated from the code checkout; git operations (updates) never touch user data; XDG standard locations |
| Press-to-start instead of auto start | User request; the management window is meant to be the control centre |
| Qt Quick + Qt WebEngine as a single toolchain | QML is Qt's recommended UI layer for new work (and the route the official WebEngine examples use); the logic layer stays plain C++ behind one bridge object, so the state machine and the platform code are unaffected |
| Built-in components appended to PATH instead of prepended | System components win; the user environment is not shadowed by the application directory |
| Explicit dialog for foreign instances | Silently attaching means no log and no explanation; taking over restores the full log |
| License: AGPL-3.0 | The application icon is SillyTavern's official artwork, which is AGPL-3.0; licensing the launcher the same way keeps the whole distribution under one license and matches what the community expects. Qt is only linked dynamically, so its LGPL-3.0 terms stay satisfiable (see THIRD_PARTY_NOTICES.md) |

### Why these choices hold up

Each decision below closes a failure mode that is easy to run into with a GUI
launcher:

1. **Blocking work never runs on the UI thread.** Installs, updates and foreign
   instance cleanup run on worker threads; everything else is driven by
   asynchronous signals. A frozen management window is a dead end for the user.
2. **The backend has a stable owner.** QProcess keeps the child tied to the main
   thread, so no short-lived helper thread can orphan the backend or take it
   down by exiting.
3. **The window is decorated by the platform.** Native title bars and global
   menus come from the system (Qt serves them correctly on Wayland/KWin) instead
   of the application fighting the compositor.
4. **Chromium is the rendering engine** (Qt WebEngine), so no engine specific
   workarounds are needed for a stable window and clean GPU teardown.
5. **Leftover instances stay explainable.** An instance the launcher did not
   start is reported and can be taken over, rather than silently attaching
   without any log output.

## 9. Cross-platform status

| Capability | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Build | verified locally + CI | CI (unsigned package) | CI (unsigned package) |
| Runtime verification | verified locally | code ready, not yet tested on hardware | code ready, not yet tested on hardware |
| Management window / ST window / log / settings | yes | yes | yes |
| Install a new copy / check for updates | yes | yes (git needs the CLT) | yes |
| Component download | node yes / git via the distribution | node yes / git via the CLT | node + MinGit yes |
| Known limitations | - | no PDEATHSIG: force-killing the launcher leaves the backend running | immediate hard kill (no graceful console signal); Job Object kills the backend with a force-killed launcher |

Packages for all three platforms are produced by
`.github/workflows/build.yml`; the artifact names are listed in the README.

## 10. Build and file locations quick reference

```
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./install.sh          # Linux: build + user-level installation of the launcher, icon and .desktop entry

config   ~/.config/QuteTavern/config.json                (Linux)
runtime  <AppData>/runtime/{node-dist,mingit}
ST data  ~/.local/share/SillyTavern                              (Linux)
```