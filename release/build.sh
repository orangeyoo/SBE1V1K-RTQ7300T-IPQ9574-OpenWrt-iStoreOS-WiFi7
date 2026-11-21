#!/bin/bash
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
# SBE1V1K release build: clean-tree gate -> pinned feeds -> seed config -> world
# -> image verification -> release artifacts (kernel32 .bin).
#
# Usage: release/build.sh [--allow-dirty] [-j N]
#   Version = git tag on HEAD (v1.0, v1.0-rc1, ...). Untagged/dirty trees are
#   refused unless --allow-dirty (development builds, version gets a -dev-<sha>
#   suffix and the result can never pass release/gate.sh).
# Env: SOURCE_DATE_EPOCH defaults to the HEAD commit time.
set -euo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD
ALLOW_DIRTY=0; JOBS=$(nproc)
while [ $# -gt 0 ]; do
	case "$1" in
		--allow-dirty) ALLOW_DIRTY=1; shift ;;
		-j) JOBS=$2; shift 2 ;;
		*) echo "unknown arg $1" >&2; exit 2 ;;
	esac
done
LOG=$TOP/logs/release; mkdir -p "$LOG"
say() { echo "[build] $*"; }
die() { echo "[build] FAIL: $*" >&2; exit 1; }

# ---- 1. source state -------------------------------------------------------
HEAD=$(git rev-parse HEAD)
dirty=$(git status --porcelain --untracked-files=normal)
tag=$(git describe --exact-match --tags --match 'v*' HEAD 2>/dev/null || true)
if [ -n "$dirty" ] || [ -z "$tag" ]; then
	[ "$ALLOW_DIRTY" = 1 ] || die "tree not releasable (dirty=$( [ -n "$dirty" ] && echo yes || echo no ) tag=${tag:-none}); commit + tag, or pass --allow-dirty"
	VERSION="${tag:-v1.0}"; VERSION="${VERSION#v}-dev-$(git rev-parse --short=9 HEAD)"
	[ -n "$dirty" ] && VERSION="$VERSION-dirty"
else
	VERSION="${tag#v}"
fi
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct HEAD)}
say "version=$VERSION head=$HEAD epoch=$SOURCE_DATE_EPOCH"

# ---- 2. pinned inputs ------------------------------------------------------
sha256sum -c --quiet release/pins/binaries.sha256 || die "vendored binary differs from release/pins/binaries.sha256"
release/feeds.sh > "$LOG/feeds.log" 2>&1 || die "feeds (see $LOG/feeds.log)"
release/check-feeds.sh || die "feeds not at pinned commits"

# ---- 3. config -------------------------------------------------------------
# Target/package metadata caches do not notice every Makefile change (a
# modified target.mk kept its old DEFAULT_PACKAGES); rebuild them each time.
rm -f tmp/.targetinfo tmp/.config-target.in tmp/info/.targetinfo-*
make prepare-tmpinfo > "$LOG/tmpinfo.log" 2>&1 || die "prepare-tmpinfo"
cp release/config/sbe1v1k.seed .config
printf 'CONFIG_VERSION_NUMBER="%s"\nCONFIG_VERSION_CODE="%s"\n' "$VERSION" "$(git rev-parse --short=9 HEAD)" >> .config
make defconfig > "$LOG/defconfig.log" 2>&1 || die defconfig
release/check-config.sh .config || die "config drift after defconfig"

# ---- 4. build --------------------------------------------------------------
# Packages built from a pre-patched tree under qca/src (LOCAL_SRC copied in
# Build/Prepare) are not rebuilt when only that tree changes: a hostapd
# fix in qca/src never reached the image (2026-11-13). Clean them when the
# git tree id of their source dir differs from the last build.
for spec in \
	qca/src/network/services/hostapd:package/feeds/qca_hostapd/hostapd \
	qca/src/mac80211:package/feeds/qca_wlan_open/mac80211; do
	src=${spec%%:*}; pkg=${spec#*:}
	id=$(git rev-parse "HEAD:$src")
	stamp="tmp/.sbe-src-$(echo "$src" | tr / _)"
	if [ "$(cat "$stamp" 2>/dev/null)" != "$id" ]; then
		say "$src changed: cleaning $pkg"
		make "$pkg/clean" > /dev/null 2>&1 || true
		echo "$id" > "$stamp"
	fi
done
# base-files carries the version string; force it so a cached ipk can't lag.
make package/base-files/clean > /dev/null 2>&1 || true
make -j"$JOBS" download > "$LOG/download.log" 2>&1 || die "download (offline? see $LOG/download.log)"
release/check-dl.sh || die "dl/ content differs from release/pins/dl.sha256"
# BUILD_LOG=1 keeps a per-package log under logs/package/ so a parallel
# failure can be diagnosed afterwards (it does not change the image).
if ! make -j"$JOBS" world BUILD_LOG=1 > "$LOG/world.log" 2>&1; then
	say "parallel world failed: $(grep -c 'ERROR: ' "$LOG/world.log") package(s); retrying serially"
	grep 'ERROR: ' "$LOG/world.log" | sed 's/^ */[build]   /'
	make -j1 world BUILD_LOG=1 > "$LOG/world-j1.log" 2>&1 || die "world (see $LOG/world-j1.log)"
fi

# ---- 4b. offline core feed -------------------------------------------------
# iStore apps depend on target/core packages (block-mount, kmod-*) that exist
# for this build's libc arch (aarch64_cortex-a73_neon-vfpv4) on no public
# server: ipq95xx is not an OpenWrt target and the generated 24.10-SNAPSHOT
# URLs are 404. 96-sbe-environment therefore points opkg at the OpenWrt a53
# generic feeds, which carry no target packages at all, so every such
# dependency failed ("cannot find dependency block-mount", 2026-11-16).
# Ship the target feed this build just produced inside the image and let opkg
# read it over file://; its index is signed with key-build, whose pubkey the
# image already trusts (/etc/opkg/keys/cd23f0494b4f17df).
# The feed only exists once world has built every package, so it is staged
# after the build and the image is re-packed. It goes into TARGET_DIR, which
# Image/mkfs copies verbatim (include/image.mk "$(CP) $(TARGET_DIR_ORIG)
# $(mkfs_cur_target_dir)"), not into $TOPDIR/files: prepare_rootfs is handed
# that directory but its copy never reached the image here (measured
# 2026-11-16, rootfs byte-identical with 318 packages staged). Removing
# root.squashfs is what forces mkfs to run again - target/install alone is a
# no-op because no package changed and make does not track these files.
CORE_SRC=bin/targets/ipq95xx/generic/packages
TARGET_DIR=$(ls -d build_dir/target-*/root-ipq95xx 2>/dev/null | head -1)
KDIR=$(ls -d build_dir/target-*/linux-ipq95xx_generic 2>/dev/null | head -1)
[ -f "$CORE_SRC/Packages.sig" ] || die "no signed target feed in $CORE_SRC"
[ -d "$TARGET_DIR" ] || die "no TARGET_DIR (build_dir/target-*/root-ipq95xx)"
[ -d "$KDIR" ] || die "no KDIR (build_dir/target-*/linux-ipq95xx_generic)"
CORE_DST=$TARGET_DIR/usr/lib/sbe-core-feed
rm -rf "$CORE_DST"
mkdir -p "$CORE_DST"
cp "$CORE_SRC"/*.ipk "$CORE_SRC"/Packages "$CORE_SRC"/Packages.gz \
   "$CORE_SRC"/Packages.sig "$CORE_DST"/
say "offline core feed: $(ls "$CORE_DST"/*.ipk | wc -l) packages, $(du -sh "$CORE_DST" | cut -f1)"
rm -f "$KDIR"/root.squashfs
make -j"$JOBS" target/install > "$LOG/image-refresh.log" 2>&1 ||
	die "image re-assembly (see $LOG/image-refresh.log)"

# ---- 5. artifacts ----------------------------------------------------------
release/mkrelease.sh "$VERSION" || die "artifact verification"
say "OK $VERSION -> $TOP/bin/release/$VERSION"
