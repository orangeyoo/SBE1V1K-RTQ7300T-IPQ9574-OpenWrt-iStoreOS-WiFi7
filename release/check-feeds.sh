#!/bin/sh
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
# Feeds must sit exactly on the commits in release/pins/feeds.lock, with only
# the release/patches/feeds/<name> changes applied on top.
set -eu
cd "$(dirname "$0")/.."
rc=0
grep -v '^#' release/pins/feeds.lock | while read -r name url commit; do
	[ -n "$name" ] || continue
	d=feeds/$name
	head=$(git -C "$d" rev-parse HEAD 2>/dev/null || echo none)
	if [ "$head" != "$commit" ]; then
		echo "check-feeds: $name at $head, expected $commit"; exit 1
	fi
	want=$(cat release/patches/feeds/"$name"/*.patch 2>/dev/null | grep -E '^\+\+\+ b/' | sed 's|^+++ b/||' | sort -u)
	have=$(git -C "$d" status --porcelain | awk '{print $2}' | sort -u)
	if [ "$want" != "$have" ]; then
		echo "check-feeds: $name worktree changes differ from release patches"
		echo "  patched: $want"; echo "  actual:  $have"; exit 1
	fi
	echo "check-feeds: $name OK $commit"
done || rc=1
exit $rc
