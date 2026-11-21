
define Device/qcom_alxx
        $(call Device/MultiDTBFitImage)
	DEVICE_VENDOR := Qualcomm Technologies, Inc.
	DEVICE_MODEL := AP-ALXX
	DEVICE_VARIANT :=
	BOARD_NAME := ap-alxx
	SOC := ipq9574
	KERNEL_INSTALL := 1
	KERNEL_SIZE := $(if $(CONFIG_USE_PRPLMESH_WHM), $(if $(CONFIG_DEBUG),10240k,7400k), $(if $(CONFIG_DEBUG),10240k,7168k))
	IMAGE_SIZE := 25344k
	IMAGE/sysupgrade.bin := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += qcom_alxx

define Device/qcom_al01-c1
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Qualcomm Technologies, Inc.
	DEVICE_MODEL := AP-AL01-C1
	DEVICE_VARIANT := C1
	BOARD_NAME := ap-al01.1-c1
	BUILD_DTS_ipq9574-al01-c1 := 1
	SOC := ipq9574
	KERNEL_INSTALL := 1
	KERNEL_SIZE := $(if $(CONFIG_DEBUG),8680k,7000k)
	IMAGE_SIZE := 25344k
	IMAGE/sysupgrade.bin := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += $(if $(CONFIG_LINUX_5_4), qcom_al01-c1)

define Device/qcom_al02-c1
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Qualcomm Technologies, Inc.
	DEVICE_MODEL := AP-AL02-C1
	DEVICE_VARIANT := C1
	BOARD_NAME := ap-al02-c1
	BUILD_DTS_ipq9574-al02-c1 := 1
	SOC := ipq9574
	KERNEL_INSTALL := 1
	KERNEL_SIZE := $(if $(CONFIG_DEBUG),8680k,7000k)
	IMAGE_SIZE := 25344k
	IMAGE/sysupgrade.bin := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += $(if $(CONFIG_LINUX_5_4), qcom_al02-c1)

define Device/qcom_al02-c2
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Qualcomm Technologies, Inc.
	DEVICE_MODEL := AP-AL02-C2
	DEVICE_VARIANT := C2
	BOARD_NAME := ap-al02-c2
	BUILD_DTS_ipq9574-al02-c2 := 1
	SOC := ipq9574
	KERNEL_INSTALL := 1
	KERNEL_SIZE := $(if $(CONFIG_DEBUG),8680k,7000k)
	IMAGE_SIZE := 25344k
	IMAGE/sysupgrade.bin := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += $(if $(CONFIG_LINUX_5_4), qcom_al02-c2)

define Device/qcom_al02-c7
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Qualcomm Technologies, Inc.
	DEVICE_MODEL := AP-AL02-C7
	DEVICE_VARIANT := C7
	BOARD_NAME := ap-al02.1-c7
	BUILD_DTS_ipq9574-al02-c7 := 1
	SOC := ipq9574
	KERNEL_INSTALL := 1
	KERNEL_SIZE := $(if $(CONFIG_DEBUG),8680k,7000k)
	IMAGE_SIZE := 25344k
	IMAGE/sysupgrade.bin := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += $(if $(CONFIG_LINUX_5_4), qcom_al02-c7)

define Device/qcom_rdp433
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Qualcomm Technologies, Inc.
	DEVICE_MODEL := RDP433
	DEVICE_VARIANT := AP-AL02-C4
	BOARD_NAME := ap-al02.1-c4
	BUILD_DTS_ipq9574-rdp433 := 1
	SOC := ipq9574
	KERNEL_INSTALL := 1
	KERNEL_SIZE := $(if $(CONFIG_DEBUG),8680k,7000k)
	IMAGE_SIZE := 25344k
	IMAGE/sysupgrade.bin := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += qcom_rdp433

define Device/prpl_freedom
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Prpl
	DEVICE_MODEL := Freedom
	DEVICE_VARIANT :=
	BOARD_NAME := prpl-freedom
	BUILD_DTS_ipq9574-freedom := 1
	SOC := ipq9574
	KERNEL_INSTALL := 1
	KERNEL_SIZE := $(if $(CONFIG_DEBUG),8680k,7000k)
	IMAGE_SIZE := 25344k
	IMAGE/sysupgrade.bin := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += prpl_freedom

define Device/qcom_db-al01-c2
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Qualcomm Technologies, Inc.
	DEVICE_MODEL := DB-AL01-C2
	DEVICE_VARIANT := DB-AL01-C2
	BOARD_NAME := db-al01-c2
	BUILD_DTS_ipq9574-db-al01-c2 := 1
	SOC := ipq9574
	KERNEL_INSTALL := 1
	KERNEL_SIZE := $(if $(CONFIG_DEBUG),8680k,7000k)
	IMAGE_SIZE := 25344k
	IMAGE/sysupgrade.bin := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += qcom_db-al01-c2


# SBE1V1K (Askey RTQ7300T) port: Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
define Device/orangeyoo_sbe1v1k
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Orangeyoo
	DEVICE_MODEL := SBE1V1K
	DEVICE_ALT0_VENDOR := Askey
	DEVICE_ALT0_MODEL := RTQ7300T
	DEVICE_ALT1_VENDOR := Spectrum
	DEVICE_ALT1_MODEL := SBE1V1K
	BOARD_NAME := sbe1v1k
	SOC := ipq9574
	# sysupgrade compares the image metadata with /tmp/sysinfo/board_name,
	# which is the DT compatible "askey,sbe1v1k". The default
	# ("orangeyoo,sbe1v1k", from the device id) made every in-OS upgrade
	# fail with "Device askey,sbe1v1k not supported by this image".
	SUPPORTED_DEVICES := askey,sbe1v1k orangeyoo,sbe1v1k
	# 1.x-verified FIT shape for the vendor APPSBL (see /work/fitwork
	# orangeyoo-r16x.its): @-delimited image names and a "conf@1" default
	# configuration.  Both are consumed by include/image-commands.mk
	# Build/fit (-l / -c) and scripts/mkits.sh, which with a single
	# DEVICE_DTS produce kernel@1 / fdt@1 / configurations.default="conf@1".
	DEVICE_DTS_DELIMITER := @
	DEVICE_DTS_CONFIG := conf@1
	# 6.6 arm64 Image text_offset is 0 (arch/arm64/kernel/image.h) and
	# CONFIG_RELOCATABLE is not set for ipq95xx 6.6, so the load address
	# must be 2MB-aligned (u-boot booti_setup enforces this and, for a
	# non-relocatable image, relocates the kernel to DRAM base + text_offset
	# regardless).  0x42000000 is 2MB-aligned, sits in the same 0x42xxxxxx
	# region the board's boot flow already uses (1.x FIT loaded a 5.4 kernel
	# at 0x42080000 = 0x42000000 + 5.4 text_offset 0x80000) and equals the
	# QSDK ipq95xx Device/Default value for 64bit + CONFIG_ENABLE_UBI_CORE.
	KERNEL_LOADADDR := 0x42000000
	KERNEL_INSTALL := 1
	KERNEL_SIZE := $(if $(CONFIG_DEBUG),34816k,32768k)
	IMAGE_SIZE := 1g
	DEVICE_PACKAGES := kmod-ath12k orangeyoo-sbe1v1k-firmware-qcn9224 \
		luci-app-store luci-app-openclash luci-app-istorex orangeyoo-quickstart \
		-dnsmasq-dhcpv6 dnsmasq-full orangeyoo-defaults sbe-pppoe-padt \
		orangeyoo-sbe1v1k-bdf kmod-hwmon-pwmfan kmod-qca-nss-ecm-standard \
		-kmod-qca-nss-eip -kmod-qca-nss-eip-crypto -kmod-qca-nss-eip-ipsec \
		-mwan3 luci-app-adblock adblock luci-app-ksmbd ksmbd-server \
		luci-app-openvpn openvpn-openssl luci-app-package-manager vsftpd \
		luci-app-filetransfer luci-lib-fs luci-app-passwall chinadns-ng \
		dns2socks ipt2socks microsocks tcping xray-core sing-box \
		luci-app-dockerman cgroupfs-mount luci-app-turboacc \
		yq orangeyoo-openclash-core -syslog-ng kmod-leds-pwm \
		block-mount kmod-fs-exfat kmod-fs-xfs \
		kmod-loop fdisk lsblk blkid parted kmod-dummy kmod-netlink-diag \
		wget-ssl kmod-wireguard wireguard-tools luci-proto-wireguard \
		kmod-usb-net-rndis kmod-usb-net-ipheth usbmuxd
	# The inherited QSDK_Open profile selects the NSS EIP (EIP197 inline
	# IPsec engine) chain, but its firmware package qca-nss-fw-eip-al has
	# no provider in this tree, so the opkg rootfs assembly fails on the
	# unresolved dependency. The engine is unusable without that blob and
	# is unrelated to NAT/WiFi acceleration (nss-drv/ppe/ecm), hence the
	# explicit per-device deselect.
	IMAGE/sysupgrade.bin := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += orangeyoo_sbe1v1k
