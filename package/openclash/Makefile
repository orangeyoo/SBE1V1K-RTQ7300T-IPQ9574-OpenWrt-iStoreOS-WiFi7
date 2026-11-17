# Repackaged 1:1 from Orangeyoo 1.0 stage-root (luci-app-openclash 0.47.156,
# arch all) so 2.0 ships the exact same backend the 1.0 line validated.
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-openclash
PKG_VERSION:=0.47.156
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-openclash
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=OpenClash (1.0 parity repack)
  PKGARCH:=all
  DEPENDS:=+libc +dnsmasq-full +bash +curl +ca-bundle +ip-full +ruby +ruby-yaml +kmod-tun +unzip
endef

define Build/Prepare
endef

define Build/Compile
endef

define Package/luci-app-openclash/conffiles
/etc/openclash/
endef

define Package/luci-app-openclash/install
	$(CP) ./files/. $(1)/
endef

$(eval $(call BuildPackage,luci-app-openclash))
