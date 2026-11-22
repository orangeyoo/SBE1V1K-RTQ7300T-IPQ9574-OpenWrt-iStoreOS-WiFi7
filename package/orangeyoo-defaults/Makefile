include $(TOPDIR)/rules.mk
PKG_NAME:=orangeyoo-defaults
PKG_VERSION:=1.0
PKG_RELEASE:=15
include $(INCLUDE_DIR)/package.mk
define Package/orangeyoo-defaults
  SECTION:=base
  CATEGORY:=Base system
  TITLE:=Orangeyoo 2.0 factory defaults (LAN 30.1 + per-unit wireless layout)
  PKGARCH:=all
endef
define Build/Prepare
endef
define Build/Compile
endef
define Package/orangeyoo-defaults/install
	$(INSTALL_DIR) $(1)/www/luci-static
	$(CP) ./files/www/luci-static/. $(1)/www/luci-static/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/etc/init.d/factory-wireless $(1)/etc/init.d/
	$(INSTALL_BIN) ./files/usr/sbin/factory-wireless-layout $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/etc/init.d/sbe-wifi-guard $(1)/etc/init.d/
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./files/etc/uci-defaults/. $(1)/etc/uci-defaults/
	$(INSTALL_DIR) $(1)/usr/share/ucode/luci
	$(CP) ./files/usr/share/ucode/luci/. $(1)/usr/share/ucode/luci/
	$(INSTALL_DIR) $(1)/etc/config
	$(CP) ./files/etc/config/. $(1)/etc/config/
	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/lib/upgrade/keep.d/orangeyoo-defaults $(1)/lib/upgrade/keep.d/
	$(INSTALL_DIR) $(1)/etc/sysctl.d
	$(INSTALL_DATA) ./files/etc/sysctl.d/13-sbe-tcp-cubic.conf $(1)/etc/sysctl.d/
endef
$(eval $(call BuildPackage,orangeyoo-defaults))
