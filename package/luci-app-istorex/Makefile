# Repackaged 1:1 from Orangeyoo 1.0 stage-root (luci-app-istorex 0.6.6-r2).
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-istorex
PKG_VERSION:=0.6.6-r2
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-istorex
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=iStoreX (1.0 parity repack)
  PKGARCH:=all
  DEPENDS:=+luci-app-store
endef

define Build/Prepare
endef

define Build/Compile
endef

define Package/luci-app-istorex/install
	$(CP) ./files/. $(1)/
endef

$(eval $(call BuildPackage,luci-app-istorex))
