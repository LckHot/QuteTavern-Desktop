# Third-party notices

The release packages of the QuteTavern contain third-party software
and assets. This file lists them together with their licenses; the full license
texts can be obtained from the projects linked below.

The launcher itself is licensed under the GNU Affero General Public License
v3.0 (see [LICENSE](LICENSE)).

## Qt 6 (Widgets, Network, WebEngine)

- Ships with: all packages (the Linux AppImage, the Windows installer and the
  macOS package contain the Qt libraries, the WebEngine helper process, its
  resources and its locale packs)
- License: LGPL-3.0 (Qt WebEngine additionally contains Chromium code under
  BSD-style licenses)
- Copyright: The Qt Company Ltd. and contributors
- Sources: <https://code.qt.io/> and <https://download.qt.io/>
- The launcher links Qt dynamically and ships it unmodified. As required by the
  LGPL, the libraries can be replaced with a compatible build: replace the
  bundled `.so` files inside the AppImage, drop a matching Qt next to
  `qutetavern.exe` on Windows, or replace the frameworks inside
  `QuteTavern.app` on macOS.

## Chromium (used through Qt WebEngine)

- License: BSD-3-Clause and other licenses; see
  <https://doc.qt.io/qt-6/qtwebengine-licensing.html> and the
  `LICENSES.chromium.html` that Qt ships for details.

## SillyTavern (application icon)

- `resources/icon.png` is the official SillyTavern application icon
  (`public/img/apple-icon-512x512.png` in the upstream repository). It is
  pixel-identical to that file and was only re-encoded as PNG.
- Copyright: the SillyTavern project and its contributors
- License: AGPL-3.0 (<https://github.com/SillyTavern/SillyTavern>) - the same
  license as this launcher, so the icon is redistributed under identical terms.
- SillyTavern itself is not bundled: the launcher only starts it as a child
  process and never modifies it.

## Node.js and MinGit (downloaded at runtime, never bundled)

- On request the launcher downloads portable copies of Node.js (Node.js
  license, MIT-style) and, on Windows, MinGit (GPL-2.0 with a linking
  exception) into its application data directory. They are not part of the
  release packages and can be removed by deleting the `runtime` directory.