#!/usr/bin/env bash
#
# Build LXR for sndstation and package it as a .deb for a unit:
#
#   /usr/bin/lxr-sndstation                 the app
#   /usr/share/sndstation/apps/lxr.json     its manifest: the launcher lists it
#   /usr/share/lxr-sndstation/fonts/        its typeface, Open Sans, and the licence
#   /usr/share/doc/lxr-sndstation/copyright
#
#   sndstation/packaging/make-deb.sh --sndstation-dir DIR
#       --sndstation-dir DIR  the sndstation tree to build against (with its submodules)
#       --build-dir DIR       where to build (default: sndstation/build-deb)
#       --out DIR             where the .deb goes (default: sndstation/dist)
#       --revision R          the package revision (default: when sndstation/ or
#                             native/ last changed, UTC YYYYMMDDHHMM, +local when
#                             uncommitted; 1 out of git)
#       --needs-sndstation V  the oldest sndstation release it depends on
#                             (default: 0.15.1)
#       --no-tests            skip the tests
#       --quiet               no build output
#
# Build it where the platform's own release is built, so it installs where the
# platform does: arm64 Ubuntu 22.04 (sndstation's docs/project/release-process.md),
# which makes a package for Ubuntu 22.04 and later and Debian bookworm and later.
# What has to be installed there:
#
#   build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev
#   libasound2-dev
#
# Then, on the unit: sudo apt install ./lxr-sndstation_<version>_arm64.deb
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)   # sndstation/ in the LXR repo
cd "$here"

PKG=lxr-sndstation
sndstation_dir=""
build_dir=build-deb
out_dir=dist
revision=auto
# the oldest platform it is checked against: 0.15.1, the release on the units
# (2026-11-22), whose config/ differs from 0.16.0's in comments only
needs=0.15.1
tests=1
quiet=0
maintainer="LXR port contributors"

die() { printf 'make-deb: %s\n' "$*" >&2; exit 1; }
step() { printf '\n== %s\n' "$*"; }
note() { printf '  %s\n' "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --sndstation-dir) sndstation_dir=$2; shift 2 ;;
        --build-dir)      build_dir=$2; shift 2 ;;
        --out)            out_dir=$2; shift 2 ;;
        --revision)       revision=$2; shift 2 ;;
        --needs-sndstation) needs=$2; shift 2 ;;
        --no-tests)       tests=0; shift ;;
        --quiet)          quiet=1; shift ;;
        -h|--help)        sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)                die "unknown option $1" ;;
    esac
done
[ -n "$sndstation_dir" ] || die "--sndstation-dir names the sndstation tree to build against"
[ -f "$sndstation_dir/include/sndstation/sndstation.h" ] || die "$sndstation_dir is not a sndstation tree"

# Everything missing at once, not the first thing.
missing=()
for t in cmake c++ pkg-config dpkg-deb dpkg-shlibdeps readelf; do
    command -v "$t" >/dev/null || missing+=("$t")
done
if command -v pkg-config >/dev/null; then
    for p in sdl2 SDL2_ttf SDL2_image alsa; do pkg-config --exists "$p" || missing+=("$p (pkg-config)"); done
fi
[ ${#missing[@]} -eq 0 ] || die "missing: ${missing[*]}"

# ── version: the project's, and a revision that moves by itself ──
version=$(sed -n 's/^project(lxr-sndstation VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$version" ] || die "could not read the version from CMakeLists.txt"
if [ "$revision" = auto ]; then
    revision=$(cd .. && TZ=UTC git log -1 --format=%cd --date=format-local:%Y%m%d%H%M -- sndstation native 2>/dev/null || true)
    if [ -z "$revision" ]; then
        revision=1
    elif [ -n "$(cd .. && git status --porcelain -- sndstation native 2>/dev/null)" ]; then
        revision="$revision+local"
    fi
fi
full="$version-$revision"
plat_version=$(sed -n 's/^set(SNDSTATION_VERSION \([0-9.]*\)).*/\1/p' "$sndstation_dir/CMakeLists.txt")
[ -n "$plat_version" ] || die "could not read SNDSTATION_VERSION from $sndstation_dir/CMakeLists.txt"

# ── build, test, install into a stage ──
jobs=$(nproc 2>/dev/null || echo 2)
run() { if [ "$quiet" = 1 ]; then "$@" >/dev/null; else "$@"; fi; }
step "building $PKG $full against sndstation $plat_version (for $needs and later) in $build_dir/"
run cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DSNDSTATION_DIR="$sndstation_dir" \
    -DLXR_SNDSTATION_TESTS="$([ "$tests" = 1 ] && echo ON || echo OFF)"
run cmake --build "$build_dir" -j"$jobs"
if [ "$tests" = 1 ]; then
    step "testing"
    ctest --test-dir "$build_dir" --output-on-failure
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
chmod 0755 "$stage"   # the archive's root is the unit's /
DESTDIR="$stage" run cmake --install "$build_dir" --prefix /usr --strip
install -d -m 0755 "$stage/DEBIAN" "$stage/usr/share/doc/$PKG"
cat > "$stage/usr/share/doc/$PKG/copyright" <<'COPYRIGHT'
LXR for sndstation: the LXR V2 drum machine's sound engine and sequencer as an
app for sndstation handhelds.

Files: *
License: GPL-3.0-only (/usr/share/common-licenses/GPL-3)
 "LXR" and "Erica Synths" are trademarks of Erica Synths, which the GPL does
 not grant; this package is not made or endorsed by Erica Synths.

Files: usr/share/lxr-sndstation/fonts/*
Copyright: 2020 The Open Sans Project Authors
License: OFL-1.1 (usr/share/lxr-sndstation/fonts/OpenSans-OFL.txt)
COPYRIGHT
find "$stage/usr" -type d -exec chmod 0755 {} +
find "$stage/usr/share" -type f -exec chmod 0644 {} +

app="$stage/usr/bin/lxr-sndstation"
[ -x "$app" ] || die "the install put no /usr/bin/lxr-sndstation in the stage"

# ── architecture and dependencies, read off the binary ──
machine=$(readelf -h "$app" | sed -n 's/^ *Machine: *//p')
case "$machine" in
    *AArch64*) arch=arm64 ;;
    *X86-64*)  arch=amd64 ;;
    *)         die "cannot map ELF machine '$machine' to a dpkg architecture" ;;
esac
host_arch=$(dpkg --print-architecture)
[ "$arch" = "$host_arch" ] || die "built for $arch on $host_arch: dpkg-shlibdeps needs a native build"
sd=$(mktemp -d)
mkdir -p "$sd/debian"
printf 'Source: x\nPackage: x\nArchitecture: any\n' > "$sd/debian/control"
libs=$(cd "$sd" && dpkg-shlibdeps -O "$app" | sed -n 's/^shlibs:Depends=//p')
rm -rf "$sd"
[ -n "$libs" ] || die "dpkg-shlibdeps found no dependencies for $app"
# The platform: the launcher that lists it and the session that brings the
# launcher back when it exits. The SDK is linked in, so all it needs of the
# installed platform besides those is the config the SDK reads from
# /usr/share/sndstation/config. The floor is the oldest release checked for
# both, not the SDK's own version, which can be one not released yet.
depends="sndstation (>= $needs), $libs"

size=$(du -sk "$stage/usr" | cut -f1)
cat > "$stage/DEBIAN/control" <<CONTROL
Package: $PKG
Version: $full
Section: sound
Priority: optional
Architecture: $arch
Depends: $depends
Maintainer: $maintainer
Installed-Size: $size
Description: LXR drum machine for sndstation handhelds
 The LXR V2 drum machine's sound engine and sequencer, ported from its
 firmware: seven drum voices and an FX bus, 16 patterns of up to 64 steps
 with parameter locks, Euclid and shuffle, played from the handheld's buttons
 and from MIDI. Listed in the sndstation launcher.
CONTROL

# ── the package ──
step "packaging"
mkdir -p "$out_dir"
deb="$out_dir/${PKG}_${full}_${arch}.deb"
dpkg-deb --build --root-owner-group "$stage" "$deb" >/dev/null

# What it holds, asserted: what makes it work, nothing outside its own places,
# and no JetBrains face anywhere.
contents=$(dpkg-deb -c "$deb" | awk '{print $NF}')
for want in ./usr/bin/lxr-sndstation ./usr/share/sndstation/apps/lxr.json \
            ./usr/share/lxr-sndstation/fonts/OpenSans-Regular.ttf \
            ./usr/share/lxr-sndstation/fonts/OpenSans-Bold.ttf \
            ./usr/share/lxr-sndstation/fonts/OpenSans-OFL.txt \
            "./usr/share/doc/$PKG/copyright"; do
    printf '%s\n' "$contents" | grep -qx "$want" || die "$deb has no $want"
done
if printf '%s\n' "$contents" | grep -vE '^\./(usr/(bin/(lxr-sndstation)?|share/(sndstation/(apps/(lxr\.json)?)?|lxr-sndstation/(fonts/.*)?|doc/(lxr-sndstation/(copyright)?)?)?)?)?$'; then
    die "$deb holds something outside its own places (above)"
fi
if printf '%s\n' "$contents" | grep -qi jetbrains; then die "$deb names a JetBrains face"; fi
dpkg-deb -c "$deb" | awk '$NF == "./" && $1 != "drwxr-xr-x" {exit 1}' || die "the archive root is not 0755"

echo
echo "$deb"
note "version      $full"
note "architecture $arch ($machine)"
note "depends      $depends"
note "installed    $size KiB"
note "glibc        $(readelf -V "$app" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1) at most"
echo
echo "  on the unit: sudo apt install ./$(basename "$deb")"
