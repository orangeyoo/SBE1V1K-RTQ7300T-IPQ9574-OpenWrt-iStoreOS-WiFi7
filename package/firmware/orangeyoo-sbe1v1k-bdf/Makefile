#
# Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
#
# SPDX-License-Identifier: GPL-2.0
#
# ath12k board data (BDF) for the Orangeyoo SBE1V1K (Askey RTQ7300T,
# AP-AL02-C4 family, 3x QCN9224-class PCIe radios on pcie1/2/3).
#
# Evidence (2026-11-05 board/radio evidence pass, see the board dts header):
#  - Factory 0:ART partition (p20, 1 MiB,
#    sha256 f87414f110101942a9c73b6df3efe089bc3de613ddfd4f5d21808c9c87789188)
#    carries one calibration block per radio: 0x58800 (board_id 1), 0x8a800
#    (board_id 4, WSI controller), 0xbc800 (board_id 2).  Each block is a
#    calheader (tag 0x04040001) plus the vendor BDF blob; the blocks are
#    shipped verbatim in ./src and used as the board-2.bin DATA payloads.
#  - The community board-askey_sbe1v1k.qcn9274 payloads match these blocks
#    at 99.9% (MAC placeholder + check word + a few tune fields).
#  - The ath12k that ships with the QSDK14 ipq95xx open profile
#    (qca/feeds/wlan-open, backports-6.1-9a0dddfb3) matches board-2.bin
#    BOARD names "bus=pci,qmi-chip-id=<c>,qmi-board-id=<b>"; its feed patch
#    0001-d takes <b> from the DT "qcom,board_id" of the wifi@0 node
#    (1/4/2, see ipq9574-sbe1v1k.dts).  <c> = 0 is the community-measured
#    QMI chip id for this radio family - TODO on-device verification via
#    the ath12k "boot using board name" debug line; regenerate with
#    --chip-id <c> if it differs.
#
# Zero-unit-identity (2.0-pre1 delivery gate): the generated board-2.bin is
# built with --neutralize-mac, so the reference unit's radio MACs never enter
# the firmware; each ART block's MAC field carries a community-style
# placeholder instead.  Per-unit radio calibration is served at runtime by
# ./src/ath12k-sbe1v1k-caldata (installed to /etc/hotplug.d/firmware/), which
# loads the cal-pci-*.bin request straight from this unit's own 0:ART
# partition via the sysfs firmware loader (RAM only, no persistent file).
# The MHI/regdb firmware for the QCN9274-family dirs (amss.bin/m3.bin/
# regdb.bin) is provisioned by the sibling package
# orangeyoo-sbe1v1k-firmware-qcn9224 (factory 0:WIFIFW bank, p24); the
# DEPENDS below guarantees both are selected together.
#
# Install coverage: the runtime fw.dir depends on the radio's SoC hw version
# at probe time (QCN9274/hw1.0, QCN92XX/hw1.0 or WCN7850/hw2.0 in the
# backports ath12k hw table), so the generated board-2.bin is installed into
# all three.

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/package.mk

PKG_NAME:=orangeyoo-sbe1v1k-bdf
PKG_VERSION:=2026-11-05
PKG_RELEASE:=8

# Input payload provenance (sha256):
#  art-radio-1.bin 5416ea6fe2b204e60c8f7d85e71ecc68f76108af7d45b14788297223037b0504
#  art-radio-4.bin 4179d7fef9a28ef63a6d1e6a56541ed1db7d15a1667a1ce8688e15038637e2d3
#  art-radio-2.bin 09299dec287fbdd1bea2c5cc80d74bf4dc322cb3d06aa608794064b2083d6752

define Package/orangeyoo-sbe1v1k-bdf
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=ath12k BDF for Orangeyoo SBE1V1K (Askey RTQ7300T)
  DEPENDS:=@TARGET_ipq95xx +orangeyoo-sbe1v1k-firmware-qcn9224
endef

define Package/orangeyoo-sbe1v1k-bdf/description
  ath12k board-2.bin generated from the factory 0:ART calibration blocks
  (board_id 1/4/2 = pcie1/pcie2/pcie3 radios) with the per-unit radio MACs
  neutralized to community-style placeholders (zero-unit-identity gate),
  plus a firmware hotplug script that serves each unit its own calibration
  from its own 0:ART partition at runtime.  Matched per radio by the
  wlan-open ath12k via the DT qcom,board_id.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	python3 $(PKG_BUILD_DIR)/gen_board2.py \
		-o $(PKG_BUILD_DIR)/board-2.bin \
		--chip-id 0 \
		--neutralize-mac \
		--board $(PKG_BUILD_DIR)/art-radio-1.bin:1,$(PKG_BUILD_DIR)/art-radio-4.bin:4,$(PKG_BUILD_DIR)/art-radio-2.bin:2 \
		--regdb $(PKG_BUILD_DIR)/board-askey_sbe1v1k.qcn9274
endef

define Package/orangeyoo-sbe1v1k-bdf/install
	$(INSTALL_DIR) \
		$(1)/lib/firmware/ath12k/QCN9274/hw1.0 \
		$(1)/lib/firmware/ath12k/QCN92XX/hw1.0 \
		$(1)/lib/firmware/ath12k/WCN7850/hw2.0 \
		$(1)/etc/hotplug.d/firmware
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/board-2.bin \
		$(1)/lib/firmware/ath12k/QCN9274/hw1.0/board-2.bin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/board-2.bin \
		$(1)/lib/firmware/ath12k/QCN92XX/hw1.0/board-2.bin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/board-2.bin \
		$(1)/lib/firmware/ath12k/WCN7850/hw2.0/board-2.bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ath12k-sbe1v1k-caldata \
		$(1)/etc/hotplug.d/firmware/12-ath12k-sbe1v1k-caldata
	$(INSTALL_DIR) $(1)/lib/preinit
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/81-sbe1v1k-wifi-caldata \
		$(1)/lib/preinit/81-sbe1v1k-wifi-caldata
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/sbe1v1k-caldata-lib.sh \
		$(1)/lib/sbe1v1k-caldata-lib.sh
	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/rc.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/sbe1v1k-wifi-caldata \
		$(1)/etc/init.d/sbe1v1k-wifi-caldata
	ln -sf ../init.d/sbe1v1k-wifi-caldata \
		$(1)/etc/rc.d/S01sbe1v1k-wifi-caldata
endef

$(eval $(call BuildPackage,orangeyoo-sbe1v1k-bdf))
