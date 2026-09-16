#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build a Debian and an RPM package around a binary that links the
# distribution's Qt. Qt is deliberately *not* bundled: the packages declare the
# Qt libraries they need and let the system's package manager provide them.
#
#   make-packages.sh <binary> <version> <output-dir>
#
# The binary has to be built against the Qt of the target distribution (the CI
# builds the .deb on Ubuntu and the .rpm inside a Fedora container).
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
    # Let dpkg work out which packages the binary needs (Qt included) instead of
    # guessing names: this is what dh_shlibdeps does for regular packages.
    DEPENDS=""
    if command -v dpkg-shlibdeps >/dev/null 2>&1; then
        mkdir -p "$WORK/dpkg/debian"
        printf 'Source: qutetavern\nPackage: qutetavern\nArchitecture: amd64\n' \
            > "$WORK/dpkg/debian/control"
        ( cd "$WORK/dpkg" && dpkg-shlibdeps -O -e "$ROOT/usr/bin/qutetavern" ) \
            > "$WORK/shlibs.txt" 2> "$WORK/shlibs.err" || true
        DEPENDS="$(sed -n 's/^shlibs:Depends=//p' "$WORK/shlibs.txt")"
        if [ -s "$WORK/shlibs.err" ]; then
            sed 's/^/  shlibdeps: /' "$WORK/shlibs.err" >&2
        fi
    fi
    if [ -z "$DEPENDS" ]; then
        echo "warning: dpkg-shlibdeps produced nothing, falling back to a static list" >&2
        DEPENDS="libc6 (>= 2.35), libstdc++6, libqt6core6, libqt6gui6, libqt6widgets6, libqt6network6, libqt6webenginewidgets6"
    fi
    # WebEngine needs its runtime data (helper process, resources) and Qt its
    # platform plugins; neither is covered by the shared library dependencies
    DEPENDS="$DEPENDS, qt6-qpa-plugins, libqt6webenginecore6-bin"

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
    tar -C "$ROOT" -czf "$RPMTOP/SOURCES/payload.tar.gz" .

    # No manual Requires: rpm's dependency generator records the SONAMEs of
    # every library the binary links (libQt6Widgets.so.6, ...). Any RPM
    # distribution that provides those libraries satisfies the package, which
    # keeps it usable on Fedora, RHEL and openSUSE alike.
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