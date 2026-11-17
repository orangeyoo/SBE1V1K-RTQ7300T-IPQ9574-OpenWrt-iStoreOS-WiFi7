include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=nat46

# Bumped to the adb2f72e (2025-11-04) snapshot: the in-tree qca-nss-ppe
# drv/ppe_drv/tun module calls the newer nat46 API (xlate_6_to_4) that the
# old 2022-09-19 snapshot does not export.
PKG_MIRROR_HASH:=5d5c14bbb700490a9357c2c8baffd52615f3b572f82eb2fcbfc530bd23f2cdc3
PKG_SOURCE_URL:=https://github.com/ayourtch/nat46.git
PKG_SOURCE_DATE:=2025-11-04
PKG_SOURCE_PROTO:=git
PKG_SOURCE_VERSION:=adb2f72e6fb81ee11b54513d384b68e3c3d93464

PKG_MAINTAINER:=Hans Dedecker <dedeckeh@gmail.com>
PKG_LICENSE:=GPL-2.0

# The kbuild symvers collector reads $(PKG_BUILD_DIR)/$(PKG_EXTMOD_SUBDIRS)/
# Module.symvers; without this the exported MAP-T APIs (xlate_4_to_6/
# xlate_6_to_4) never reach KBUILD_EXTRA_SYMBOLS and dependent modules
# (qca-nss-ppe, qca-nss-ecm) fail at modpost.
PKG_EXTMOD_SUBDIRS:=nat46/modules

include $(INCLUDE_DIR)/package.mk

define KernelPackage/nat46
  DEPENDS:=@IPV6 +kmod-nf-conntrack
  TITLE:=Stateless NAT46 translation kernel module
  SECTION:=kernel
  SUBMENU:=Network Support
  FILES:=$(PKG_BUILD_DIR)/nat46/modules/nat46.ko
  AUTOLOAD:=$(call AutoLoad,33,nat46)
endef

include $(INCLUDE_DIR)/kernel-defaults.mk

# qca-nss-ppe / qca-nss-ecm compile against these headers via
# -I$(STAGING_DIR)/usr/include/nat46 and "#include <nat46/nat46-core.h>",
# while qca-nss-ppe-mapt uses the flat "#include <nat46-core.h>" form, so
# both layouts are staged.
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/nat46/nat46
	$(CP) $(PKG_BUILD_DIR)/nat46/modules/*.h $(1)/usr/include/nat46/nat46/
	$(INSTALL_DIR) $(1)/usr/include/nat46
	$(CP) $(PKG_BUILD_DIR)/nat46/modules/*.h $(1)/usr/include/nat46/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)/nat46/modules" \
		MODFLAGS="-DMODULE -mlong-calls" \
		EXTRA_CFLAGS="-DNAT46_VERSION=\\\"$(PKG_SOURCE_VERSION)\\\"" \
		modules
endef

$(eval $(call KernelPackage,nat46))
