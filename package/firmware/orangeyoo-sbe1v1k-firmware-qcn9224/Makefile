#
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
#
# SPDX-License-Identifier: GPL-2.0
#
# ath12k MHI/regdb firmware for the Orangeyoo SBE1V1K (Askey RTQ7300T,
# AP-AL02-C4 family, 3x QCN9224-class PCIe radios on pcie1/2/3).
#
# Why this package exists (2026-11-05 firmware-chain evidence pass):
#  - The wlan-open feed ath12k probes the radios at PCI ID 0x1109
#    (QCN9274_DEVICE_ID) and, per the SoC hw version it reads at probe,
#    requests its firmware from either
#      QCN92XX/hw1.0  (0x1109 + soc hw major=2, the expected branch; the
#                      feed patch renames upstream "QCN9274/hw2.0") or
#      QCN9274/hw1.0  (0x1109 + soc hw major=1, kept as a defensive copy).
#  - Before any board data can be matched, ath12k fatally requires from
#    that directory: amss.bin (MHI start, API V1 fallback when no
#    firmware-2.bin container), regdb.bin (QMI BDF_TYPE_REGDB) and
#    m3.bin (qcn9274-hw20 m3_loader=driver).  Neither ath12k-firmware-
#    wcn7850 (only populates WCN7850/hw2.0, a branch this silicon never
#    takes) nor the linux-firmware QCN9274/hw2.0 payload (PCIe-card
#    BE14-class firmware, no regdb.bin) provides these for this board.
#  - Source: the factory 0:WIFIFW partition bank unpacked at
#    p24-root/qcn9224 (2024-11 bank; ap-al02-c4_qcn9224_pci1/2/3 per its
#    firmware_rdp_feature.ini).  Input payload provenance (sha256):
#    amss.bin  a7089d77e2ad540311d8b07a90b1767d44b50a0c8a55896fc1298e114c11a448
#    m3.bin    cc303cd72be68e89f96ea83e96b7887afa6cb641800b56dd8fc604bdcd18f7b8
#    regdb.bin 09bf842d4817b92fba6db2b22c8130b110ec745f6c3fe35c2ded483dc7ecf019
#
# 2026-11-14: amss.bin/m3.bin replaced by WLAN.WBE.1.4-01734 (2024-10-04),
#  taken from the ASUS TUF-BE9400 GPL tree (release/src-qca-ipq53xx/
#  ipq53xx/qca-wifi-fw/source/QCN9224/BIN_FILES/qcn9224, single-MAC
#  amss.bin, not amss_dualmac.bin):
#    amss.bin  193f38c78cb4b9975facf3fb274d5d3fb8d9df5bd9db88d09bbf3f57d65d8339
#    m3.bin    5f01e272dd7da7f4b3f86e08373bd6fba38de475fd1ce6b4c934dab6a616a5a6
#  The factory WBE.1.1.r7 asserts again when mac80211 restores an 802.11s
#  mesh point after a firmware restart (vdev start, nested reset, reboot)
#  and offers only Mode0 group recovery. WBE.1.4 restores the mesh in
#  place (node, injected 6G assert: APs and mesh back in ~32 s) and
#  advertises Mode2 recovery. regdb.bin, rxgainlut.bin, board data and
#  caldata stay the factory ones.
#
# File ownership vs orangeyoo-sbe1v1k-bdf (which DEPENDS on this package):
#  this package installs amss/m3/regdb only; the bdf package installs
#  board-2.bin and the per-radio cal-pci-*.bin.  No file overlaps, so
#  ipk install order cannot clobber either side.

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/package.mk

PKG_NAME:=orangeyoo-sbe1v1k-firmware-qcn9224
PKG_VERSION:=2026-11-14
PKG_RELEASE:=1

define Package/orangeyoo-sbe1v1k-firmware-qcn9224
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=ath12k MHI/regdb firmware for Orangeyoo SBE1V1K QCN9224 radios
  DEPENDS:=@TARGET_ipq95xx
endef

define Package/orangeyoo-sbe1v1k-firmware-qcn9224/description
  Factory WIFIFW-bank amss.bin/m3.bin/regdb.bin for the 3x QCN9224-class
  PCIe radios, installed into both ath12k hw_rev candidate directories
  (QCN92XX/hw1.0 = expected 0x1109+soc-major-2 branch, QCN9274/hw1.0
  defensive copy for soc-major-1).
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	true
endef

define Package/orangeyoo-sbe1v1k-firmware-qcn9224/install
	$(INSTALL_DIR) \
		$(1)/lib/firmware/ath12k/QCN9274/hw1.0 \
		$(1)/lib/firmware/ath12k/QCN92XX/hw1.0
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/amss.bin \
		$(1)/lib/firmware/ath12k/QCN92XX/hw1.0/amss.bin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/m3.bin \
		$(1)/lib/firmware/ath12k/QCN92XX/hw1.0/m3.bin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/regdb.bin \
		$(1)/lib/firmware/ath12k/QCN92XX/hw1.0/regdb.bin
		$(INSTALL_DATA) $(PKG_BUILD_DIR)/rxgainlut.bin \
		$(1)/lib/firmware/ath12k/QCN92XX/hw1.0/rxgainlut.bin
		$(INSTALL_DATA) $(PKG_BUILD_DIR)/qdss_trace_config.bin \
		$(1)/lib/firmware/ath12k/QCN92XX/hw1.0/qdss_trace_config.bin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/amss.bin \
		$(1)/lib/firmware/ath12k/QCN9274/hw1.0/amss.bin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/m3.bin \
		$(1)/lib/firmware/ath12k/QCN9274/hw1.0/m3.bin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/regdb.bin \
		$(1)/lib/firmware/ath12k/QCN9274/hw1.0/regdb.bin
		$(INSTALL_DATA) $(PKG_BUILD_DIR)/rxgainlut.bin \
		$(1)/lib/firmware/ath12k/QCN9274/hw1.0/rxgainlut.bin
		$(INSTALL_DATA) $(PKG_BUILD_DIR)/qdss_trace_config.bin \
		$(1)/lib/firmware/ath12k/QCN9274/hw1.0/qdss_trace_config.bin
endef

$(eval $(call BuildPackage,orangeyoo-sbe1v1k-firmware-qcn9224))
