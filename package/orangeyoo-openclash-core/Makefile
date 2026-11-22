#
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
#
# SPDX-License-Identifier: GPL-2.0-or-later
#

include $(TOPDIR)/rules.mk

PKG_NAME:=orangeyoo-openclash-core
PKG_VERSION:=20260810
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/orangeyoo-openclash-core
  SECTION:=orangeyoo
  CATEGORY:=Orangeyoo firmware
  TITLE:=OpenClash mihomo (meta) core binary
  DEPENDS:=+luci-app-openclash @(aarch64)
endef

define Package/orangeyoo-openclash-core/description
  Prebundled mihomo (Clash Meta) core for OpenClash, arm64 static build
  (openclash-mihomo-arm64-20260810). Installed to /etc/openclash/core/clash_meta.
endef

define Build/Prepare
	$(CP) ./files/etc/openclash/core/clash_meta $(PKG_BUILD_DIR)/
	chmod +x $(PKG_BUILD_DIR)/clash_meta
endef

define Build/Compile
	true
endef

define Package/orangeyoo-openclash-core/install
	$(INSTALL_DIR) $(1)/etc/openclash/core
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/clash_meta $(1)/etc/openclash/core/clash_meta
endef

$(eval $(call BuildPackage,orangeyoo-openclash-core))
