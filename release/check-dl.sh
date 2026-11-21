#!/bin/sh
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
# Every source archive that make download fetched must match release/pins/dl.sha256.
# Archives not listed are reported (new package) and fail the check.
# Usage: release/check-dl.sh [--update]   (--update rewrites the pin file)
set -eu
cd "$(dirname "$0")/.."
TOP=$PWD
PIN=release/pins/dl.sha256
if [ "${1:-}" = --update ]; then
	(cd dl && find . -maxdepth 1 -type f ! -name '*.flock' ! -name '.*' -printf '%f\n' | LC_ALL=C sort | xargs sha256sum) > "$PIN"
	echo "check-dl: wrote $(wc -l < "$PIN") entries"; exit 0
fi
fail=0
(cd dl && sha256sum -c --quiet "$TOP/$PIN") || fail=1
awk '{print $2}' "$PIN" | LC_ALL=C sort > /tmp/.dlpins
extra=$(cd dl && find . -maxdepth 1 -type f ! -name '*.flock' ! -name '.*' -printf '%f\n' | LC_ALL=C sort | comm -23 - /tmp/.dlpins)
if [ -n "$extra" ]; then echo "check-dl: unpinned archives:"; echo "$extra"; fail=1; fi
[ $fail = 0 ] && echo "check-dl: OK $(wc -l < "$PIN") archives"
exit $fail
