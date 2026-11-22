#!/bin/sh
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
# Assert the resolved .config still carries every release-critical setting.
set -eu
C=${1:-.config}
fail=0
need() { grep -qxF "$1" "$C" || { echo "check-config: missing: $1"; fail=1; }; }
deny() { if grep -qxF "$1" "$C"; then echo "check-config: forbidden: $1"; fail=1; fi; }

need 'CONFIG_TARGET_DEVICE_ipq95xx_generic_DEVICE_orangeyoo_sbe1v1k=y'
need '# CONFIG_DEBUG is not set'
need '# CONFIG_DEVEL is not set'
need 'CONFIG_REPRODUCIBLE_DEBUG_INFO=y'
need '# CONFIG_PACKAGE_syslog-ng is not set'
need '# CONFIG_PACKAGE_kmod-qca-nss-eip is not set'
need 'CONFIG_PACKAGE_logd=y'
need 'CONFIG_PACKAGE_orangeyoo-defaults=y'
need 'CONFIG_PACKAGE_sbe-pppoe-padt=y'
need 'CONFIG_PACKAGE_kmod-ath12k=y'
need 'CONFIG_PACKAGE_wpad-mesh-openssl=y'
need 'CONFIG_PACKAGE_kmod-qca-nss-dp=y'
need 'CONFIG_PACKAGE_kmod-qca-nss-ecm-standard=y'
need 'CONFIG_PACKAGE_kmod-qca-nss-ppe-ds=y'
need 'CONFIG_PACKAGE_kmod-leds-pwm=y'	# status LED is on the PWM block
# QCA feed overrides must be active (see release/feeds.sh)
grep -q '^CONFIG_OVERRIDE_PKGS="hostapd mac80211 qca-nss-dp qca-ssdk wifi-scripts"$' "$C" || { echo "check-config: feed overrides differ: $(grep ^CONFIG_OVERRIDE_PKGS "$C")"; fail=1; }
for d in prpl_freedom qcom_alxx qcom_db-al01-c2 qcom_rdp433; do
	deny "CONFIG_TARGET_DEVICE_ipq95xx_generic_DEVICE_$d=y"
done
for p in bluez-daemon sbe1v1k-cpc sbe1v1k-bt-gateway; do
	deny "CONFIG_PACKAGE_$p=y"
done
if grep -q '^CONFIG_PACKAGE_uboot-.*ipq9574.*=y' "$C"; then echo 'check-config: reference-board U-Boot packages selected (stale target metadata?)'; fail=1; fi
grep -q '^CONFIG_VERSION_NUMBER="[0-9]' "$C" || { echo "check-config: no version"; fail=1; }
[ $fail = 0 ] && echo "check-config: OK"
exit $fail
