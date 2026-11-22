#
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
#
# SPDX-License-Identifier: GPL-2.0
#
# QuickStart first-boot wizard suite for the Orangeyoo SBE1V1K,
# replicated 1:1 from the Orangeyoo 1.0 stage-root (apk-installed suite
# there; the 2.0 opkg-flow replication missed all of it, which made the
# iStore landing fall back to redirect_fallback - D4, audit
# tmp/agent-manual-layer-report.md section 2.5).
#
# Contents (21 paths, 13.26 MB): the static Go quickstart daemon
# (/usr/sbin/quickstart + startdhns symlink; serves
# /var/run/quickstart/local.sock), its init.d pair (92/93) and hotplug
# handlers (block/iface), the legacy Lua LuCI glue (controllers
# quickstart.lua + istore_backend.lua, view quickstart/{home,main}.htm,
# menu.d wildcard admin/quickstart/*, zh-cn lmo), the SPA front-end
# (www/luci-static/quickstart: index.js/vendor.js/style.css + i18n
# json), the libexec helpers (auto_setup.sh, dhcpvalid.sh) and the
# vendor-OUI database (/usr/share/quickstart/manuf).
#
# Rendering note: the wizard page is a legacy Lua template (1.0 Lua
# LuCI); under the 2.0 ucode dispatcher it goes through ucodebridge
# like every other legacy target.  It is a self-contained SPA page, so
# theme chrome is not expected.  1.0 shipped no rpcd ACL for it either;
# replicated as-is.  Runtime verification is a device gate (pre13).

include $(TOPDIR)/rules.mk

PKG_NAME:=orangeyoo-quickstart
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Orangeyoo

include $(INCLUDE_DIR)/package.mk

define Package/orangeyoo-quickstart
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=QuickStart first-boot wizard (1.0 suite replicated)
  URL:=https://github.com/istoreos
  DEPENDS:=+luci-compat
endef

define Package/orangeyoo-quickstart/description
 QuickStart first-boot wizard for the Orangeyoo SBE1V1K: the static Go
 daemon, its init/hotplug wiring, the LuCI wizard page and the SPA
 front-end, replicated 1:1 from the Orangeyoo 1.0 stage-root so the
 iStore landing serves the wizard instead of redirect_fallback.
 The daemon binary is the 1.0 aarch64 build (statically linked Go);
 the legacy LuCI glue renders through luci-compat on the ucode
 dispatcher.
endef

define Package/orangeyoo-quickstart/conffiles
/etc/config/quickstart
endef

# Arch-specific (non-all) file packages need a real PKG_BUILD_DIR or the
# whole compile chain becomes a silent no-op ("Nothing to be done for
# 'compile'"): stage the replicated tree under a subdir through
# Build/Prepare (like the orangeyoo-sbe1v1k-bdf package stages ./src).
# The install copies from that staging subdir, never from
# $(PKG_BUILD_DIR)/. itself - the build system runs Package/install
# against $(PKG_BUILD_DIR)/.pkgdir/<name> too and that would recurse.
# Executable bits ship on the init/hotplug/libexec files as extracted.
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./files/. $(PKG_BUILD_DIR)/files/
endef

define Build/Compile
endef

define Package/orangeyoo-quickstart/install
	$(INSTALL_DIR) $(1)
	$(CP) $(PKG_BUILD_DIR)/files/. $(1)/
endef

$(eval $(call BuildPackage,orangeyoo-quickstart))
