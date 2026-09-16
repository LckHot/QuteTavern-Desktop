#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build a Debian and an RPM package from an assembled AppDir, so that a single
# Linux build produces the AppImage plus both native package formats.
#
#   make-packages.sh <appdir> <version> <output-dir>
#
# The payload goes below /opt/qutetavern - the bundled Qt stays next to the
# binary exactly as in the AppDir - and is exposed through /usr/bin/qutetavern.
set -euo pipefail

APPDIR="$(realpath "${1:?usage: make-packages.sh <appdir> <version> <output-dir>}")"
VERSION="${2:?missing version}"
OUTDIR="$(realpath -m "${3:?missing output directory}")"
MAINTAINER="${MAINTAINER:-QuteTavern contributors <LckHot@users.noreply.github.com>}"
HOMEPAGE="https://github.com/LckHot/QuteTavern-Desktop"
PREFIX="/opt/qutetavern"
DESCRIPTION="Desktop launcher for SillyTavern"

[ -x "$APPDIR/usr/bin/qutetavern" ] || {
    echo "error: $APPDIR/usr/bin/qutetavern is missing" >&2
    exit 1
}
mkdir -p "$OUTDIR"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
ROOT="$WORK/root"

# ---------- payload ----------
mkdir -p "$ROOT$PREFIX" "$ROOT/usr/bin" "$ROOT/usr/share"
cp -a "$APPDIR/usr/." "$ROOT$PREFIX/"
cp -a "$APPDIR/usr/share/." "$ROOT/usr/share/" # desktop entry, icon, notices

# Without this Qt looks for the build machine's paths; with it, plugins and the
# WebEngine runtime (libexec, resources, translations) resolve relative to the
# installation prefix.
cat > "$ROOT$PREFIX/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
EOF

ln -s "$PREFIX/bin/qutetavern" "$ROOT/usr/bin/qutetavern"

mkdir -p "$ROOT/usr/share/doc/qutetavern"
cat > "$ROOT/usr/share/doc/qutetavern/copyright" <<EOF
Upstream: $HOMEPAGE
License: AGPL-3.0-or-later (see LICENSE next to this file)
Third-party components: see THIRD_PARTY_NOTICES.md next to this file
EOF

SIZE_KB=$(du -sk "$ROOT" | cut -f1)

# ---------- RPM ----------
if command -v rpmbuild >/dev/null 2>&1; then
    RPMTOP="$WORK/rpm"
    mkdir -p "$RPMTOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS}
    tar -C "$ROOT" -czf "$RPMTOP/SOURCES/payload.tar.gz" .

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
QuteTavern supervises the SillyTavern server as a child process and shows its
web interface in an embedded Chromium window (Qt WebEngine). It installs and
updates SillyTavern, and starts or stops the backend with one click.

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
tar -C %{buildroot} -xzf %{SOURCE0}

%files
$PREFIX
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
    echo "warning: rpmbuild is not installed, skipping the RPM package" >&2
fi

# ---------- Debian ----------
if command -v dpkg-deb >/dev/null 2>&1; then
    mkdir -p "$ROOT/DEBIAN"
    cat > "$ROOT/DEBIAN/control" <<EOF
Package: qutetavern
Version: $VERSION
Section: games
Priority: optional
Architecture: amd64
Maintainer: $MAINTAINER
Installed-Size: $SIZE_KB
Depends: libc6 (>= 2.35), libgl1, libx11-6, libxcb1, libxkbcommon0, libfontconfig1, libfreetype6, libnss3, libnspr4, libasound2, libdbus-1-3, libgbm1
Homepage: $HOMEPAGE
Description: $DESCRIPTION
 QuteTavern supervises the SillyTavern server as a child process and shows its
 web interface in an embedded Chromium window (Qt WebEngine). It installs and
 updates SillyTavern, and starts or stops the backend with one click.
EOF

    dpkg-deb --build --root-owner-group "$ROOT" "$OUTDIR/QuteTavern-$VERSION-amd64.deb" >/dev/null
    echo "built $OUTDIR/QuteTavern-$VERSION-amd64.deb"
else
    echo "warning: dpkg-deb is not installed, skipping the Debian package" >&2
fi