#!/bin/bash
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
# Release gate. Every check must PASS before a build may be published.
#
# Usage: release/gate.sh <version> [--selftest]
#   reads bin/release/<version>/ (produced by release/build.sh -> mkrelease.sh)
#   writes bin/release/<version>/gate.txt; exit 0 only if all checks pass.
#
# Checks
#   G1 source tree clean, HEAD tagged v<version>, version string matches tag
#   G2 feeds / vendored binaries / dl archives equal to release/pins
#   G3 mkrelease verify.txt all PASS (kernel32 layout, metadata, FIT, fdt, rootfs)
#   G4 private data scan  (release/scan-rules/private.rules) over rootfs + image + source
#   G5 customer UI wording (release/scan-rules/ui-words.rules) over UI surfaces
#   G6 SHA256SUMS consistent
set -uo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD
V=${1:?version}
OUT=bin/release/$V
RULES=release/scan-rules
fails=0
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
res() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails+1)); fi; }

# allow.list: <path-regex> <tab> <rule-id-regex> <tab> reason (reason mandatory)
allowed() { # $1=path $2=rule-id
	[ -f $RULES/allow.list ] || return 1
	grep -v '^#' $RULES/allow.list | while IFS=$'\t' read -r pre rre why; do
		[ -n "$pre" ] && [ -n "$why" ] || continue
		echo "$1" | grep -qE "$pre" && echo "$2" | grep -qE "$rre" && { echo y; break; }
	done | grep -q y
}

scan_tree() { # $1=rules $2=dir $3=label $4=default scope regex(optional)  -> hits on stdout
	local id pat scope
	local visible=$(grep -m1 '^VISIBLE=' "$1" | cut -d= -f2-)
	grep -vE '^(#|VISIBLE=)' "$1" | while IFS=$'\t' read -r id pat scope; do
		[ -n "$id" ] || continue
		[ "$scope" = VISIBLE ] && scope=$visible
		[ -n "$scope" ] || scope=${4:-}
		grep -rEIil --exclude-dir=.git -e "$pat" "$2" 2>/dev/null | while read -r f; do
			rel=${f#"$2"/}
			[ -n "$scope" ] && ! echo "/$rel" | grep -qE "$scope" && continue
			allowed "$3:/$rel" "$id" && continue
			echo "$id $3:/$rel :: $(grep -EIio -m1 -e "$pat" "$f" | head -n1 | head -c 80)"
		done
	done
}

{
echo "# gate $V  $(date -u +%FT%TZ)"
# ---- G1 -------------------------------------------------------------------
dirty=$(git status --porcelain --untracked-files=normal | grep -v '^?? bin/' )
tag=$(git describe --exact-match --tags --match "v$V" HEAD 2>/dev/null || true)
[ -z "$dirty" ]; res "G1a source tree clean" $?
[ "$tag" = "v$V" ]; res "G1b HEAD tagged v$V (got ${tag:-none})" $?
grep -q "^source_commit=$(git rev-parse HEAD)$" "$OUT/manifest/source.txt" 2>/dev/null; res "G1c artifacts built from HEAD" $?

# ---- G2 -------------------------------------------------------------------
release/check-feeds.sh > "$W/feeds.txt" 2>&1; res "G2a feeds at pinned commits" $?
sha256sum -c --quiet release/pins/binaries.sha256 > "$W/bins.txt" 2>&1; res "G2b vendored binaries match pins" $?
release/check-dl.sh > "$W/dl.txt" 2>&1; res "G2c dl archives match pins" $?
diff -q release/pins/feeds.lock "$OUT/manifest/pins/feeds.lock" > /dev/null; res "G2d pins archived with artifacts" $?

# ---- G3 -------------------------------------------------------------------
[ -f "$OUT/verify.txt" ] && ! grep -q '^FAIL' "$OUT/verify.txt" && [ "$(grep -c '^PASS' "$OUT/verify.txt")" -ge 22 ]
res "G3 image verification ($(grep -c '^PASS' "$OUT/verify.txt" 2>/dev/null) PASS)" $?

# ---- G4 -------------------------------------------------------------------
unsquashfs -q -d "$W/r" "$OUT/sbe1v1k-$V-root.squashfs" > /dev/null 2>&1
scan_tree $RULES/private.rules "$W/r" rootfs > "$W/g4.txt"
# raw image: kernel/FIT/dtb strings (rootfs is covered by the readback above)
strings -a -n 6 "$OUT/sbe1v1k-$V-fit-uImage.itb" > "$W/fit.str"; mkdir -p "$W/s"; mv "$W/fit.str" "$W/s/"
scan_tree $RULES/private.rules "$W/s" fit >> "$W/g4.txt"
# tracked sources that end up in images
git ls-files -z package target/linux/feeds/ipq95xx target/linux/feeds/files-6.6 release/config | \
	xargs -0 grep -EIil -f <(grep -v '^#' $RULES/private.rules | cut -f2) 2>/dev/null | while read -r f; do
	grep -v '^#' $RULES/private.rules | while IFS=$'\t' read -r id pat; do
		grep -EIiq -e "$pat" "$f" || continue
		allowed "src:/$f" "$id" && continue
		echo "$id src:/$f :: $(grep -EIio -m1 -e "$pat" "$f" | head -n1 | head -c 80)"
	done
done >> "$W/g4.txt"
n4=$(wc -l < "$W/g4.txt"); [ "$n4" = 0 ]; res "G4 private data scan ($n4 hits)" $?
sed 's/^/  /' "$W/g4.txt" | head -50

# ---- G5 -------------------------------------------------------------------
UI='^/(www/|usr/share/luci/menu\.d/|usr/lib/lua/luci/(view|i18n)/|etc/banner$|etc/openwrt_release$|usr/lib/os-release$|etc/openwrt_version$)'
for f in $(find "$W/r/usr/lib/lua/luci/i18n" -name '*.lmo' 2>/dev/null); do strings -n 4 "$f" > "$f.txt"; done
scan_tree $RULES/ui-words.rules "$W/r" ui "$UI" > "$W/g5.txt"
# A release candidate (v<x>-rcN) legitimately shows its own version string;
# it is reported as a candidate, never as a final release.
case "$V" in
*-rc[0-9]*)
	grep -vE "^ver\.prerelease .* :: .*${V#*-}\$" "$W/g5.txt" > "$W/g5.f" || true; mv "$W/g5.f" "$W/g5.txt"
	CANDIDATE=1 ;;
esac
n5=$(wc -l < "$W/g5.txt"); [ "$n5" = 0 ]; res "G5 customer UI wording ($n5 hits)" $?
sed 's/^/  /' "$W/g5.txt" | head -50

# ---- G6 -------------------------------------------------------------------
(cd "$OUT" && sha256sum -c --quiet SHA256SUMS) > /dev/null 2>&1; res "G6 SHA256SUMS" $?

echo "# result: $([ $fails = 0 ] && echo "PASS${CANDIDATE:+ (release candidate - not a final release)}" || echo "FAIL ($fails)")"
} 2>&1 | tee "$OUT/gate.txt"
grep -q '^# result: PASS' "$OUT/gate.txt"
