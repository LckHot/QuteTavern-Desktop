#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build a Debian and an RPM package around a binary that links the
# distribution's Qt. Qt is deliberately *not* bundled: the packages declare the
# Qt libraries and QML modules they need and let the system's package manager
# provide them.
#
#   make-packages.sh <binary> <version> <output-dir>
#
# The binary is built against the official Qt binaries at the floor version the
# CI pins (QT_VERSION_FLOOR in .github/workflows/build.yml), and the packages
# are installed and started on a distribution that ships a Qt at or above that
# floor (.deb: a Debian container, .rpm: a Fedora container).
set -euo pipefail

BINARY="$(realpath "${1:?usage: make-packages.sh <binary> <version> <output-dir>}")"
VERSION="${2:?missing version}"
OUTDIR="$(realpath -m "${3:?missing output directory}")"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MAINTAINER="${MAINTAINER:-QuteTavern contributors <LckHot@users.noreply.github.com>}"
HOMEPAGE="https://github.com/LckHot/QuteTavern-Desktop"
DESCRIPTION="Desktop launcher for SillyTavern"
# Every line starts with a space: the Debian control format needs that for
# description continuation lines
SUMMARY_LONG=" QuteTavern supervises the SillyTavern server as a child process and shows its
  web interface in an embedded Chromium window (Qt WebEngine). It installs and
  updates SillyTavern, and starts or stops the backend with one click."

[ -x "$BINARY" ] || { echo "error: $BINARY is not executable" >&2; exit 1; }
mkdir -p "$OUTDIR"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
ROOT="$WORK/root"

# ---------- payload (plain FHS layout, no bundled libraries) ----------
install -Dm755 "$BINARY" "$ROOT/usr/bin/qutetavern"
install -Dm644 "$REPO_ROOT/packaging/linux/qutetavern.desktop" \
    "$ROOT/usr/share/applications/qutetavern.desktop"
install -Dm644 "$REPO_ROOT/resources/icon.png" \
    "$ROOT/usr/share/icons/hicolor/256x256/apps/qutetavern.png"
install -Dm644 "$REPO_ROOT/LICENSE" "$ROOT/usr/share/doc/qutetavern/LICENSE"
install -Dm644 "$REPO_ROOT/THIRD_PARTY_NOTICES.md" \
    "$ROOT/usr/share/doc/qutetavern/THIRD_PARTY_NOTICES.md"
cat > "$ROOT/usr/share/doc/qutetavern/copyright" <<EOF
Upstream: $HOMEPAGE
License: AGPL-3.0-or-later (see LICENSE next to this file)
Third-party components: see THIRD_PARTY_NOTICES.md next to this file
EOF

SIZE_KB=$(du -sk "$ROOT" | cut -f1)

# ---------- Debian ----------
if command -v dpkg-deb >/dev/null 2>&1; then
    # The dependencies are explicit and CI-verified (the package is installed and
    # started inside a Debian container before it is published). dpkg-shlibdeps is
    # deliberately not used: the build links the official Qt binaries, whose
    # libraries belong to no Debian package.
    #
    # Three groups, for three different reasons:
    #  - the system libraries Qt and Chromium need at runtime
    #  - the Qt libraries this binary links (Qt 6.5 floor: the QML UI uses
    #    QQmlApplicationEngine::loadFromModule, added in 6.5, and the native
    #    folder dialogs from Qt 6.3)
    #  - the QML modules, which are loaded by the QML engine at runtime and are
    #    therefore invisible to the shared library dependencies: the dialogs and
    #    pages import QtQuick.Controls (which pulls Templates/Controls2), the
    #    layout types come from QtQuick.Layouts, and the web view is
    #    QtWebEngine's QML module. qt6-qpa-plugins provides the platform plugins
    #    and libqt6webenginecore6-bin the WebEngine helper process + resources.
    DEPENDS="libc6 (>= 2.35), libgcc-s1, libstdc++6, libgl1, libx11-6, libx11-xcb1, libxcb1, libxkbcommon0, libxkbcommon-x11-0, libfontconfig1, libfreetype6, libnss3, libnspr4, libasound2, libdbus-1-3, libgbm1, libqt6core6 (>= 6.5), libqt6gui6 (>= 6.5), libqt6network6 (>= 6.5), libqt6qml6 (>= 6.5), libqt6quick6 (>= 6.5), libqt6quickcontrols2-6 (>= 6.5), libqt6quicktemplates2-6 (>= 6.5), libqt6webchannel6 (>= 6.5), libqt6positioning6 (>= 6.5), libqt6webenginecore6 (>= 6.5), libqt6webenginequick6 (>= 6.5), libqt6webenginecore6-bin (>= 6.5), qml6-module-qtqml, qml6-module-qtquick, qml6-module-qtquick-window, qml6-module-qtquick-templates, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtwebengine, qt6-qpa-plugins"

    mkdir -p "$ROOT/DEBIAN"
    cat > "$ROOT/DEBIAN/control" <<EOF
Package: qutetavern
Version: $VERSION
Section: games
Priority: optional
Architecture: amd64
Maintainer: $MAINTAINER
Installed-Size: $SIZE_KB
Depends: $DEPENDS
Homepage: $HOMEPAGE
Description: $DESCRIPTION
$SUMMARY_LONG
EOF

    echo "  Depends: $DEPENDS"
    dpkg-deb --build --root-owner-group "$ROOT" "$OUTDIR/QuteTavern-$VERSION-amd64.deb" >/dev/null
    echo "built $OUTDIR/QuteTavern-$VERSION-amd64.deb"
    echo "  Depends: $DEPENDS"
else
    echo "note: dpkg-deb is not available, skipping the Debian package"
fi

# ---------- RPM ----------
if command -v rpmbuild >/dev/null 2>&1; then
    RPMTOP="$WORK/rpm"
    mkdir -p "$RPMTOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS}
    # DEBIAN/control must never leak into the RPM payload (the section order
    # makes that possible), so it is excluded explicitly
    tar -C "$ROOT" --exclude="DEBIAN" -czf "$RPMTOP/SOURCES/payload.tar.gz" .

    # No manual Requires: rpm's dependency generator records the SONAMEs of
    # every library the binary links (libQt6Quick.so.6, libQt6Qml.so.6,
    # libQt6WebEngineQuick.so.6, ...). On Fedora the packages providing those
    # libraries (qt6-qtdeclarative, qt6-qtwebengine) also carry the QML modules
    # under /usr/lib64/qt6/qml, so both the libraries and the modules arrive
    # through the same dependencies - which keeps the package usable on Fedora,
    # RHEL and openSUSE alike. The CI verifies this by checking that the QML
    # directories exist after `dnf install` in a clean Fedora container.
    cat > "$RPMTOP/SPECS/qutetavern.spec" <<EOF
Name:      qutetavern
Version:   $VERSION
Release:   1
Summary:   $DESCRIPTION
License:   AGPL-3.0-or-later
URL:       $HOMEPAGE
BuildArch: x86_64
Source0:   payload.tar.gz

%description
$SUMMARY_LONG

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
tar -C %{buildroot} -xzf %{SOURCE0}

%files
/usr/bin/qutetavern
/usr/share/applications/qutetavern.desktop
/usr/share/icons/hicolor/256x256/apps/qutetavern.png
/usr/share/doc/qutetavern

%changelog
* $(LC_ALL=C date '+%a %b %d %Y') $MAINTAINER - $VERSION-1
- QuteTavern $VERSION
EOF

    rpmbuild -bb --define "_topdir $RPMTOP" "$RPMTOP/SPECS/qutetavern.spec" >/dev/null
    cp "$RPMTOP"/RPMS/*/*.rpm "$OUTDIR/QuteTavern-$VERSION-x86_64.rpm"
    echo "built $OUTDIR/QuteTavern-$VERSION-x86_64.rpm"
else
    echo "note: rpmbuild is not available, skipping the RPM package"
fi