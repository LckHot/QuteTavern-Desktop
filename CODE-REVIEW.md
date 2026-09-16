# QuteTavern — code review findings and improvements

Reviewed: 2026-09-16 · v1.0.2 (commit f6af307) · ~3,150 lines of C++ (Qt 6 Widgets + WebEngine).
Scope: requirement conformance, memory safety, maintainability, use of existing tooling.

**Severity key**
- **P1 bug** — wrong behaviour on a supported platform
- **P2 robustness** — works today, breaks in realistic configurations / contradicts docs
- **P3 hygiene** — maintainability, tooling, docs

**How this review was verified (real execution, not reading alone)**
- Full app compiled against Qt 6.11 with `-Wall -Wextra` (3 warnings, see F8).
- A headless harness compiled from this repo's `Backend/Util/Settings/Updater` and
  driven against a fake SillyTavern: 20 checks, all passed — probe → spawn →
  readiness parse (ANSI-coloured "Go to:" line) → Running; SIGTERM stop; foreign
  instance detection + takeover (the /proc + listening-inode cross-check really
  filters to the listener); EADDRINUSE exit classification; npm dependency
  bootstrap → Running.
- The same harness under AddressSanitizer + UndefinedBehaviorSanitizer: clean.
- Offscreen smoke test of the built binary: stays alive (exit 124), matching CI.
- Runtime coupling re-checked against SillyTavern 1.19.0 sources: the `Go to:` line
  still exists and is ANSI-wrapped; `--global` / `--browserLaunchEnabled` are real
  flags; `env-paths@^3` data layout; `start.sh` npm flags.

---

## F1 — P1 — Windows: SillyTavern's global data directory is wrong

**Evidence** `src/Util.cpp:172-173` builds `%APPDATA%\SillyTavern`. SillyTavern
resolves its global data directory via `envPaths('SillyTavern', {suffix:''})`
(`src/command-line.js`, release 1.19.0) — on Windows that is
`%LOCALAPPDATA%\SillyTavern\Data` (different root, extra `\Data` component;
`env-paths` is a direct dependency of SillyTavern). `README.md:128` and `:134`
document the same wrong path.

**Impact (Windows only)**
- `Util::detectPort()` reads a `config.yaml` that does not exist → always falls
  back to 8000. Custom ports are never probed, so foreign instances on custom
  ports are missed and an unrelated listener on 8000 can produce a false
  "foreign instance" dialog.
- The **Data directory** button opens — and *creates* — an empty
  `%APPDATA%\SillyTavern`, misdirecting users about where their data lives.

**Suggested fix** — platform-correct `stDataDir()`:
Windows `%LOCALAPPDATA%\SillyTavern\Data`; Linux `$XDG_DATA_HOME/SillyTavern`
else `~/.local/share/SillyTavern` (see F2); macOS unchanged. Update the README
table. Worth a unit test for the path builder.

## F2 — P2 — Linux: `XDG_DATA_HOME` ignored

**Evidence** `src/Util.cpp:177` hardcodes `~/.local/share/SillyTavern`; upstream
honours `$XDG_DATA_HOME`. Same symptoms as F1 for users who set it.

**Suggested fix** included in F1's patch.

## F3 — P2 — Windows: netstat parsing depends on a localized string

**Evidence** `src/Util.cpp:350` matches the literal `"LISTENING"` in `netstat -ano`
output. netstat.exe localizes the state column (German: `ABHÖREN`), so on
non-English Windows `findStPids()` returns nothing: "Terminate and take over"
reports "Terminated 0 process(es)" and the port stays occupied.

**Suggested fix** — parse without the state text: a listening TCP row has foreign
address `0.0.0.0:0` / `[::]:0`; keep "PID = last column". (Or `GetExtendedTcpTable`,
heavier.) Add a fixture-based test.

## F4 — P2 — Installer does not filter version tags the way Updater does

**Evidence** `src/Installer.cpp:51-56` takes the first line of
`git tag --list --sort=-v:refname` unfiltered; `src/Updater.cpp:21-25,64` filters
with `^[0-9][0-9.]*$` to skip pre-releases. The two paths disagree; a future
pre-release tag on the release branch would make "Install a new copy" check out
an RC while the update flow refuses to.

**Suggested fix** — one shared helper (e.g. `Updater::latestVersionTag(root)`)
used by both; unit test with a tag-list fixture.

## F5 — P2 — Blocking work on the UI thread (contradicts DESIGN §8.1)

**Evidence** `EnvCheck.cpp:242,311` call `Util::extractArchive()` from the main
thread; `Util.cpp:574` blocks up to 180 s in `waitForFinished()`. Extracting the
~30 MB Node archive freezes the dialog for seconds to a minute. Same class: the
login-shell PATH harvest (`Util.cpp:83`, up to ~4 s) can run on the first failed
lookup from the UI thread (`EnvCheck::finishOk` re-check).

**Suggested fix** — move download+extract into a worker (the `QThread::create` +
`QMetaObject::invokeMethod(qApp, ...)` pattern already used for install/update),
or at minimum pre-warm the harvest before dialogs are shown.

## F6 — P2 (docs) — claims in README/DESIGN not backed by the code

- **PDEATHSIG**: `README.md:142` implies only macOS leaves the backend running
  when the launcher is force-killed. No `prctl` / `setChildProcessModifier` /
  job-object code exists — a SIGKILL of the launcher orphans the backend on
  *every* platform.
- **taskkill**: `README.md:48` and `DESIGN.md:135` say the backend stop uses
  `taskkill` on Windows; the code stops its own child via
  `QProcess::terminate()` → `kill()` (`Backend.cpp:451-469`). `taskkill` is only
  used for *foreign* instances (`Util.cpp:373`).
- **node >= 22** (`README.md:151`): no version check exists anywhere; upstream
  `engines` says `>= 20`.

**Suggested fix** — either implement Linux `PR_SET_PDEATHSIG` in
`Backend::spawnNode()` (two lines via `setChildProcessModifier`) and correct the
other two claims, or correct all three.

## F7 — P3 — npm install flags: duplicated three times and drifted from upstream

**Evidence** the same argument list is written out in `Backend.cpp:210-215`,
`Updater.cpp:117`, `Installer.cpp:72`, and misses `--no-save` / `--no-progress`
that SillyTavern's `start.sh` uses
(`npm install --no-save --no-audit --no-fund --loglevel=error --no-progress --omit=dev --ignore-scripts`).
Tested: with the lockfile in sync, omitting `--no-save` does **not** modify
`package-lock.json`, so this is not an active bug — but `--no-save` is upstream's
guard against manifest drift and `--no-progress` keeps the log clean.

**Suggested fix** — one shared constant aligned with `start.sh`; delete the copies.

## F8 — P3 — `-Wall -Wextra` warnings (CI builds without warning flags)

**Evidence** (compiler output on this tree)
- `EnvCheck.cpp:274` `fetchMinGitUrl()` is defined but never used on Linux/macOS:
  the only call site (`EnvCheck.cpp:203`) sits under `#if defined(Q_OS_WIN)` but
  the definition does not.
- `MainWindow.cpp:390` `applyState()`: the local struct is read after a `switch`
  with no `default:` → `-Wmaybe-uninitialized` (a future `Status` value would read
  uninitialized memory).

**Suggested fix** — guard the MinGit function with `#ifdef Q_OS_WIN`; add
`default:` to the switch (or initialize the struct first).

## F9 — P3 — build enables no warnings

**Evidence** `CMakeLists.txt` only adds `/utf-8` for MSVC. F8 went unnoticed
because CI compiles without `-Wall -Wextra` / `/W4`.

**Suggested fix** — `target_compile_options(qutetavern PRIVATE -Wall -Wextra)` +
MSVC equivalent; the tree becomes warning-free once F8 is fixed.

## F10 — P3 — no automated tests for the fragile parts

The pure-logic surfaces most likely to break silently — version-tag filtering,
the "Go to:" line parse, ANSI stripping, `/proc` & netstat parsing, the exit
classifier — have no tests; CI only asserts packaging (install/start/uninstall).

**Suggested fix** — a `tests/` target (Qt Test via `qt_add_test`, or a plain
ctest executable) covering those functions; wire it into `build.yml` before the
packaging jobs.

## F11 — P3 — repository hygiene

- `aqtinstall.log` (24 KB local Qt-installer debug log) is committed (added in
  `8a9185b`, modified in `6775399`).

**Suggested fix** — `git rm --cached aqtinstall.log` and add it to `.gitignore`.

## F12 — P3 — small robustness / consistency nits

- `main.cpp:29-41` — the single-instance probe uses a 300 ms connect and then
  *unconditionally* `QLocalServer::removeServer()`: a slow first instance can be
  "stolen" (two instances run; the first one's raise socket is gone). Retry the
  probe once, or only remove the server after `listen()` failed.
- `Backend.cpp:244-249` — `onNpmFinished()` calls `finishStop()` when the state is
  not `Starting`; after `stop()` already did the same, this emits a second
  `Stopped` `stateChanged()` (harmless; reducible).
- `Backend.h:50` — "Thread-safe log entry point" is misleading: `log()` mutates
  `m_tail` and emits; it is only safe on the object's thread (all current call
  sites correctly marshal via `QMetaObject::invokeMethod(qApp, …)`).
- `Backend.cpp:338` — `processId()` is read immediately after `start()`; it can
  legally be 0 there (log after the `started()` signal instead).
- `EnvCheck.cpp:214` says "latest Node.js **LTS** version" but the code takes
  `arr.first()` of `nodejs.org/dist/index.json` (the newest release overall,
  which may be a non-LTS "Current" line).
- `install.sh:88` generates `Exec=bash -lc <path>` while
  `packaging/linux/qutetavern.desktop:4` ships `Exec=qutetavern`; with the
  launcher's own login-shell PATH fallback the wrapper is redundant — pick one
  mechanism and document it.
- `Util.cpp:216-226` `openPath()` re-implements
  `QDesktopServices::openUrl(QUrl::fromLocalFile())`.
- The HTTP probe (`Backend.cpp:93-146`) hand-rolls `QTcpSocket` while the project
  already uses `QNetworkAccessManager` (`EnvCheck`); a HEAD through QNAM would
  remove the manual buffer handling (low priority — the current code is correct).

---

## Appendix A — verified working (do not re-investigate)

- Backend state machine under real processes: probe → dependency check → spawn →
  readiness parse → Running; graceful stop; foreign detection + takeover;
  EADDRINUSE / MODULE_NOT_FOUND classification; npm bootstrap.
- Cross-thread `QPointer` discipline matches DESIGN §3; ASan/UBSan clean on the
  exercised paths.
- Readiness parse still matches SillyTavern 1.19.0 (`Go to: <url> to open
  SillyTavern`, URL ANSI-coloured; strip-then-match works).
- `--global`, `--browserLaunchEnabled` are real flags; npm flags otherwise match
  `start.sh`.
- Packaging/CI, license notices (AGPL/Qt LGPL replacement options) and SPDX
  headers are thorough.

## Appendix B — not verified in this review

- macOS / Windows runtime behaviour (no hardware available; DESIGN §9 says the
  same) — F1 and F3 need a real Windows machine (or at least the added unit
  tests) to confirm the fixes.
- GUI dialogs and the WebEngine ST window beyond "process stays alive"
  (offscreen platform only).
- Qt 6.2 floor compile (built against 6.11 here; CI covers 6.2.4).

## Appendix C — reproducing the harness (this machine)

Harness + fake SillyTavern + ASan build live in `/opt/data/tmp/qtr/`
(`harness/harness.cpp`, `setup.sh`, `run.sh`, `run-asan.sh`; Qt from
`/home/linuxbrew/.linuxbrew`). It is scratch tooling, not part of the repo.
