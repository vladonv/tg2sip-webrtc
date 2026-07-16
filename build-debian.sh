#!/bin/bash
# Build tg2sip-webrtc from scratch on a clean Debian install, using only
# official Debian repo packages for everything except PJSIP, TDLib and
# tgcalls/tg_owt (none of which are packaged in Debian, so those are built
# from source into a private prefix under $DEPS_DIR, not /usr/local - see
# the LOCAL_PREFIX comment below for why).
#
# Verified 2026-07-15 on two clean boxes with byte-for-byte identical steps:
#   - Debian 13 (trixie)
#   - Debian 12 (bookworm)
# No Debian-version-specific branching was needed despite a ~2 year gap in
# FFmpeg/absl/spdlog/GCC versions between the two releases - hence one
# script instead of separate bookworm/trixie variants.
#
# Usage: run as root, from anywhere inside a clone of this repo:
#   ./build-debian.sh
#
# Produces build/tg2sip-webrtc and build/tg2sip-gendb.

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root (installs apt packages)." >&2
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_DIR="${DEPS_DIR:-$HOME/tg2sip-build-deps}"
# PJSIP and TDLib are NOT packaged in Debian, so they're built from source -
# but 'make/cmake install'-ing them into /usr/local would litter a packaged
# distro with files dpkg doesn't know about (no 'apt remove' cleanup, risk of
# colliding with a future distro package at the same path). Both end up
# statically linked into tg2sip-webrtc (.a only, no .so consumed at runtime),
# so there's no reason they need to live anywhere system-wide at all - they
# install into this private prefix instead, found via PKG_CONFIG_PATH /
# CMAKE_PREFIX_PATH below. 'rm -rf "$DEPS_DIR"' undoes everything from steps
# 3-4 with nothing left on the rest of the system.
LOCAL_PREFIX="$DEPS_DIR/local"
PJSIP_JOBS="${PJSIP_JOBS:-$(nproc)}"
# TDLib/tg_owt compile units are heavier per-TU than PJSIP's; cap concurrency
# to stay safe on small (~8GB) boxes rather than assume nproc-wide is fine.
BUILD_JOBS="${BUILD_JOBS:-4}"

echo "==> repo root: $REPO_ROOT"
echo "==> deps build dir: $DEPS_DIR"
mkdir -p "$DEPS_DIR"

echo "==> [1/6] Installing apt build dependencies (official Debian repos only)"
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    git build-essential cmake ninja-build pkg-config gperf yasm nasm ca-certificates \
    libjpeg-dev libopus-dev libasound2-dev libpulse-dev \
    libx11-dev libxext-dev libxdamage-dev libxfixes-dev libxrender-dev libxrandr-dev libxtst-dev libxcomposite-dev \
    libevent-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    libpipewire-0.3-dev libabsl-dev libvpx-dev \
    libssl-dev zlib1g-dev libspdlog-dev libfmt-dev
# NOTE: deliberately NOT installing libyuv-dev - tg_owt's CMakeLists.txt
# unconditionally builds libyuv from the vendored submodule with no
# pkg-config fallback, so the system package would never be used anyway.
# libvpx-dev IS used (see step 2's submodule note) when present - it's just
# an optimization (system lib preferred over compiling the vendored one),
# not a hard requirement.

echo "==> [2/6] Initializing required git submodules (selective - see comments)"
cd "$REPO_ROOT"
git submodule update --init third_party/tgcalls
cd third_party/tgcalls
# 'cmake' here is NOT the build tool (already installed via apt above) - it's
# the desktop-app/cmake_helpers submodule, a set of .cmake scripts tgcalls's
# own build files include(). Mandatory.
git submodule update --init cmake
# libvpx/libyuv: required even though libvpx-dev/(never) libyuv-dev may
# satisfy the actual linking - tg_owt's CMakeLists.txt unconditionally
# add_library()s an OBJECT target from these submodules' source trees, and
# add_library() fails at configure time with zero source files present,
# before any pkg-config check runs. A shallow clone is enough since only the
# current tree (not history) is needed.
git submodule update --init --depth 1 \
    tgcalls/third_party/webrtc/src/third_party/libyuv \
    tgcalls/third_party/webrtc/src/third_party/libvpx/source/libvpx
# Deliberately NOT initializing tgcalls/third_party/pybind11 - unused, this
# project doesn't build tgcalls' Python bindings.
cd "$REPO_ROOT"

echo "==> [3/6] Building PJSIP 2.9 from source"
if [ ! -d "$DEPS_DIR/pjproject" ]; then
    git clone --depth 1 --branch 2.9 https://github.com/pjsip/pjproject.git "$DEPS_DIR/pjproject"
fi
cd "$DEPS_DIR/pjproject"
cp "$REPO_ROOT/buildenv/config_site.h" pjlib/include/pj/
./configure --prefix="$LOCAL_PREFIX" --disable-sound CFLAGS="-O3 -DNDEBUG -fPIC"
make dep
make -j"$PJSIP_JOBS"
make install

echo "==> [4/6] Building TDLib from source (master - the repo's pinned 1.7.10"
echo "    commit is rejected by current Telegram servers with UPDATE_APP_TO_LOGIN)"
if [ ! -d "$DEPS_DIR/td" ]; then
    git clone https://github.com/tdlib/td.git "$DEPS_DIR/td"
fi
mkdir -p "$DEPS_DIR/td/build"
cd "$DEPS_DIR/td/build"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$LOCAL_PREFIX" -G Ninja ..
cmake --build . --target install -j"$BUILD_JOBS"

echo "==> [5/6] Configuring tg2sip-webrtc"
cd "$REPO_ROOT"
# Point pkg-config (PJSIP, found via pkg_check_modules) and CMake's own
# find_package (Td) at the private prefix from steps 3-4 instead of /usr/local.
export PKG_CONFIG_PATH="$LOCAL_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="$LOCAL_PREFIX"

echo "==> [6/6] Building tg2sip-webrtc"
cmake --build build -j"$BUILD_JOBS"

echo "==> Done. Binaries:"
ls -la "$REPO_ROOT/build/tg2sip-webrtc" "$REPO_ROOT/build/tg2sip-gendb"
echo "==> Note: these are unstripped debug builds (intentional - see repo notes"
echo "    on the iOS-callee silent-audio bug). Run 'strip' on a separate copy"
echo "    before shipping a production/deployment artifact."
