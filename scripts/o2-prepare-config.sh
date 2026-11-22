#!/bin/sh
# o2-prepare-config.sh — apply the Orangeyoo 2.0 SBE1V1K build-config
# overrides that live outside git (target .config is a build artifact):
# the product version branding consumed by /etc/openwrt_release and
# /usr/lib/os-release (signing gate) and verified by the delivery
# pipeline.  Idempotent; safe to re-run after defconfig rounds.
#
# Usage: scripts/o2-prepare-config.sh [VERSION]   (run from the tree root)
#   VERSION defaults to 2.0-pre1; pass e.g. 2.0-pre2 for a new pre series.

set -e
cd "$(dirname "$0")/.."
# Product version injected into CONFIG_VERSION_NUMBER (banner,
# /etc/openwrt_release, /usr/lib/os-release).  Default keeps the
# pre1 behavior; later pres pass their own version as $1.
O2_VERSION="${1:-2.0-pre1}"
[ -f .config ] || { echo "no .config; run make defconfig first" >&2; exit 2; }

sed -i '/^CONFIG_VERSIONOPT=/d; /^# CONFIG_VERSIONOPT is not set$/d' .config
sed -i '/^CONFIG_VERSION_DIST=/d; /^# CONFIG_VERSION_DIST is not set$/d' .config
sed -i '/^CONFIG_VERSION_NUMBER=/d; /^# CONFIG_VERSION_NUMBER is not set$/d' .config
cat >> .config <<'EOF'
CONFIG_VERSIONOPT=y
CONFIG_VERSION_DIST="iStoreOS-SBE1V1K-Orangeyoo"
EOF
# the branding heredoc above is quoted ('EOF') so $O2_VERSION cannot
# expand inside it; add the version line separately:
printf 'CONFIG_VERSION_NUMBER="%s"\n' "$O2_VERSION" >> .config
make defconfig

echo "active branding:"
grep -e '^CONFIG_VERSIONOPT' -e '^CONFIG_VERSION_DIST' -e '^CONFIG_VERSION_NUMBER' .config

#NSS EIP deselect note (see Device/orangeyoo_sbe1v1k in
# target/linux/feeds/ipq95xx/image/generic.mk): the package negations
# "-kmod-qca-nss-eip*" there document intent, but the effective removal
# for the shared rootfs needs these explicit settings in .config:
sed -i 's/^CONFIG_PACKAGE_kmod-qca-nss-eip=y$/# CONFIG_PACKAGE_kmod-qca-nss-eip is not set/' .config
sed -i 's/^CONFIG_PACKAGE_kmod-qca-nss-eip-crypto=y$/# CONFIG_PACKAGE_kmod-qca-nss-eip-crypto is not set/' .config
sed -i 's/^CONFIG_PACKAGE_kmod-qca-nss-eip-ipsec=y$/# CONFIG_PACKAGE_kmod-qca-nss-eip-ipsec is not set/' .config
# dnsmasq variant: full replaces dhcpv6 (openclash dep); keep dhcpv6 out on defconfig rounds
# luci-compat: provides luci.cbi.datatypes + luci.model.network (openclash config pages)
sed -i "s/# CONFIG_PACKAGE_luci-compat is not set$/CONFIG_PACKAGE_luci-compat=y/" .config
sed -i "s/CONFIG_PACKAGE_dnsmasq-dhcpv6=y$/# CONFIG_PACKAGE_dnsmasq-dhcpv6 is not set/" .config

# UI language: Simplified Chinese.  One tristate drives everything:
# luci.mk gives every luci-i18n-*-zh-cn package
#   DEFAULT:=LUCI_LANG_zh_Hans||(ALL&&m)
# so CONFIG_LUCI_LANG_zh_Hans=y installs the zh-cn translation of every
# selected LuCI app/module that ships po/zh_Hans.  orangeyoo-defaults
# flips the actual default via uci-defaults (luci.main.lang=zh_cn).
sed -i '/^CONFIG_LUCI_LANG_zh_Hans=/d; /^# CONFIG_LUCI_LANG_zh_Hans is not set$/d' .config
printf 'CONFIG_LUCI_LANG_zh_Hans=y\n' >> .config

make defconfig
grep -e 'qca-nss-eip' -e 'CONFIG_LUCI_LANG_zh_Hans' .config
echo "zh-cn translations pulled in:"
grep -e '^CONFIG_PACKAGE_luci-i18n-.*-zh-cn=y' .config | head -20
