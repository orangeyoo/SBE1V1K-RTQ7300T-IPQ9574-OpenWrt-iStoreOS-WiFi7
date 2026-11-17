#
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
#
# SPDX-License-Identifier: GPL-2.0-or-later
#

include $(TOPDIR)/rules.mk

PKG_NAME:=sbe-pppoe-padt
PKG_VERSION:=1.0
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/sbe-pppoe-padt
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Drop a stale PPPoE session at boot (SBE1V1K)
  DEPENDS:=+ppp-mod-pppoe
endef

define Package/sbe-pppoe-padt/description
  Records the PPPoE session on every connect and, after an unclean
  shutdown (power loss, panic, hang), sends PADT for it at boot so the
  ISP access concentrator drops it before the router dials again.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall \
		-o $(PKG_BUILD_DIR)/pppoe-padt $(PKG_BUILD_DIR)/pppoe-padt.c
endef

define Package/sbe-pppoe-padt/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/ppp/ip-up.d $(1)/etc/ppp/ip-down.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/pppoe-padt $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/sbe-pppoe-padt.init $(1)/etc/init.d/sbe-pppoe-padt
	$(INSTALL_BIN) ./files/ip-up $(1)/etc/ppp/ip-up.d/50-sbe-pppoe-session
	$(INSTALL_BIN) ./files/ip-down $(1)/etc/ppp/ip-down.d/50-sbe-pppoe-session
	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/keep $(1)/lib/upgrade/keep.d/sbe-pppoe-padt
endef

$(eval $(call BuildPackage,sbe-pppoe-padt))
