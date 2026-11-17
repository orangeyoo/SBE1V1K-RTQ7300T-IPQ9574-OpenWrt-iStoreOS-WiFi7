#!/bin/bash
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
# Assemble and verify release artifacts for one built version.
# Output: bin/release/<version>/
#   sbe1v1k-<v>-sysupgrade.bin   kernel32 raw image: the only flashable deliverable (recovery page, sysupgrade, LuCI)
#   sbe1v1k-<v>-fit-uImage.itb, sbe1v1k-<v>-root.squashfs, sbe1v1k-<v>-fdt1.dtb
#   manifest/{packages.txt,config,seed,pins/*,source.txt}, SHA256SUMS, verify.txt
set -euo pipefail
cd "$(dirname "$0")/.."
V=$1
IMG=bin/targets/ipq95xx/generic
OUT=bin/release/$V
rm -rf "$OUT"; mkdir -p "$OUT/manifest"
pfx="$IMG/istoreos-sbe1v1k-orangeyoo-$V-"
SU=$(ls "$pfx"*-orangeyoo_sbe1v1k-squashfs-sysupgrade.bin)
FIT=$(ls "$pfx"*-orangeyoo_sbe1v1k-fit-uImage.itb)
MAN=$(ls "$pfx"*-orangeyoo_sbe1v1k.manifest 2>/dev/null || ls "$pfx"*.manifest)
B=sbe1v1k-$V
MiB=1048576
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0
chk() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails+1)); fi; }

{
# ---- raw kernel32 layout (webfix recovery + platform.sh) -----------------
sz=$(stat -c %s "$SU")
chk "R1 FIT magic d00dfeed"            '[ "$(head -c4 "$SU" | xxd -p)" = d00dfeed ]'
chk "R2 size in (32MiB,1GiB] ($sz)"    '[ $sz -gt $((32*MiB)) ] && [ $sz -le $((1024*MiB)) ]'
chk "R3a 7MiB zero"                    '[ "$(dd if="$SU" bs=1 skip=$((7*MiB)) count=4096 status=none | tr -d "\0" | wc -c)" = 0 ]'
chk "R3b 32MiB = hsqs"                 '[ "$(dd if="$SU" bs=1 skip=$((32*MiB)) count=4 status=none)" = hsqs ]'
dd if="$SU" of="$W/root.squashfs" bs=$MiB skip=32 status=none
read -r used ver < <(python3 -c "import struct,sys;d=open(sys.argv[1],'rb').read(96);print(struct.unpack_from('<Q',d,40)[0],struct.unpack_from('<H',d,28)[0])" "$W/root.squashfs")
chk "R4 squashfs v4 bytes_used<=payload" '[ "$ver" = 4 ] && [ $used -le $((sz-32*MiB)) ]'
fs=$(stat -c %s "$FIT")
chk "R6 embedded FIT == itb"           'cmp -s -n $fs "$SU" "$FIT"'
truncate -s "$used" "$W/root.squashfs"
# in-OS sysupgrade matches this list against the board's own name
staging_dir/host/bin/fwtool -i "$W/meta.json" "$SU" 2> /dev/null
chk "M1 metadata supports askey,sbe1v1k" 'grep -q "\"askey,sbe1v1k\"" "$W/meta.json"'
chk "M2 metadata version = $V"         'grep -q "\"version\": \"$V\"" "$W/meta.json"'

# ---- FIT shape --------------------------------------------------------------
dumpimage -l "$FIT" > "$W/fit.txt"
chk "F1 kernel@1/fdt@1/conf@1"         'grep -q "(kernel@1)" "$W/fit.txt" && grep -q "(fdt@1)" "$W/fit.txt" && grep -q "Default Configuration: .conf@1." "$W/fit.txt"'
chk "F2 lzma + load=entry=0x42000000"  'grep -q "lzma compressed" "$W/fit.txt" && grep -q "Load Address: 0x42000000" "$W/fit.txt" && grep -q "Entry Point:  0x42000000" "$W/fit.txt"'
idx=$(grep -n "(fdt@1)" "$W/fit.txt" | sed -E 's/.*Image ([0-9]+).*/\1/')
dumpimage -T flat_dt -p "$idx" -o "$OUT/$B-fdt1.dtb" "$FIT" > /dev/null
dtc -I dtb -O dts "$OUT/$B-fdt1.dtb" -o "$W/fdt1.dts" 2> /dev/null
chk "D1 compatible askey,sbe1v1k"      'grep -q "compatible = \"askey,sbe1v1k" "$W/fdt1.dts"'
chk "D2 PHY map 0x14/0x15/0x12/0x13/0x1c/0x00" '[ "$(grep -o "phy_address = <0x[0-9a-f]*>" "$W/fdt1.dts" | tr -d "\n")" = "phy_address = <0x14>phy_address = <0x15>phy_address = <0x12>phy_address = <0x13>phy_address = <0x1c>phy_address = <0x00>" ]'
chk "D4 PPE-DS: 3 nodes, 46 EDMA IRQs"  'grep -q "qcom,ppeds-num = <0x03>" "$W/fdt1.dts" && [ "$(awk "/edma@3ab00000/,/};/" "$W/fdt1.dts" | grep -o "0x00 0x[0-9a-f]* 0x04" | wc -l)" = 46 ]'
chk "D6 WAN port on GMAC (QGMAC_PORT)" 'grep -q "port_mac_sel = \"QGMAC_PORT\"" "$W/fdt1.dts"'
chk "D5 status LED on pwm-leds"        'grep -q "compatible = \"pwm-leds\"" "$W/fdt1.dts"'
chk "D3 no reset-gpios on 10G PHY"     '! awk "/ethernet-phy@0 \{/,/\};/" "$W/fdt1.dts" | grep -q reset-gpios'

# ---- rootfs content ---------------------------------------------------------
unsquashfs -q -d "$W/r" "$W/root.squashfs" > /dev/null
R=$W/r
chk "C1 release string = $V"           'grep -q "^DISTRIB_RELEASE=.$V.$" "$R/etc/openwrt_release"'
chk "C2 no syslog-ng (logd only)"      '[ ! -e "$R/usr/sbin/syslog-ng" ] && [ -x "$R/sbin/logd" ]'
chk "C3 no bluetooth stack"            '[ ! -e "$R/usr/bin/cpcd" ] && [ ! -e "$R/usr/sbin/bluetoothd" ]'
chk "C4 orangeyoo-defaults present"    '[ -x "$R/usr/sbin/factory-wireless-layout" ]'
chk "C6 port LEDs + offload hotplug" '[ -x "$R/etc/init.d/sbe-port-leds" ] && [ -f "$R/etc/hotplug.d/iface/30-sbe-offload" ]'
chk "C7 offline core feed (block-mount + signed index)" '[ -f "$R/usr/lib/sbe-core-feed/Packages.sig" ] && ls "$R"/usr/lib/sbe-core-feed/block-mount_*.ipk > /dev/null 2>&1 && [ "$(ls "$R"/usr/lib/sbe-core-feed/kmod-*.ipk | wc -l)" -gt 100 ]'
chk "C8 opkg reads the offline feed" 'grep -q "^src/gz sbe_core file:///usr/lib/sbe-core-feed$" "$R/etc/uci-defaults/96-sbe-environment"'
chk "C9 offline feed reaches upgraded units" '[ -x "$R/etc/uci-defaults/97-sbe-core-feed" ]'
chk "C5 ath12k + QCN9224 firmware"     '[ -f "$R/lib/firmware/ath12k/QCN92XX/hw1.0/amss.bin" ] && [ -f "$R/lib/firmware/ath12k/QCN92XX/hw1.0/board-2.bin" ]'
grep '^Package: ' "$R/usr/lib/opkg/status" | sed 's/^Package: //' | LC_ALL=C sort > "$OUT/manifest/packages.txt"

} 2>&1 | tee "$OUT/verify.txt"
fails=$(grep -c '^FAIL' "$OUT/verify.txt" || true)

cp "$SU" "$OUT/$B-sysupgrade.bin"
cp "$FIT" "$OUT/$B-fit-uImage.itb"
dd if="$SU" of="$OUT/$B-root.squashfs" bs=$MiB skip=32 status=none
truncate -s "$(python3 -c "import struct,sys;print(struct.unpack_from('<Q',open(sys.argv[1],'rb').read(48),40)[0])" "$OUT/$B-root.squashfs")" "$OUT/$B-root.squashfs"
cp .config "$OUT/manifest/config"
cp release/config/sbe1v1k.seed "$OUT/manifest/seed"
cp -r release/pins "$OUT/manifest/pins"
cp "$MAN" "$OUT/manifest/opkg.manifest"
{
	echo "version=$V"
	echo "source_commit=$(git rev-parse HEAD)"
	echo "source_tag=$(git describe --tags --exact-match HEAD 2>/dev/null || echo none)"
	echo "qsdk_base=$(git rev-parse 21a8d1339)"
	echo "source_date_epoch=${SOURCE_DATE_EPOCH:-unset}"
	for d in feeds/packages feeds/luci feeds/routing feeds/telephony; do echo "feed_${d#feeds/}=$(git -C "$d" rev-parse HEAD)"; done
	echo "toolchain=$(ls -d staging_dir/toolchain-* | sed 's|.*/||')"
	echo "host=$(uname -srm); $(. /etc/os-release; echo "$PRETTY_NAME")"
} > "$OUT/manifest/source.txt"
(cd "$OUT" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | xargs sha256sum > SHA256SUMS)
echo "mkrelease: $V fails=$fails -> $OUT"
[ "$fails" = 0 ]
