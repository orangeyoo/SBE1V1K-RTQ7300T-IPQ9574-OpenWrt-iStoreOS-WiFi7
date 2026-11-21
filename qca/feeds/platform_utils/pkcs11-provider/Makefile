include $(TOPDIR)/rules.mk

PKG_NAME:=pkcs11-provider
PKG_RELEASE:=1

PKG_SOURCE_URL:=https://github.com/latchset/pkcs11-provider.git
PKG_SOURCE_PROTO:=git
PKG_SOURCE_DATE:=2025-02-11
PKG_SOURCE_VERSION:=8f6b94409d4872265076df310492da1e5f6abdf7
PKG_MIRROR_HASH:=02bf7ce96814a07174e3ce3b934133dcc17a5293a71692d45b637d4d95418000
PKG_SOURCE_SUBMODULES:=skip

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/meson.mk

define Package/$(PKG_NAME)
  SECTION:=libs
  CATEGORY:=Libraries
  DEPENDS:= +libopenssl
  TITLE:=QCA-PKCS11 library
endef

define Package/pkcs11-provider/description
  This is an OpenSSL 3.x provider to access Hardware and Software Tokens using the PKCS#11 Cryptographic Token Interface.The PKCS#11 provider is a connector that allows OpenSSL to make proper use of such drivers. This code targets PKCS#11 version 3.1 but is backwards compatible to version 3.0 and 2.40 as well.
endef

define Package/$(PKG_NAME)/install
	$(INSTALL_DIR) $(1)/usr/lib/engines-3/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/openwrt-build/src/pkcs11.so $(1)/usr/lib/engines-3/
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
