#!/bin/sh
# SBE1V1K cal-pci writer (shared by preinit hook and START=01 init.d).
# Writes this unit's own 0:ART calibration slots (OEM ap-al02-c4 recipe:
# 184320B at 362496/567296/772096 = pcie1/2/3 radios, board_id 1/4/2)
# into the ath12k_wifi7 fork firmware dir as tmpfs files, so the fork's
# cal-pci-<addr>.bin firmware requests succeed before mission mode.
# RAM-only: initramfs root is tmpfs; no unit identity lands in the image.
#
# b7 lesson: the preinit hook ran with no console trace and produced no
# files; every step now prints to /dev/console and the ART device falls
# back through find_mmc_part AND /dev/mmcblk0p20 explicitly.

sbe1v1k_caldata_note() {
	echo "sbe1v1k-caldata: $*" > /dev/console 2>/dev/null
	logger -t sbe1v1k-caldata "$*" 2>/dev/null
}

sbe1v1k_write_cal_pci() {
	local dir=/lib/firmware/ath12k/QCN92XX/hw1.0
	local art spec addr off
	[ -d /lib/firmware ] || { sbe1v1k_caldata_note "no /lib/firmware yet"; return 1; }
	mkdir -p "$dir"

	command -v find_mmc_part >/dev/null 2>&1 && \
		art=$(find_mmc_part 0:ART mmcblk0 2>/dev/null)
	case "$art" in
		/dev/*) : ;;
		mmcblk*) art="/dev/$art" ;;
		*) art="/dev/mmcblk0p20" ;;
	esac
	if [ ! -b "$art" ]; then
		[ -b /dev/mmcblk0p20 ] && art=/dev/mmcblk0p20
	fi
	sbe1v1k_caldata_note "ART device: $art"
	[ -b "$art" ] || { sbe1v1k_caldata_note "no ART block device"; return 1; }

	for spec in 0001:01:00.0:362496 0002:01:00.0:567296 0003:01:00.0:772096; do
		addr=${spec%:*}
		off=${spec##*:}
		if [ -s "$dir/cal-pci-$addr.bin" ]; then
			sbe1v1k_caldata_note "cal-pci-$addr.bin already present"
			continue
		fi
		if dd if="$art" of="$dir/cal-pci-$addr.bin" \
			bs=64 skip=$((off / 64)) count=2880 2>/dev/null && \
			[ -s "$dir/cal-pci-$addr.bin" ]; then
			sbe1v1k_caldata_note "cal-pci-$addr.bin written ($(wc -c < "$dir/cal-pci-$addr.bin") bytes)"
		else
			sbe1v1k_caldata_note "cal-pci-$addr.bin WRITE FAILED"
		fi
	done
	return 0
}
