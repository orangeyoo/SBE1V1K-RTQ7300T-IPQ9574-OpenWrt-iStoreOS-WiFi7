#!/bin/sh
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
# Pinned OpenWrt feeds -> feeds.conf, checkout, patch, install.
#
# Usage: release/feeds.sh            (run from the tree root)
# Env:   FEEDS_MIRROR=/path   use <path>/<name>.git instead of the upstream URL
#                             (offline / clean-room rebuild); default
#                             /work/mirrors/feeds when that directory exists.
#
# feeds.conf is generated (git-ignored); the pins live in release/pins/feeds.lock.
set -eu
cd "$(dirname "$0")/.."
LOCK=release/pins/feeds.lock
PATCHES=release/patches/feeds
: "${FEEDS_MIRROR:=$([ -d /work/mirrors/feeds ] && echo /work/mirrors/feeds || true)}"

{
	grep -v '^#' "$LOCK" | while read -r name url commit; do
		[ -n "$name" ] || continue
		[ -n "$FEEDS_MIRROR" ] && url="$FEEDS_MIRROR/$name.git"
		echo "src-git $name $url^$commit"
	done
	cat release/pins/feeds.links
} > feeds.conf

./scripts/feeds update -a

grep -v '^#' "$LOCK" | while read -r name url commit; do
	[ -n "$name" ] || continue
	d=feeds/$name
	git -C "$d" checkout -q --detach "$commit"
	git -C "$d" reset -q --hard "$commit"
	git -C "$d" clean -qfdx
	if [ -d "$PATCHES/$name" ]; then
		for p in "$PATCHES/$name"/*.patch; do
			patch -d "$d" -p1 -s --no-backup-if-mismatch < "$p"
			echo "feeds: $name <- $(basename "$p")"
		done
	fi
	[ "$(git -C "$d" rev-parse HEAD)" = "$commit" ] || { echo "feeds: $name not at $commit" >&2; exit 1; }
done

./scripts/feeds update -i -a
./scripts/feeds install -a
# QCA feed packages that replace core OpenWrt packages (same set the
# 2.0-pre36 build used, see CONFIG_OVERRIDE_PKGS): ath12k mac80211 backports
# and its wifi-scripts, NSS dataplane + switch driver, QCA hostapd/wpad.
# (PassWall cores come straight from the pinned packages feed.)
./scripts/feeds install -f -p qca_wlan_open mac80211 wifi-scripts
./scripts/feeds install -f -p qca_nss qca-nss-dp qca-ssdk
./scripts/feeds install -f -p qca_hostapd hostapd

