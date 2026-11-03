# QuteTavern Android Support Plan

Status: proposal (codebase v1.1.0) · [Back to README](README.md) · [Design document](DESIGN.md)

This document plans how Android joins Linux / Windows / macOS as a fourth
supported platform **without an architecture change**: Android becomes a fourth
column in the existing platform-abstraction tables, not a new design. It records
the constraints, the enabling facts, the per-module change list, the milestones
and the risks.

## 1. Goal and non-goals

**Goal**: the same launcher on Android — the management window installs
SillyTavern from upstream, starts/stops the backend, streams the log, and the
front end is one tap away.

**Non-goals**:

- No architecture changes: the Backend QProcess state machine, the threading
  model, the window contract and the install/update logic stay exactly as they
  are (DESIGN.md §3–§4).
- No forked UI stack: Qt Widgets stays; no QML is introduced.
- No Play Store requirement: the APK is sideloaded from GitHub releases like
  the other packages.

## 2. The compatibility principle: couple to interfaces, never to bytes

QuteTavern exists to be a wrapper that **does not need maintenance when
SillyTavern upstream moves** (unlike projects that ship a pinned SillyTavern
inside a shell). The wrapper's entire coupling to upstream today is a small set
of stable interfaces:

| Coupling point | Consumed by |
| --- | --- |
| CLI flags `--global` / `--dataRoot` / `--browserLaunchEnabled` | `Backend` |
| The `Go to: http://…` readiness line on stdout | `Backend` regex |
| Top-level `port:` of the global `config.yaml` | `Util::readConfigPort` |
| `server.js` + `package.json` with `name == sillytavern` | `Util::validateRoot` |
| The `release` branch and plain numeric version tags | `Installer` / `Updater` |

None of these requires the wrapper to hold upstream's bytes. Therefore on
Android:

- **The payload (SillyTavern) is always cloned from upstream at runtime** —
  the same `Install a new copy from GitHub` / `Check for updates` flows as on
  the desktop, byte-identical semantics (dirty-tree protection included).
  SillyTavern is never shipped inside the APK.
- **The tools (Node.js, git) are bundled with the APK** — this is not a new
  coupling but the desktop "built-in components" model (DESIGN.md §5) with a
  different delivery channel, and on Android it is forced by the platform
  (§3, constraint 2). Tool versions evolve on a years scale and do not track
  SillyTavern releases.

Android adds **zero** new coupling points to the table above.

## 3. Platform constraints (the three hard facts)

1. **Qt WebEngine is not available on Android** — verified against the module's
   own configure script (not only the docs): `qtwebengine/configure.cmake`
   (6.11) gates the module with
   `qt_webengine_configure_check("supported-platform" MODULES QtWebEngine
   CONDITION LINUX OR WIN32 OR MACOS MESSAGE "Build can be done only on Linux,
   Windows or macOS.")`. The same file carries Android support for the *sibling*
   module QtPdf (`MODULES QtPdf`, plus the `android-ndk` /
   `android-ndk-api-level` checks: NDK >= 27, API >= 28) — that is QtPdf, not
   WebEngine, and Qt WebEngine's platform notes list Windows/Linux/macOS only.
   Re-verify by reading those two checks before assuming the situation changed.
   `StWindow` (QWebEngineView) therefore cannot compile — the only compile-level
   obstacle. Qt WebView was considered and rejected: it is a
   QML-only module whose native view overlay misbehaves inside QQuickWidget in
   a Widgets application. **The front end opens in the system browser**
   (Chromium-based on all current devices; "Add to Home screen" gives a
   PWA-like icon).
2. **No system Node.js, and targetSdk ≥ 29 forbids `exec()` of files in the
   app's writable home directory** (SELinux W^X). Executables can only arrive
   through the APK's `jniLibs` (named `lib*.so`, extracted to
   `nativeLibraryDir` with `extractNativeLibs=true`, which is an exec-allowed
   directory). Runtime-downloaded binaries can never gain exec permission —
   which is exactly why the tools must be bundled while the payload (pure data:
   JS sources and npm tarballs) must not and need not be.
3. **No system git; `/proc` hides other applications' processes; background
   apps are frozen and killed.** Consequences: git ships as a bundled tool;
   foreign-instance takeover degrades to attach/cancel (§7); while the backend
   runs, a foreground service with a partial wake lock keeps the process alive
   (§6, AndroidSupport).

## 4. Enabling facts (verified against the tree)

- **SillyTavern's dependencies are pure JS**: 96 direct dependencies, ~440
  installed packages, **zero `.node` native addons**. `npm install` on device
  needs no compiler and no node-gyp; the resulting tree is platform-independent.
- **`server.js` accepts `--dataRoot`** (`globalThis.DATA_ROOT = cliArgs.dataRoot`),
  so the global data directory is pointed straight into app-private storage;
  there is no fight with `env-paths` platform conventions.
- The backend is reached over plain loopback TCP/HTTP: the probe
  (`QTcpSocket`), the readiness regex and the timeouts are platform-agnostic.
- **Android is a Linux kernel**: the graceful stop sequence SIGTERM → 5 s →
  SIGKILL carries over unchanged — SillyTavern's cleanup hooks work, which even
  Windows does not get.
- `QLocalServer`/`QLocalSocket` (single instance), `QDesktopServices::openUrl`
  (browser / file intents) and `QStandardPaths` (app-private storage) all work
  on Android.

## 5. Toolchain: Node and git as APK-bundled built-in components

The desktop resolves commands as *inherited PATH → login-shell PATH → built-in
components* and injects `Util::commandEnv()` into every child process. Android
replaces the last two stages with the bundled toolchain; the resolution API is
unchanged.

### 5.1 Node.js

- Built from upstream source with the official (experimental) `android-configure`
  + NDK, one pinned, verified Node version per ABI (`arm64-v8a` first,
  `x86_64` for emulators/Chromebooks later).
- Packaged as `lib/<abi>/libnode_exec.so` inside the APK; executed by
  `QProcess` from `nativeLibraryDir`. **The Backend keeps its process model:
  a real child process, stdout streaming, `terminate()`/`kill()` semantics and
  reaping all unchanged.**

### 5.2 git

- git is portable C and cross-builds with the NDK (the Termux git package is
  the reference for the resulting file layout). Over HTTPS only two executables
  are needed: the `git` main binary (clone/fetch/checkout/status/describe/tag
  are builtins) and the `git-remote-https` transport helper; their library
  dependencies (libcurl, libpcre2, …) are ordinary `jniLibs` citizens resolved
  by the Android loader.
- At first run the launcher creates a **symlink farm** in its files directory
  (`git` → `libgit.so`, `git-remote-https` → `libgit_remote_https.so`, …) and
  points `GIT_EXEC_PATH` at the farm. The SELinux exec check applies to the
  resolved target inode, which lives in `nativeLibraryDir`, so execution is
  permitted. A bundled `cacert.pem` is referenced via `GIT_SSL_CAINFO`.

### 5.3 npm on device

- npm is `node <npm-cli.js>`; given a working node it needs only `HOME`,
  `TMPDIR` and `npm_config_cache` pointed at app-private storage. Registry
  access uses Node's compiled-in CA bundle. The existing flags
  (`Util::npmInstallArgs`: `--omit=dev --ignore-scripts --no-save`) are kept;
  with `--ignore-scripts` no lifecycle subprocesses run and the pure-JS tree
  (§4) needs no toolchain beyond node itself.

### 5.4 `commandEnv()` / `findCommand()` Android branches

```
Util::findCommand("node") → <nativeLibraryDir>/libnode_exec.so
Util::findCommand("git")  → <nativeLibraryDir>/libgit.so
Util::commandEnv()        → inherited env + PATH(farm dirs) + HOME + TMPDIR
                            + npm_config_cache + GIT_EXEC_PATH + GIT_SSL_CAINFO
```

This is the *single seam* the rest of the codebase sees — precisely the role
DESIGN.md §5 already assigns to these two functions.

### 5.5 Toolchain refresh (the residual coupling, honestly scoped)

If upstream ever raises its Node major-version requirement, the bundled
toolchain is refreshed by an ordinary **APK update** — an event on a years
scale, decoupled from SillyTavern's weekly release rhythm (Node major bumps are
rare and strongly backward-compatible). Optional future hardening, mirroring
the EnvCheck download dialog: a "toolchain update" check against the project's
own releases that fetches a small tools-only APK and prompts installation.
Never tied to SillyTavern versions.

## 6. Module-by-module change list

| File | Change |
| --- | --- |
| `CMakeLists.txt` | `find_package(WebEngineWidgets)` and `StWindow.cpp` behind `if(NOT ANDROID)`; `qt_add_executable` already produces the APK via androiddeployqt; new `android/` package source (`QT_ANDROID_PACKAGE_SOURCE_DIR`): AndroidManifest.xml (`extractNativeLibs=true`, INTERNET + FOREGROUND_SERVICE permissions, foregroundServiceType), gradle settings; per-ABI APK splits |
| `src/main.cpp` | Guard `AA_ShareOpenGLContexts` behind `#ifndef Q_OS_ANDROID`; single-instance unchanged |
| `src/Util.{h,cpp}` | Fourth platform column: `findCommand`/`commandEnv` per §5.4; `stDataDir()`/`detectPort()` read the `--dataRoot` location the launcher itself passes; `findStPids()` matches only own child PIDs (`/proc` hides other apps); `killForeignBackends()` restricted to own children; `extractArchive()`/runtime-download paths unused; new tiny `PlatformCapabilities` query (`canInstall`, `canUpdate`, `canBindDirectory`, `canTakeOverForeign`, `hasEmbeddedBrowser`) so the UI asks capabilities instead of scattering `#ifdef` |
| `src/Backend.{h,cpp}` | **State machine unchanged.** Two additions: append `--dataRoot <AppDataLocation>/SillyTavern` to the spawn arguments; call `AndroidSupport::setKeepAlive(bool)` from the Android branch of `setStatus()` (Running → on, leaving Running → off) |
| `src/Installer.{h,cpp}` | **Zero changes** — consumes only `findCommand`/`commandEnv`/`runStreaming` |
| `src/Updater.{h,cpp}` | **Zero changes** — same reason |
| `src/EnvCheck.{h,cpp}` | Download flow replaced by a **toolchain self-test** (`node --version`, `git --version` smoke run against the bundled binaries) with repair guidance (reinstall APK); same dialog skeleton, new copy |
| `src/MainWindow.{h,cpp}` | `openStWindow()` Android branch → `QDesktopServices::openUrl(url)`; "bind an existing directory" hidden (scoped storage makes foreign checkouts meaningless); install/update actions driven by `PlatformCapabilities` |
| `src/StWindow.{h,cpp}` | Excluded from the Android build |
| `src/AndroidSupport.{h,cpp}` | **New file, `Q_OS_ANDROID` only.** One function: `setKeepAlive(bool)` — starts/stops a foreground service with a persistent notification ("SillyTavern backend is running") and holds a partial wake lock via JNI. The only genuinely new platform glue, with a deliberately narrow boundary |
| `src/Settings.{h,cpp}` | Unchanged (JSON in `AppConfigLocation`) |
| `tests/` | Existing pure-logic tests unchanged; Android branches compile-checked in CI (build-only) |

## 7. User-visible differences (honest degradation, DESIGN.md style)

| Capability | Linux / macOS / Windows | Android |
| --- | --- | --- |
| Front end | Embedded Chromium window (StWindow) | System browser via `openUrl` (auto-opened on Running, reopenable from the management window) |
| Window semantics | Backend running ⟺ ST window exists | Backend running ⟺ notification present; the browser tab's lifecycle belongs to the browser |
| Install a new copy from GitHub | yes | yes (on-device clone + npm install) |
| Check for updates | yes | yes (identical git flow, dirty-tree protection included) |
| Bind an existing directory | yes | no (scoped storage) |
| Foreign instance on the port | take over / attach / cancel | attach / cancel only (`/proc` hides other apps, e.g. a Termux-hosted instance) |
| Graceful stop | SIGTERM → 5 s → SIGKILL (Windows: immediate kill) | SIGTERM → 5 s → SIGKILL (Linux kernel semantics) |
| Environment check | download Node/MinGit at runtime | bundled toolchain self-test |
| Background operation | n/a (desktop stays resident) | foreground service + wake lock while Running; closing the management window still stops the backend and quits |

## 8. Packaging and CI

- New job in `.github/workflows/build.yml` beside the existing four: install Qt
  for Android + NDK (aqtinstall), build, `androiddeployqt`, sign; artifact
  `QuteTavern-<version>-<abi>.apk`, published by the same `v*` tag flow.
- The Node/git toolchain binaries are **external build inputs** (produced by a
  separate, rarely-run toolchain build, pinned by version), fetched by the CI
  job — they do not live in this source tree, keeping the repository free of
  blobs and free of anything that tracks upstream releases.
- APK size: ≈ 60–90 MB per ABI (node + git dominate) → per-ABI splits.
- First-run install traffic: a full `git clone` plus `npm install`
  (≈ 300 MB tree) — streamed through the existing live log panel, with a
  Wi-Fi/storage heads-up in the install dialog.

## 9. Milestones

- **M0 — risk retirement PoC** (the only true unknowns), before touching this
  tree: (a) `libnode_exec.so` executes from `nativeLibraryDir` on real devices
  across API 29–35 and vendors; (b) `git clone` over HTTPS through the symlink
  farm + `GIT_EXEC_PATH`; (c) on-device `npm install` of a SillyTavern
  checkout. All three on the oldest and newest API levels available.
- **M1 — skeleton**: CMake/CI produce per-ABI APKs; management window runs;
  EnvCheck shows the toolchain self-test; install/update hidden.
- **M2 — core loop**: runtime clone from GitHub → on-device npm install →
  backend state machine → system browser opens the front end; log panel,
  update check and graceful stop verified on device.
- **M3 — hardening & docs**: foreground-service keep-alive; degradation copy;
  DESIGN.md gains the Android column (§5 table, §8 decisions, §9 status);
  README packaging table lists the APKs.

## 10. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| `exec` from `nativeLibraryDir` blocked by a vendor SELinux policy | M0 PoC across vendors/API levels first; last-resort fallback is an in-process libnode thread (nodejs-mobile style) — deliberately not the default, as it breaks the QProcess ownership model (DESIGN.md §8.2) |
| Node-for-Android build is official but experimental | pin one verified Node version; toolchain refreshed independently of this repo (§5.5) |
| Symlink-farm git exec edge cases | M0(b); farm layout mirrors the Termux git package, which is battle-tested on the same kernels |
| 90 s readiness timeout tight on low-end devices | measure in M2; make the timeout platform-scaled if needed (one constant, not a design change) |
| APK size / first-install traffic on metered connections | per-ABI splits (§8); install dialog states size and recommends Wi-Fi |

## 11. What this plan preserves

- The wrapper still ships **no SillyTavern bytes**; upstream can restructure,
  add dependencies or re-tag releases and both desktop and Android builds keep
  working untouched as long as the §2 interfaces hold.
- The Backend state machine, Installer, Updater and Settings carry **zero or
  near-zero diffs**; all divergence is behind the existing seams (`Util`
  platform column, `openStWindow()`, EnvCheck's dialog, one narrow new
  `AndroidSupport` module).
- The honest-degradation language of DESIGN.md extends naturally: Android's
  limitations are documented capabilities, not hidden breakage.
