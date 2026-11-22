
SUBTARGET:=generic
BOARDNAME:=QTI IPQ95xx(64bit) based boards
CPU_TYPE:=cortex-a73

define Target/Description
	Build images for IPQ95xx 64 bit system.
endef

# The QSDK reference-board U-Boot packages are not listed: this target ships
# one board whose boot chain is the factory APPSBL + webfix chainloader, and
# those packages install no files into the image.
DEFAULT_PACKAGES += \
	sysupgrade-helper
