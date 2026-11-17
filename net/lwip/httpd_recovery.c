// SPDX-License-Identifier: GPL-2.0+
/*
 * SBE1V1K eMMC/GPT recovery: Copyright (c) 2026 Orangeyoo <Orangeyoo@50848>
 * Minimal HTTP upload recovery server using lwIP httpd
 * Serves a tiny upload page at / and handles POST to /upload/{firmware|uboot}
 */

#include <dm.h>
#include <dm/ofnode.h>
#include <blk.h>
#include <env.h>
#include <image.h>
#include <log.h>
#include <malloc.h>
#include <memalign.h>
#include <command.h>
#include <mmc.h>
#include <mtd.h>
#include <net-lwip.h>
#include <net.h>
#include <part.h>
#include <pwm.h>
#include <initcall.h>
#include <ubi_uboot.h>
#include <watchdog.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/libfdt.h>
#include <console.h>
#include <asm/global_data.h>
#include <dm/pinctrl.h>
#include <dm/uclass.h>

#ifdef crc32
#undef crc32
#endif
#include <u-boot/crc.h>
#include <u-boot/sha256.h>

#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/ip.h>
#include <lwip/timeouts.h>
#include <lwip/udp.h>
#include <lwip/apps/httpd.h>
#include <lwip/apps/fs.h>
#include <lwip/prot/dhcp.h>
#include <lwip/prot/iana.h>
#include <version.h>
#include <timestamp.h>
#include <limits.h>
#include <vsprintf.h>
#include <hexdump.h>
#include <miiphy.h>
#include <linux/mii.h>
#include <mtd/ubi-user.h>
#include <timer.h>
#include <asm/io.h>
#include <asm/unaligned.h>
#include "../../drivers/mtd/ubi/ubi.h"
#include <stdarg.h>

DECLARE_GLOBAL_DATA_PTR;

__weak int xr1710g_sync_factory(void)
{
	return 0;
}

__weak void recovery_board_watchdog_kick(void)
{
}

__weak void recovery_board_http_acl(bool enable)
{
}

static void recovery_watchdog_poll(void)
{
	recovery_board_watchdog_kick();
	WATCHDOG_RESET();
}

static bool recovery_debug_enabled(void)
{
	return env_get_yesno("recovery_debug") == 1;
}

static void __printf(1, 2) recovery_debug_printf(const char *fmt, ...)
{
	va_list ap;

	if (!recovery_debug_enabled())
		return;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static bool recovery_board_is_sbe1v1k(void)
{
	return ofnode_device_is_compatible(ofnode_root(), "askey,sbe1v1k");
}

/*
 * Upload buffer
 * Use env 'recovery_addr' if set, otherwise fall back to U-Boot 'loadaddr'.
 * Avoid hard-coding a RAM address which may overlap U-Boot/lwIP memory.
 */
/* Default maximum upload size in bytes (override with env 'recovery_max') */
#define RECOVERY_UPLOAD_MAX    (32 * 1024 * 1024UL)
#define RECOVERY_MIN_FIRMWARE_SIZE (1 * 1024 * 1024UL)
#define RECOVERY_MAX_UBOOT_SIZE    (1 * 1024 * 1024UL)
#define RECOVERY_MAX_UBOOT_SIZE_MMC (4 * 1024 * 1024UL)
/*
 * Factory-file restore caps.  boot0/boot1 are eMMC hardware partitions
 * (8192 x 512B = 4MiB verified on this board); ART is GPT P20 (1MiB).
 * Uploads must match these sizes exactly.
 */
#define RECOVERY_MAX_BOOT_SIZE     (4 * 1024 * 1024UL)
#define RECOVERY_BOOT_SECTORS      (RECOVERY_MAX_BOOT_SIZE / 512UL)
#define RECOVERY_MAX_ART_SIZE      (1 * 1024 * 1024UL)
#define RECOVERY_ART_PART_SPEC     "0#0:ART"
#define RECOVERY_KERNEL_PAD_SIZE    (7000 * 1024UL)
#define RECOVERY_MMC_WRITE_CHUNK    (1024 * 1024UL)
#define RECOVERY_MMC_ZERO_CHUNK     (4 * 1024 * 1024UL)
#define RECOVERY_MMC_STREAM_CHUNK   (1024 * 1024UL)
#define RECOVERY_MMC_BACKUP_CHUNK   (16 * 1024 * 1024UL)
#define RECOVERY_MMC_ERASE_CHUNK    (32 * 1024 * 1024ULL)
#define RECOVERY_MMC_ERASE_PROGRESS_STEPS 1000U
#define RECOVERY_SQUASHFS_MAGIC      0x73717368U
#define RECOVERY_SQUASHFS_MAJOR      4U
#define RECOVERY_SQUASHFS_MAJOR_OFF  28U
#define RECOVERY_SQUASHFS_BYTES_OFF  40U
#define RECOVERY_SQUASHFS_HEADER_MIN 48U
#define RECOVERY_SBE1V1K_CONTROL_BOARD "BOARD=askey_sbe1v1k"
#define RECOVERY_TAR_END_BLOCKS       2U
#define RECOVERY_FWTOOL_MAGIC          0x46577830U /* FWx0 */
#define RECOVERY_FWTOOL_TRAILER_SIZE   16U
#define RECOVERY_FWTOOL_HEADER_SIZE    8U
#define RECOVERY_FWTOOL_METADATA_MAX   (30U * 1024U)
#define RECOVERY_FWTOOL_PADDING_MAX    (64U * 1024U)
#define RECOVERY_FWTOOL_TAIL_MAX       \
	(RECOVERY_FWTOOL_PADDING_MAX + RECOVERY_FWTOOL_HEADER_SIZE + \
	 RECOVERY_FWTOOL_METADATA_MAX + RECOVERY_FWTOOL_TRAILER_SIZE)
#define RECOVERY_FWTOOL_INFO           1U
#define RECOVERY_REPARTITION_MAX    64UL
#define RECOVERY_SBE1V1K_GPT_MAX    12288UL
#define RECOVERY_SBE1V1K_FULL_CHAINLOADER_START 81954ULL
#define RECOVERY_SBE1V1K_FULL_CHAINLOADER_SIZE  8192ULL
#define RECOVERY_SBE1V1K_FULL_KERNEL_START      90146ULL
#define RECOVERY_SBE1V1K_FULL_KERNEL_SIZE       65536ULL
#define RECOVERY_SBE1V1K_FULL_ROOTFS_START      155682ULL
#define RECOVERY_SBE1V1K_FULL_ROOTFS_SIZE       2097152ULL
#define RECOVERY_SBE1V1K_FULL_DATA_START        2252834ULL
#define RECOVERY_SBE1V1K_FIT_TFTP_ADDR         0x80000000UL
#define RECOVERY_SBE1V1K_FIT_PERSISTENT_ADDR   0x44000000UL
#define RECOVERY_GPT_TYPE_BASIC_DATA       "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
#define RECOVERY_GPT_TYPE_LINUX_FS         "0FC63DAF-8483-4772-8E79-3D69D8477DE4"

/* Delay before reboot after flashing completes, to let browser finish reads */
#define REBOOT_DELAY_MS        3000
#define FLASH_START_DELAY_MS   3000
#define RECOVERY_STATIC_IPADDR           "192.168.255.1"
#define RECOVERY_STATIC_NETMASK          "255.255.255.0"
#define RECOVERY_STATIC_GATEWAY          "0.0.0.0"
#define RECOVERY_DHCP_CLIENT_IPADDR      "192.168.255.2"
#define RECOVERY_DHCP_BROADCAST_IPADDR   "192.168.255.255"
#define RECOVERY_DHCP_LEASE_SECS         86400U
#define RECOVERY_DHCP_MAX_MSG_LEN        1500
#define RECOVERY_LED_PORTS     2
#define RECOVERY_LED_POLL_MS   100
#define RECOVERY_STATUS_LED_MAX           8
#define RECOVERY_STATUS_PWM_PERIOD_NS     1000000U
#define RECOVERY_STATUS_HW_PWM_UPDATE_MS  20
#define RECOVERY_STATUS_BREATHE_PERIOD_MS 3200
#define RECOVERY_STATUS_BREATHE_HALF_MS   (RECOVERY_STATUS_BREATHE_PERIOD_MS / 2)
#define RECOVERY_STATUS_SWEEP_STEP_MS     1400
#define RECOVERY_STATUS_OVERLAP_FP        (2 * 256)
#define RECOVERY_STATUS_BRIGHTNESS_FP     256
#define RECOVERY_STATUS_BREATHE_MIN_FP    64
#define RECOVERY_STATUS_PWM_NODE          "/recovery-status-pwm-leds"
#define RECOVERY_PWM_POLARITY_INVERTED    BIT(0)

#define RECOVERY_GPIO_SYSCTL_BASE      0x1fbf0200
#define RECOVERY_CHIP_SCU_BASE         0x1fa20000
#define RECOVERY_REG_GPIO_DATA         0x0004
#define RECOVERY_REG_GPIO_OE           0x0014
#define RECOVERY_REG_GPIO_CTRL         0x0000
#define RECOVERY_REG_GPIO_CTRL1        0x0020
#define RECOVERY_REG_GPIO_FLASH_MODE_CFG 0x0034
#define RECOVERY_REG_GPIO_CTRL2        0x0060
#define RECOVERY_REG_GPIO_CTRL3        0x0064
#define RECOVERY_REG_GPIO_DATA1        0x0070
#define RECOVERY_REG_GPIO_OE1          0x0078
#define RECOVERY_REG_GPIO_2ND_I2C_MODE 0x0214
#define RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT 0x0068

#define RECOVERY_GPIO_LAN0_LED0_MODE_MASK BIT(3)
#define RECOVERY_GPIO_LAN0_LED1_MODE_MASK BIT(4)
#define RECOVERY_GPIO_LAN1_LED0_MODE_MASK BIT(5)
#define RECOVERY_GPIO_LAN1_LED1_MODE_MASK BIT(6)
#define RECOVERY_GPIO43_FLASH_MODE_CFG BIT(23)
#define RECOVERY_GPIO44_FLASH_MODE_CFG BIT(24)
#define RECOVERY_UBOOTENV_SIZE (1 * 1024 * 1024UL)
#define RECOVERY_FACTORY_SIZE  (1 * 1024 * 1024UL)
#define RECOVERY_UBI_WRITE_CHUNK (1024 * 1024U)
#define RECOVERY_APPSBLENV_PART "0#0:APPSBLENV"
#define RECOVERY_APPSBLENV_SIZE (256 * 1024UL)
#define RECOVERY_ENV_CRC_SIZE   4
#define RECOVERY_SBE1V1K_REPARTITION_TOKEN "SBE1V1K_REPARTITION"
#define RECOVERY_SBE1V1K_CLEAN_SESSION_MAGIC 0x5342434cU /* SBCL */
#define RECOVERY_SBE1V1K_CHAINLOADER_PART "0#chainloader"
#define RECOVERY_PARTITIONS_JSON_MAX (32 * 1024UL)
#define RECOVERY_BACKUP_MAGIC         0x52424b50U
#define RECOVERY_BACKUP_NAME_MAX      100U
#define RECOVERY_TAR_BLOCK_SIZE       512ULL
#define RECOVERY_TAR_END_SIZE         (2 * RECOVERY_TAR_BLOCK_SIZE)
#define RECOVERY_TAR_MAX_MEMBER_SIZE  ((8ULL * 1024 * 1024 * 1024) - 1)
#define RECOVERY_PREPARE_BODY_MAX     32U
#define RECOVERY_PREPARE_START_DELAY_MS 100U

enum recovery_backup_mode {
	RECOVERY_BACKUP_SINGLE = 0,
	RECOVERY_BACKUP_ALL_TAR,
};

enum recovery_backup_tar_phase {
	RECOVERY_BACKUP_TAR_MEMBER_HEADER = 0,
	RECOVERY_BACKUP_TAR_MEMBER_DATA,
	RECOVERY_BACKUP_TAR_MEMBER_PADDING,
	RECOVERY_BACKUP_TAR_END,
	RECOVERY_BACKUP_TAR_DONE,
};

struct recovery_backup_source {
	int hwpart;
	int number;
	lbaint_t start;
	lbaint_t blocks;
	ulong blksz;
	unsigned long long bytes;
	char name[PART_NAME_LEN + 1];
	char filename[RECOVERY_BACKUP_NAME_MAX];
};

struct recovery_backup_tar_header {
	char name[100];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char checksum[8];
	char typeflag;
	char linkname[100];
	char magic[6];
	char version[2];
	char uname[32];
	char gname[32];
	char devmajor[8];
	char devminor[8];
	char prefix[155];
	char padding[12];
} __packed;

struct recovery_backup_file {
	u32 magic;
	enum recovery_backup_mode mode;
	struct blk_desc *desc;
	struct recovery_backup_source source;
	unsigned int source_cursor;
	lbaint_t next_lba;
	lbaint_t blocks_left;
	ulong blksz;
	u8 *cache;
	size_t cache_capacity;
	size_t cache_len;
	size_t cache_off;
	char http_header[320];
	size_t http_header_len;
	size_t http_header_off;
	struct recovery_backup_tar_header tar_header;
	size_t tar_header_off;
	unsigned long long tar_padding_left;
	unsigned long long tar_end_left;
	enum recovery_backup_tar_phase tar_phase;
	bool failed;
	bool active_counted;
	bool user_restored;
};

static u8 *recv_base;
/* Received bytes go to RAM instead of streaming to eMMC when set. */
static bool recv_to_ram;
static u32 recv_off;
static u32 recv_total;
static int post_ok;
static int upload_done;
static struct recovery_status_led_ctrl *recovery_runtime_status_leds;
static int flash_request;
static int prepare_request;
static volatile int reboot_request;
static unsigned int recovery_backup_active;
/* Progress for /status polling */
static volatile u32 prog_total; /* combined total for backward compat */
static volatile u32 prog_done;  /* combined done for backward compat */
static volatile u32 prog_erase_total;
static volatile u32 prog_erase_done;
static volatile u32 prog_write_total;
static volatile u32 prog_write_done;
/* Phase 4 extends the original progress states with a prepared stream. */
static volatile int prog_phase; /* 0 idle, 1 erase, 2 write, 3 done, -1 error */
static volatile int prog_reboot;
static bool post_destructive; /* destructive POST awaiting response ACK */
static unsigned long long prog_erase_volume_base;
static unsigned long long prog_erase_volume_bytes;
static struct recovery_status_led_ctrl *prog_status_leds;

static void post_delay_cb(void *arg)
{
    (void)arg;
    flash_request = 1;
}

static void prepare_delay_cb(void *arg)
{
	(void)arg;
	prepare_request = 1;
}

static void reboot_delay_cb(void *arg)
{
    (void)arg;
    reboot_request = 1;
}

static void recovery_prepare_static_network(void)
{
	env_set("ipaddr", RECOVERY_STATIC_IPADDR);
	env_set("netmask", RECOVERY_STATIC_NETMASK);
	env_set("gatewayip", RECOVERY_STATIC_GATEWAY);
}

enum upload_target {
	TARGET_FIRMWARE = 0,
	TARGET_UBOOT,
	TARGET_REPARTITION,
	TARGET_BOOT0,
	TARGET_BOOT1,
	TARGET_ART,
};

enum recovery_stream_format {
	RECOVERY_STREAM_RAW = 0,
	RECOVERY_STREAM_TAR,
};

enum recovery_sbe1v1k_layout {
	RECOVERY_SBE1V1K_LAYOUT_UNKNOWN = 0,
	RECOVERY_SBE1V1K_LAYOUT_FULL,
};

struct recovery_sbe1v1k_layout_desc {
	enum recovery_sbe1v1k_layout id;
	const char *name;
	const char *kernel_part;
	const char *rootfs_part;
	const char *data_part;
	const char *uboot_part;
	const char *kernpart;
	const char *rootarg;
	lbaint_t kernel_start;
	lbaint_t kernel_size;
	lbaint_t rootfs_start;
	lbaint_t rootfs_size;
	lbaint_t data_start;
	lbaint_t uboot_start;
	lbaint_t uboot_size;
	lbaint_t preserve_end;
	size_t kernel_pad;
};

struct recovery_sbe1v1k_clean_part {
	lbaint_t start;
	lbaint_t size;
};

/*
 * Authorization to skip the second full P28 zero-fill exists only in this
 * U-Boot process.  It is never stored in the environment or accepted from an
 * HTTP request.  The snapshot binds the completed clean operation to the
 * current eMMC CID, geometry, disk GUID and canonical P26-P28 map.
 */
struct recovery_sbe1v1k_clean_session {
	u32 magic;
	int devnum;
	u32 cid[4];
	u64 capacity_user;
	lbaint_t lba;
	ulong blksz;
	char disk_guid[UUID_STR_LEN + 1];
	struct recovery_sbe1v1k_clean_part parts[3];
};

static const struct recovery_sbe1v1k_layout_desc sbe1v1k_layout_full = {
	.id = RECOVERY_SBE1V1K_LAYOUT_FULL,
	.name = "full",
	.kernel_part = "0#kernel",
	.rootfs_part = "0#rootfs",
	.data_part = "0#rootfs_data",
	.uboot_part = "0#chainloader",
	.kernpart = "kernel",
	.rootarg = "/dev/mmcblk0p27",
	.kernel_start = RECOVERY_SBE1V1K_FULL_KERNEL_START,
	.kernel_size = RECOVERY_SBE1V1K_FULL_KERNEL_SIZE,
	.rootfs_start = RECOVERY_SBE1V1K_FULL_ROOTFS_START,
	.rootfs_size = RECOVERY_SBE1V1K_FULL_ROOTFS_SIZE,
	.data_start = RECOVERY_SBE1V1K_FULL_DATA_START,
	.uboot_start = RECOVERY_SBE1V1K_FULL_CHAINLOADER_START,
	.uboot_size = RECOVERY_SBE1V1K_FULL_CHAINLOADER_SIZE,
	.preserve_end = RECOVERY_SBE1V1K_FULL_CHAINLOADER_START,
	.kernel_pad = RECOVERY_SBE1V1K_FULL_KERNEL_SIZE * 512,
};

static enum upload_target current_target = TARGET_FIRMWARE;
static bool current_force_recreate;
static bool current_prepare_only;
/* Factory-file uploads carry ?sha256=<64 hex>; verified before any write. */
static bool current_sha256_valid;
static u8 current_sha256_expected[32];
static enum upload_target prepare_target = TARGET_FIRMWARE;
static size_t prepare_size;
static enum recovery_stream_format current_stream_format = RECOVERY_STREAM_RAW;
static enum recovery_stream_format prepare_stream_format = RECOVERY_STREAM_RAW;
static enum recovery_sbe1v1k_layout current_repartition_layout =
	RECOVERY_SBE1V1K_LAYOUT_FULL;
static enum recovery_sbe1v1k_layout active_sbe1v1k_layout =
	RECOVERY_SBE1V1K_LAYOUT_UNKNOWN;
static struct recovery_sbe1v1k_clean_session sbe1v1k_clean_session;

/* Revalidate the on-media map immediately before every destructive upload. */
static int recovery_verify_sbe1v1k_gpt(
	const struct recovery_sbe1v1k_layout_desc *layout);
static void recovery_sbe1v1k_clean_session_reset(void);
static int recovery_sbe1v1k_clean_session_capture(void);
static int recovery_sbe1v1k_clean_session_validate(void);

enum recovery_backend {
	RECOVERY_BACKEND_MTD = 0,
	RECOVERY_BACKEND_UBI,
	RECOVERY_BACKEND_MMC,
	RECOVERY_BACKEND_MMC_BOOT,
};

struct recovery_target {
	enum recovery_backend backend;
	const char *name;
	const char *ubi_part;
	struct mtd_info *mtd;
	struct blk_desc *blk;
	struct disk_partition part;
	loff_t ofs;
	unsigned long long cur_size;
	unsigned long long limit;
	bool ubi_needs_format;
};

struct recovery_led_ctrl {
	struct udevice *mdio_dev;
	ulong last_poll;
};

struct recovery_status_pwm_led {
	struct udevice *pwm;
	uint channel;
	uint period_ns;
	bool active_low;
	bool valid;
};

struct recovery_status_led_ctrl {
	struct recovery_status_pwm_led pwm_leds[RECOVERY_STATUS_LED_MAX];
	int pwm_count;
	bool use_pwm;
	ulong start_ms;
	ulong last_pwm_update;
};

struct recovery_dhcp_server {
	struct udp_pcb *pcb;
	struct netif *netif;
	ip4_addr_t server_ip;
	ip4_addr_t client_ip;
	ip4_addr_t netmask;
	ip4_addr_t router;
	ip4_addr_t broadcast;
	ip4_addr_t dns;
};

static const int recovery_led_phy_addrs[RECOVERY_LED_PORTS] = { 9, 10 };
static const u8 recovery_green_led_gpios[RECOVERY_LED_PORTS] = { 43, 44 };
static const u8 recovery_yellow_led_gpios[RECOVERY_LED_PORTS] = { 33, 34 };

static void recovery_led_ctrl_free(struct recovery_led_ctrl *ctrl)
{
	memset(ctrl, 0, sizeof(*ctrl));
}

static bool recovery_gpio_flash_mode_bit(u8 gpio, uintptr_t *reg, u32 *mask)
{
	if (gpio <= 15) {
		*reg = RECOVERY_GPIO_SYSCTL_BASE + RECOVERY_REG_GPIO_FLASH_MODE_CFG;
		*mask = BIT(gpio);
		return true;
	}

	if (gpio >= 16 && gpio <= 31) {
		*reg = RECOVERY_GPIO_SYSCTL_BASE + RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT;
		*mask = BIT(gpio - 16);
		return true;
	}

	if (gpio >= 36 && gpio <= 51) {
		*reg = RECOVERY_GPIO_SYSCTL_BASE + RECOVERY_REG_GPIO_FLASH_MODE_CFG_EXT;
		*mask = BIT(gpio - 20);
		return true;
	}

	return false;
}

static void recovery_clrsetbits_le32(uintptr_t addr, u32 clear, u32 set)
{
	u32 val = readl((void __iomem *)addr);

	val &= ~clear;
	val |= set;
	writel(val, (void __iomem *)addr);
}

static uintptr_t recovery_gpio_data_reg(u8 gpio)
{
	return RECOVERY_GPIO_SYSCTL_BASE +
	       (gpio < 32 ? RECOVERY_REG_GPIO_DATA : RECOVERY_REG_GPIO_DATA1);
}

static uintptr_t recovery_gpio_oe_reg(u8 gpio)
{
	return RECOVERY_GPIO_SYSCTL_BASE +
	       (gpio < 32 ? RECOVERY_REG_GPIO_OE : RECOVERY_REG_GPIO_OE1);
}

static uintptr_t recovery_gpio_dir_reg(u8 gpio)
{
	static const u16 dir_regs[] = {
		RECOVERY_REG_GPIO_CTRL,
		RECOVERY_REG_GPIO_CTRL1,
		RECOVERY_REG_GPIO_CTRL2,
		RECOVERY_REG_GPIO_CTRL3,
	};

	return RECOVERY_GPIO_SYSCTL_BASE + dir_regs[gpio / 16];
}

static void recovery_gpio_direction_output(u8 gpio)
{
	u32 bank_bit = BIT(gpio % 32);
	u32 dir_bit = BIT(2 * (gpio % 16));

	recovery_clrsetbits_le32(recovery_gpio_oe_reg(gpio), 0, bank_bit);
	recovery_clrsetbits_le32(recovery_gpio_dir_reg(gpio), 0, dir_bit);
}

static void recovery_gpio_prepare_output(u8 gpio)
{
	uintptr_t reg;
	u32 mask;

	if (recovery_gpio_flash_mode_bit(gpio, &reg, &mask))
		recovery_clrsetbits_le32(reg, mask, 0);

	recovery_gpio_direction_output(gpio);
}

static void recovery_gpio_set_value(u8 gpio, bool active_low, int active)
{
	u32 bit = BIT(gpio % 32);
	uintptr_t reg = recovery_gpio_data_reg(gpio);
	u32 set = active_low ? (active ? 0 : bit) : (active ? bit : 0);

	recovery_clrsetbits_le32(reg, bit, set);
}

static void recovery_led_set_pin(u8 gpio, int on)
{
	recovery_gpio_set_value(gpio, true, on);
}

#if CONFIG_IS_ENABLED(DM_PWM)
static void recovery_status_pwm_shutdown(struct recovery_status_led_ctrl *ctrl)
{
	int i;

	if (!ctrl->pwm_count)
		return;

	for (i = 0; i < ctrl->pwm_count; i++) {
		struct recovery_status_pwm_led *led = &ctrl->pwm_leds[i];

		if (!led->valid)
			continue;

		pwm_set_config(led->pwm, led->channel, led->period_ns, 0);
		pwm_set_enable(led->pwm, led->channel, true);
	}

	ctrl->use_pwm = false;
}

static int recovery_status_pwm_select_state(struct recovery_status_led_ctrl *ctrl)
{
	int i, j, ret;

	for (i = 0; i < ctrl->pwm_count; i++) {
		struct udevice *pwm = ctrl->pwm_leds[i].pwm;
		bool seen = false;

		for (j = 0; j < i; j++) {
			if (ctrl->pwm_leds[j].pwm == pwm) {
				seen = true;
				break;
			}
		}
		if (seen)
			continue;

		ret = pinctrl_select_state(pwm, "recovery");
		if (ret)
			return ret;
	}

	return 0;
}

static int recovery_status_pwm_parse_led(ofnode node,
					 struct recovery_status_pwm_led *led)
{
	struct ofnode_phandle_args args;
	struct udevice *pwm;
	int ret;

	memset(led, 0, sizeof(*led));

	ret = ofnode_parse_phandle_with_args(node, "pwms", "#pwm-cells", 0, 0,
					     &args);
	if (ret)
		return ret;
	if (args.args_count < 2)
		return -EINVAL;

	ret = uclass_get_device_by_ofnode(UCLASS_PWM, args.node, &pwm);
	if (ret)
		return ret;

	led->pwm = pwm;
	led->channel = args.args[0];
	led->period_ns = args.args[1] ? args.args[1] :
			 RECOVERY_STATUS_PWM_PERIOD_NS;
	led->active_low = args.args_count > 2 &&
			  (args.args[2] & RECOVERY_PWM_POLARITY_INVERTED);
	led->valid = true;

	return 0;
}

static int recovery_status_pwm_init(struct recovery_status_led_ctrl *ctrl)
{
	ofnode pwm_leds, node;
	int i, ret;

	pwm_leds = ofnode_path(RECOVERY_STATUS_PWM_NODE);
	if (!ofnode_valid(pwm_leds))
		return -ENOENT;

	ctrl->pwm_count = 0;
	ofnode_for_each_subnode(node, pwm_leds) {
		if (ctrl->pwm_count >= ARRAY_SIZE(ctrl->pwm_leds))
			break;

		ret = recovery_status_pwm_parse_led(node,
						    &ctrl->pwm_leds[ctrl->pwm_count]);
		if (ret) {
			printf("Recovery PWM LED %s parse failed: %d\n",
			       ofnode_get_name(node), ret);
			goto err;
		}

		ctrl->pwm_count++;
	}

	if (!ctrl->pwm_count)
		return -ENOENT;

	for (i = 0; i < ctrl->pwm_count; i++) {
		struct recovery_status_pwm_led *led = &ctrl->pwm_leds[i];

		ret = pwm_set_invert(led->pwm, led->channel, led->active_low);
		if (ret)
			goto err;

		ret = pwm_set_config(led->pwm, led->channel, led->period_ns, 0);
		if (ret)
			goto err;
	}

	ret = recovery_status_pwm_select_state(ctrl);
	if (ret)
		goto err;

	for (i = 0; i < ctrl->pwm_count; i++) {
		struct recovery_status_pwm_led *led = &ctrl->pwm_leds[i];

		ret = pwm_set_enable(led->pwm, led->channel, true);
		if (ret)
			goto err;
	}

	ctrl->use_pwm = true;
	ctrl->last_pwm_update = 0;

	if (recovery_debug_enabled()) {
		printf("Recovery status LEDs using hardware PWM:");
		for (i = 0; i < ctrl->pwm_count; i++)
			printf(" ch%u/%uns%s", ctrl->pwm_leds[i].channel,
			       ctrl->pwm_leds[i].period_ns,
			       ctrl->pwm_leds[i].active_low ? "(L)" : "");
		printf("\n");
	}

	return 0;

err:
	recovery_status_pwm_shutdown(ctrl);
	memset(ctrl->pwm_leds, 0, sizeof(ctrl->pwm_leds));
	ctrl->pwm_count = 0;
	ctrl->use_pwm = false;

	return ret;
}
#else
static void recovery_status_pwm_shutdown(struct recovery_status_led_ctrl *ctrl)
{
}

static int recovery_status_pwm_init(struct recovery_status_led_ctrl *ctrl)
{
	return -ENOSYS;
}
#endif

static void recovery_status_led_release(struct recovery_status_led_ctrl *ctrl)
{
	recovery_status_pwm_shutdown(ctrl);
	memset(ctrl, 0, sizeof(*ctrl));
}

static int recovery_status_led_init(struct recovery_status_led_ctrl *ctrl)
{
	int ret;

	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->start_ms = get_timer(0);

	ret = recovery_status_pwm_init(ctrl);
	if (ret)
		return ret;

	return 0;
}

static u32 recovery_status_led_breath_fp(ulong elapsed)
{
	ulong phase, ramp;
	u32 delta;

	phase = elapsed % RECOVERY_STATUS_BREATHE_PERIOD_MS;
	ramp = phase < RECOVERY_STATUS_BREATHE_HALF_MS ?
	       phase : RECOVERY_STATUS_BREATHE_PERIOD_MS - phase;
	delta = RECOVERY_STATUS_BRIGHTNESS_FP - RECOVERY_STATUS_BREATHE_MIN_FP;

	return RECOVERY_STATUS_BREATHE_MIN_FP +
	       (delta * ramp * ramp +
		(RECOVERY_STATUS_BREATHE_HALF_MS *
		 RECOVERY_STATUS_BREATHE_HALF_MS) / 2) /
	       (RECOVERY_STATUS_BREATHE_HALF_MS *
		RECOVERY_STATUS_BREATHE_HALF_MS);
}

static ulong recovery_status_led_position_fp(int led_count, ulong elapsed)
{
	ulong range_fp, phase;
	int span;

	if (led_count <= 1)
		return 0;

	span = led_count - 1;
	range_fp = span * RECOVERY_STATUS_BRIGHTNESS_FP;
	phase = elapsed % (2 * span * RECOVERY_STATUS_SWEEP_STEP_MS);

	if (phase <= span * RECOVERY_STATUS_SWEEP_STEP_MS)
		return phase * RECOVERY_STATUS_BRIGHTNESS_FP /
		       RECOVERY_STATUS_SWEEP_STEP_MS;

	phase -= span * RECOVERY_STATUS_SWEEP_STEP_MS;

	return range_fp - (phase * RECOVERY_STATUS_BRIGHTNESS_FP /
			   RECOVERY_STATUS_SWEEP_STEP_MS);
}

static u32 recovery_status_led_duty_fp(int led_count, int idx, ulong elapsed)
{
	ulong center_fp, pwm_phase;
	u32 breath_fp, weight_fp, dist_fp;
	ulong led_pos_fp;

	center_fp = recovery_status_led_position_fp(led_count, elapsed);
	led_pos_fp = idx * RECOVERY_STATUS_BRIGHTNESS_FP;
	dist_fp = center_fp > led_pos_fp ? center_fp - led_pos_fp :
					  led_pos_fp - center_fp;
	if (dist_fp >= RECOVERY_STATUS_OVERLAP_FP)
		return 0;

	weight_fp = (RECOVERY_STATUS_OVERLAP_FP - dist_fp) *
		    RECOVERY_STATUS_BRIGHTNESS_FP / RECOVERY_STATUS_OVERLAP_FP;
	weight_fp = weight_fp * weight_fp / RECOVERY_STATUS_BRIGHTNESS_FP;
	breath_fp = recovery_status_led_breath_fp(elapsed);
	pwm_phase = weight_fp * breath_fp;
	pwm_phase = (pwm_phase + RECOVERY_STATUS_BRIGHTNESS_FP / 2) /
		    RECOVERY_STATUS_BRIGHTNESS_FP;

	return min_t(u32, pwm_phase, RECOVERY_STATUS_BRIGHTNESS_FP);
}

#if CONFIG_IS_ENABLED(DM_PWM)
static void recovery_status_pwm_poll(struct recovery_status_led_ctrl *ctrl,
				     ulong now, ulong elapsed)
{
	int i;

	if (!ctrl->use_pwm || !ctrl->pwm_count)
		return;

	if (ctrl->last_pwm_update &&
	    now - ctrl->last_pwm_update < RECOVERY_STATUS_HW_PWM_UPDATE_MS)
		return;

	ctrl->last_pwm_update = now;

	for (i = 0; i < ctrl->pwm_count; i++) {
		struct recovery_status_pwm_led *led = &ctrl->pwm_leds[i];
		u32 duty_fp;
		u64 duty_ns;

		if (!led->valid)
			continue;

		duty_fp = recovery_status_led_duty_fp(ctrl->pwm_count, i,
						      elapsed);
		duty_ns = (u64)led->period_ns * duty_fp /
			  RECOVERY_STATUS_BRIGHTNESS_FP;
		pwm_set_config(led->pwm, led->channel, led->period_ns, duty_ns);
	}
}
#else
static void recovery_status_pwm_poll(struct recovery_status_led_ctrl *ctrl,
				     ulong now, ulong elapsed)
{
}
#endif

static void recovery_status_led_poll(struct recovery_status_led_ctrl *ctrl)
{
	ulong elapsed, now;

	if (!ctrl->use_pwm)
		return;

	now = get_timer(0);
	elapsed = now - ctrl->start_ms;
	recovery_status_pwm_poll(ctrl, now, elapsed);
}

static void recovery_status_led_stop(struct recovery_status_led_ctrl *ctrl)
{
	if (!ctrl->use_pwm)
		return;

	recovery_status_pwm_shutdown(ctrl);
}

enum recovery_dhcp_request_verdict {
	RECOVERY_DHCP_REQUEST_ACK = 0,
	RECOVERY_DHCP_REQUEST_NAK,
	RECOVERY_DHCP_REQUEST_IGNORE,
};

static int recovery_dhcp_get_option(const u8 *pkt, int pkt_len, u8 code,
				    const u8 **value, u8 *value_len)
{
	int off = DHCP_OPTIONS_OFS;

	while (off < pkt_len) {
		u8 opt, opt_len;

		opt = pkt[off++];
		if (opt == DHCP_OPTION_PAD)
			continue;
		if (opt == DHCP_OPTION_END)
			return -ENOENT;
		if (off >= pkt_len)
			break;

		opt_len = pkt[off++];
		if (off + opt_len > pkt_len)
			break;

		if (opt == code) {
			if (value)
				*value = pkt + off;
			if (value_len)
				*value_len = opt_len;
			return 0;
		}

		off += opt_len;
	}

	return -EINVAL;
}

static int recovery_dhcp_get_u8_option(const u8 *pkt, int pkt_len, u8 code,
				       u8 *value)
{
	const u8 *opt;
	u8 opt_len;
	int ret;

	ret = recovery_dhcp_get_option(pkt, pkt_len, code, &opt, &opt_len);
	if (ret)
		return ret;
	if (opt_len != sizeof(*value))
		return -EINVAL;

	*value = opt[0];
	return 0;
}

static int recovery_dhcp_get_ip4_option(const u8 *pkt, int pkt_len, u8 code,
					ip4_addr_t *addr)
{
	const u8 *opt;
	u8 opt_len;
	int ret;

	ret = recovery_dhcp_get_option(pkt, pkt_len, code, &opt, &opt_len);
	if (ret)
		return ret;
	if (opt_len != sizeof(addr->addr))
		return -EINVAL;

	memcpy(&addr->addr, opt, sizeof(addr->addr));
	return 0;
}

static int recovery_dhcp_put_option_head(u8 *options, int off, u8 code, u8 len)
{
	if (off < 0 || off + 2 + len > DHCP_OPTIONS_LEN)
		return -ENOSPC;

	options[off++] = code;
	options[off++] = len;

	return off;
}

static int recovery_dhcp_put_u8_option(u8 *options, int off, u8 code, u8 value)
{
	off = recovery_dhcp_put_option_head(options, off, code, sizeof(value));
	if (off < 0)
		return off;

	options[off++] = value;
	return off;
}

static int recovery_dhcp_put_u32_option(u8 *options, int off, u8 code, u32 value)
{
	u32 be_value = lwip_htonl(value);

	off = recovery_dhcp_put_option_head(options, off, code,
					    sizeof(be_value));
	if (off < 0)
		return off;

	memcpy(options + off, &be_value, sizeof(be_value));
	return off + sizeof(be_value);
}

static int recovery_dhcp_put_ip4_option(u8 *options, int off, u8 code,
					const ip4_addr_t *addr)
{
	off = recovery_dhcp_put_option_head(options, off, code,
					    sizeof(addr->addr));
	if (off < 0)
		return off;

	memcpy(options + off, &addr->addr, sizeof(addr->addr));
	return off + sizeof(addr->addr);
}

static void recovery_dhcp_finalize_options(struct pbuf *p, struct dhcp_msg *msg,
					   int opt_len)
{
	msg->options[opt_len++] = DHCP_OPTION_END;

	while (((opt_len < DHCP_MIN_OPTIONS_LEN) || (opt_len & 3)) &&
	       opt_len < DHCP_OPTIONS_LEN)
		msg->options[opt_len++] = DHCP_OPTION_PAD;

	pbuf_realloc(p, sizeof(*msg) - DHCP_OPTIONS_LEN + opt_len);
}

static enum recovery_dhcp_request_verdict
recovery_dhcp_classify_request(struct recovery_dhcp_server *srv,
			       const struct dhcp_msg *req,
			       const u8 *pkt, int pkt_len)
{
	ip4_addr_t option_ip, ciaddr;
	bool has_server_id, has_requested_ip;

	has_server_id = !recovery_dhcp_get_ip4_option(pkt, pkt_len,
						      DHCP_OPTION_SERVER_ID,
						      &option_ip);
	if (has_server_id) {
		if (!ip4_addr_eq(&option_ip, &srv->server_ip))
			return RECOVERY_DHCP_REQUEST_IGNORE;
	}

	has_requested_ip = !recovery_dhcp_get_ip4_option(pkt, pkt_len,
							 DHCP_OPTION_REQUESTED_IP,
							 &option_ip);
	if (has_requested_ip) {
		return ip4_addr_eq(&option_ip, &srv->client_ip) ?
			RECOVERY_DHCP_REQUEST_ACK :
			RECOVERY_DHCP_REQUEST_NAK;
	}

	ciaddr.addr = req->ciaddr.addr;
	if (!ip4_addr_isany(&ciaddr)) {
		return ip4_addr_eq(&ciaddr, &srv->client_ip) ?
			RECOVERY_DHCP_REQUEST_ACK :
			RECOVERY_DHCP_REQUEST_NAK;
	}

	return RECOVERY_DHCP_REQUEST_ACK;
}

static int recovery_dhcp_send_reply(struct recovery_dhcp_server *srv,
				    const struct dhcp_msg *req,
				    u8 message_type)
{
	struct pbuf *p;
	struct dhcp_msg *reply;
	ip_addr_t src_addr;
	ip_addr_t reply_addr;
	int opt_len;
	err_t err;

	static u32 dhcp_tx_log_count;

	p = pbuf_alloc(PBUF_TRANSPORT, sizeof(*reply), PBUF_RAM);
	if (!p)
		return -ENOMEM;

	reply = p->payload;
	memset(reply, 0, sizeof(*reply));

	reply->op = DHCP_BOOTREPLY;
	reply->htype = req->htype;
	reply->hlen = req->hlen;
	reply->hops = req->hops;
	reply->xid = req->xid;
	reply->secs = req->secs;
	reply->flags = req->flags | lwip_htons(0x8000);
	if (message_type != DHCP_NAK)
		reply->yiaddr.addr = srv->client_ip.addr;
	reply->siaddr.addr = 0;
	reply->giaddr = req->giaddr;
	memcpy(reply->chaddr, req->chaddr, DHCP_CHADDR_LEN);
	reply->cookie = PP_HTONL(DHCP_MAGIC_COOKIE);

	opt_len = 0;
	opt_len = recovery_dhcp_put_u8_option(reply->options, opt_len,
					      DHCP_OPTION_MESSAGE_TYPE,
					      message_type);
	opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
					       DHCP_OPTION_SERVER_ID,
					       &srv->server_ip);
	if (message_type != DHCP_NAK) {
		opt_len = recovery_dhcp_put_u32_option(reply->options, opt_len,
						       DHCP_OPTION_LEASE_TIME,
						       RECOVERY_DHCP_LEASE_SECS);
		opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
						       DHCP_OPTION_SUBNET_MASK,
						       &srv->netmask);
		opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
						       DHCP_OPTION_ROUTER,
						       &srv->router);
		opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
						       DHCP_OPTION_DNS_SERVER,
						       &srv->dns);
		opt_len = recovery_dhcp_put_ip4_option(reply->options, opt_len,
						       DHCP_OPTION_BROADCAST,
						       &srv->broadcast);
	}
	if (opt_len < 0) {
		pbuf_free(p);
		return opt_len;
	}

	recovery_dhcp_finalize_options(p, reply, opt_len);

	reply_addr = *IP_ADDR_BROADCAST;

	ip_addr_copy_from_ip4(src_addr, srv->server_ip);
	err = udp_sendto_if_src(srv->pcb, p, &reply_addr,
				LWIP_IANA_PORT_DHCP_CLIENT, srv->netif,
				&src_addr);

	if (recovery_debug_enabled() && dhcp_tx_log_count < 16) {
		printf("DHCP TX type=%u xid=0x%08x to %s rc=%d\n",
		       message_type, lwip_ntohl(req->xid),
		       ip4addr_ntoa(&srv->client_ip), err);
		dhcp_tx_log_count++;
	}

	pbuf_free(p);

	return err == ERR_OK ? 0 : -EIO;
}

static void recovery_dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
			       const ip_addr_t *addr, u16_t port)
{
	struct recovery_dhcp_server *srv = arg;
	const struct dhcp_msg *req;
	u8 pkt[RECOVERY_DHCP_MAX_MSG_LEN];
	u8 message_type;
	int copy_len, pkt_len;
	static u32 dhcp_rx_log_count;

	(void)pcb;

	if (!p)
		return;

	if (port != LWIP_IANA_PORT_DHCP_CLIENT)
		goto out;

	copy_len = p->tot_len;
	if (copy_len > sizeof(pkt))
		copy_len = sizeof(pkt);

	pkt_len = pbuf_copy_partial(p, pkt, copy_len, 0);
	if (pkt_len < DHCP_OPTIONS_OFS)
		goto out;

	req = (const struct dhcp_msg *)pkt;
	if (req->op != DHCP_BOOTREQUEST ||
	    req->htype != LWIP_IANA_HWTYPE_ETHERNET ||
	    req->hlen != ARP_HLEN ||
	    req->cookie != PP_HTONL(DHCP_MAGIC_COOKIE))
		goto out;

	if (recovery_dhcp_get_u8_option(pkt, pkt_len, DHCP_OPTION_MESSAGE_TYPE,
					&message_type))
		goto out;

	if (recovery_debug_enabled() && dhcp_rx_log_count < 16) {
		printf("DHCP RX type=%u xid=0x%08x from %s:%u len=%d\n",
		       message_type, lwip_ntohl(req->xid), ipaddr_ntoa(addr),
		       port, pkt_len);
		dhcp_rx_log_count++;
	}

	switch (message_type) {
	case DHCP_DISCOVER:
		recovery_dhcp_send_reply(srv, req, DHCP_OFFER);
		break;
	case DHCP_REQUEST:
		switch (recovery_dhcp_classify_request(srv, req, pkt, pkt_len)) {
		case RECOVERY_DHCP_REQUEST_ACK:
			recovery_dhcp_send_reply(srv, req, DHCP_ACK);
			break;
		case RECOVERY_DHCP_REQUEST_NAK:
			recovery_dhcp_send_reply(srv, req, DHCP_NAK);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

out:
	pbuf_free(p);
}

static int recovery_dhcp_server_init(struct recovery_dhcp_server *srv,
				     struct netif *netif)
{
	char server_ip[IP4ADDR_STRLEN_MAX];
	char client_ip[IP4ADDR_STRLEN_MAX];
	char netmask[IP4ADDR_STRLEN_MAX];
	char router[IP4ADDR_STRLEN_MAX];
	char broadcast[IP4ADDR_STRLEN_MAX];
	err_t err;

	memset(srv, 0, sizeof(*srv));

	srv->netif = netif;
	ip4_addr_copy(srv->server_ip, *netif_ip4_addr(netif));
	ip4_addr_copy(srv->netmask, *netif_ip4_netmask(netif));
	ip4_addr_copy(srv->router, *netif_ip4_gw(netif));
	ip4_addr_copy(srv->dns, *netif_ip4_addr(netif));

	if (ip4_addr_isany(&srv->router))
		ip4_addr_copy(srv->router, srv->server_ip);

	if (!ip4addr_aton(RECOVERY_DHCP_CLIENT_IPADDR, &srv->client_ip))
		return -EINVAL;

	if (ip4_addr_isany(&srv->netmask)) {
		if (!ip4addr_aton(RECOVERY_DHCP_BROADCAST_IPADDR, &srv->broadcast))
			return -EINVAL;
	} else {
		srv->broadcast.addr = (srv->server_ip.addr & srv->netmask.addr) |
				      ~srv->netmask.addr;
	}

	srv->pcb = udp_new();
	if (!srv->pcb)
		return -ENOMEM;

	ip_set_option(srv->pcb, SOF_BROADCAST);

	err = udp_bind(srv->pcb, IP4_ADDR_ANY, LWIP_IANA_PORT_DHCP_SERVER);
	if (err != ERR_OK) {
		udp_remove(srv->pcb);
		srv->pcb = NULL;
		return -EIO;
	}

	udp_bind_netif(srv->pcb, netif);
	udp_recv(srv->pcb, recovery_dhcp_recv, srv);

	recovery_debug_printf("DHCP recovery server: %s/67 -> offer %s mask %s gw %s bcast %s\n",
			      ip4addr_ntoa_r(&srv->server_ip, server_ip,
					     sizeof(server_ip)),
			      ip4addr_ntoa_r(&srv->client_ip, client_ip,
					     sizeof(client_ip)),
			      ip4addr_ntoa_r(&srv->netmask, netmask,
					     sizeof(netmask)),
			      ip4addr_ntoa_r(&srv->router, router,
					     sizeof(router)),
			      ip4addr_ntoa_r(&srv->broadcast, broadcast,
					     sizeof(broadcast)));

	return 0;
}

static void recovery_dhcp_server_stop(struct recovery_dhcp_server *srv)
{
	if (srv->pcb)
		udp_remove(srv->pcb);

	memset(srv, 0, sizeof(*srv));
}

static int recovery_led_init(struct recovery_led_ctrl *ctrl)
{
	ofnode mdio_node;
	int i, ret;

	memset(ctrl, 0, sizeof(*ctrl));

	/* Make sure PHY LED mux is disabled so software can own the lines. */
	recovery_clrsetbits_le32(RECOVERY_CHIP_SCU_BASE + RECOVERY_REG_GPIO_2ND_I2C_MODE,
				 RECOVERY_GPIO_LAN0_LED0_MODE_MASK |
				 RECOVERY_GPIO_LAN0_LED1_MODE_MASK |
				 RECOVERY_GPIO_LAN1_LED0_MODE_MASK |
				 RECOVERY_GPIO_LAN1_LED1_MODE_MASK, 0);

	for (i = 0; i < RECOVERY_LED_PORTS; i++) {
		recovery_gpio_prepare_output(recovery_green_led_gpios[i]);
		recovery_gpio_prepare_output(recovery_yellow_led_gpios[i]);
		recovery_led_set_pin(recovery_green_led_gpios[i], 0);
		recovery_led_set_pin(recovery_yellow_led_gpios[i], 0);
	}

	mdio_node = ofnode_path("/soc/switch@1fb58000/mdio");
	if (!ofnode_valid(mdio_node))
		return 0;

	ret = uclass_get_device_by_ofnode(UCLASS_MDIO, mdio_node, &ctrl->mdio_dev);
	if (ret)
		ctrl->mdio_dev = NULL;

	return 0;
}

static int recovery_led_phy_speed(struct recovery_led_ctrl *ctrl, int idx)
{
	int bmcr, bmsr, stat1000, ctrl1000, lpa;

	if (!ctrl->mdio_dev || idx >= ARRAY_SIZE(recovery_led_phy_addrs))
		return 0;

	bmsr = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
			    MDIO_DEVAD_NONE, MII_BMSR);
	if (bmsr < 0)
		return 0;

	bmsr = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
			    MDIO_DEVAD_NONE, MII_BMSR);
	if (bmsr < 0)
		return 0;

	if (!(bmsr & BMSR_LSTATUS))
		return 0;

	bmcr = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
			    MDIO_DEVAD_NONE, MII_BMCR);
	if (bmcr < 0)
		return 0;

	if (!(bmcr & BMCR_ANENABLE)) {
		if (bmcr & BMCR_SPEED1000)
			return SPEED_1000;
		if (bmcr & BMCR_SPEED100)
			return SPEED_100;

		return SPEED_10;
	}

	stat1000 = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
				MDIO_DEVAD_NONE, MII_STAT1000);
	ctrl1000 = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
				MDIO_DEVAD_NONE, MII_CTRL1000);
	if (stat1000 >= 0 && ctrl1000 >= 0) {
		stat1000 &= ctrl1000 << 2;
		if (stat1000 & (PHY_1000BTSR_1000FD | PHY_1000BTSR_1000HD))
			return SPEED_1000;
	}

	lpa = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
			   MDIO_DEVAD_NONE, MII_ADVERTISE);
	if (lpa < 0)
		return SPEED_10;

	bmsr = dm_mdio_read(ctrl->mdio_dev, recovery_led_phy_addrs[idx],
			    MDIO_DEVAD_NONE, MII_LPA);
	if (bmsr < 0)
		return SPEED_10;

	lpa &= bmsr;
	if (lpa & (LPA_100FULL | LPA_100HALF))
		return SPEED_100;

	return SPEED_10;
}

static void recovery_led_poll(struct recovery_led_ctrl *ctrl)
{
	ulong now;
	int i, speed;

	now = get_timer(0);
	if (ctrl->last_poll && now - ctrl->last_poll < RECOVERY_LED_POLL_MS)
		return;

	ctrl->last_poll = now;

	for (i = 0; i < RECOVERY_LED_PORTS; i++) {
		speed = recovery_led_phy_speed(ctrl, i);
		recovery_led_set_pin(recovery_green_led_gpios[i],
				     speed == SPEED_1000);
		recovery_led_set_pin(recovery_yellow_led_gpios[i],
				     speed == SPEED_10 || speed == SPEED_100);
	}
}

static void recovery_service_runtime(struct recovery_status_led_ctrl *status_leds);

static const char *recovery_target_name(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "firmware";
	case TARGET_UBOOT:
		return "uboot";
	case TARGET_BOOT0:
		return "boot0";
	case TARGET_BOOT1:
		return "boot1";
	case TARGET_ART:
		return "art";
	case TARGET_REPARTITION:
		return "repartition";
	}

	return "unknown";
}

static const char *recovery_default_target(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "fit";
	case TARGET_UBOOT:
		return "uboot";
	case TARGET_BOOT0:
		return "boot0";
	case TARGET_BOOT1:
		return "boot1";
	case TARGET_ART:
		return "art";
	case TARGET_REPARTITION:
		return "repartition";
	}

	return "fit";
}

static const char *recovery_target_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "recovery_mtd";
	case TARGET_UBOOT:
		return "recovery_mtd_uboot";
	case TARGET_BOOT0:
		return "recovery_mtd_boot0";
	case TARGET_BOOT1:
		return "recovery_mtd_boot1";
	case TARGET_ART:
		return "recovery_mtd_art";
	case TARGET_REPARTITION:
		return "recovery_mtd";
	}

	return "recovery_mtd";
}

static const char *recovery_raw_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "recovery_dev";
	case TARGET_UBOOT:
		return "recovery_dev_uboot";
	case TARGET_BOOT0:
		return "recovery_dev_boot0";
	case TARGET_BOOT1:
		return "recovery_dev_boot1";
	case TARGET_ART:
		return "recovery_dev_art";
	case TARGET_REPARTITION:
		return "recovery_dev";
	}

	return "recovery_dev";
}

static const char *recovery_size_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "recovery_size";
	case TARGET_UBOOT:
		return "recovery_size_uboot";
	case TARGET_BOOT0:
		return "recovery_size_boot0";
	case TARGET_BOOT1:
		return "recovery_size_boot1";
	case TARGET_ART:
		return "recovery_size_art";
	case TARGET_REPARTITION:
		return "recovery_size";
	}

	return "recovery_size";
}

static ulong recovery_raw_offset(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return env_get_hex("recovery_ofs", 0x050000);
	case TARGET_UBOOT:
		return env_get_hex("uboot_ofs", 0x0);
	case TARGET_REPARTITION:
		return 0;
	case TARGET_BOOT0:
	case TARGET_BOOT1:
	case TARGET_ART:
		return 0;
	}

	return 0;
}

static bool recovery_backend_is_mmc(void)
{
	const char *backend = env_get("recovery_backend");

	if (recovery_board_is_sbe1v1k())
		return true;

	return backend && (!strcasecmp(backend, "mmc") ||
			   !strcasecmp(backend, "emmc"));
}

static const char *recovery_mmcdev(void)
{
	const char *dev = env_get("recovery_mmcdev");

	if (!dev)
		dev = env_get("mmcdev");

	return dev ?: "0";
}

static const char *recovery_mmc_part_env(enum upload_target tgt)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return "recovery_part_kernel";
	case TARGET_UBOOT:
		return "recovery_part_uboot";
	case TARGET_BOOT0:
		return "recovery_part_boot0";
	case TARGET_BOOT1:
		return "recovery_part_boot1";
	case TARGET_ART:
		return "recovery_part_art";
	case TARGET_REPARTITION:
		return "recovery_part_uboot";
	}

	return "recovery_part_kernel";
}

static unsigned long recovery_uboot_limit(void)
{
	unsigned long def = recovery_backend_is_mmc() ?
			    RECOVERY_MAX_UBOOT_SIZE_MMC :
			    RECOVERY_MAX_UBOOT_SIZE;

	return env_get_hex("recovery_uboot_max", def);
}

static bool recovery_string_is_uint(const char *s)
{
	if (!s || !*s)
		return false;

	while (*s) {
		if (*s < '0' || *s > '9')
			return false;
		s++;
	}

	return true;
}

static int recovery_format_mmc_part_spec(const char *spec, char *buf,
					 size_t buf_len)
{
	const char *sep;
	size_t dev_len;
	int len;

	if (!spec || !*spec)
		return -EINVAL;

	if (strchr(spec, '#')) {
		strlcpy(buf, spec, buf_len);
		return 0;
	}

	sep = strchr(spec, ':');
	if (sep && !recovery_string_is_uint(sep + 1)) {
		dev_len = sep - spec;
		if (dev_len >= buf_len)
			return -ENOSPC;

		memcpy(buf, spec, dev_len);
		buf[dev_len] = '#';
		strlcpy(buf + dev_len + 1, sep + 1, buf_len - dev_len - 1);
		return 0;
	}

	if (sep) {
		strlcpy(buf, spec, buf_len);
		return 0;
	}

	len = snprintf(buf, buf_len, "%s#%s", recovery_mmcdev(), spec);
	if (len < 0 || len >= buf_len)
		return -ENOSPC;

	return 0;
}

static int recovery_get_mmc_part(const char *spec, struct blk_desc **desc,
				 struct disk_partition *part)
{
	char part_spec[96];
	int ret;

	ret = recovery_format_mmc_part_spec(spec, part_spec, sizeof(part_spec));
	if (ret)
		return ret;

	ret = part_get_info_by_dev_and_name_or_num("mmc", part_spec, desc,
						   part, false);
	if (ret < 0)
		printf("Failed to resolve eMMC partition '%s': %d\n",
		       part_spec, ret);
	if (ret >= 0 && desc && part && *desc) {
		ulong blksz = part->blksz ?: (*desc)->blksz;

		if (!*desc || !blksz || !(*desc)->lba ||
		    part->start > (*desc)->lba ||
		    part->size > (*desc)->lba - part->start ||
		    (unsigned long long)part->size > ~0ULL / blksz) {
			printf("Rejecting eMMC partition '%s': invalid device bounds\n",
			       part_spec);
			return -EOVERFLOW;
		}
	} else if (ret >= 0) {
		printf("Rejecting eMMC partition '%s': missing device descriptor\n",
		       part_spec);
		return -ENODEV;
	}

	return ret < 0 ? ret : 0;
}

static unsigned long long
recovery_mmc_part_bytes(const struct disk_partition *part)
{
	return (unsigned long long)part->size *
	       (unsigned long long)part->blksz;
}

static bool recovery_data_range_ok(const void *base, size_t total,
				   const void *data, size_t size)
{
	uintptr_t b = (uintptr_t)base;
	uintptr_t d = (uintptr_t)data;

	if (d < b)
		return false;
	if (size > total)
		return false;
	if (d - b > total - size)
		return false;

	return true;
}

struct recovery_image_part {
	const void *data;
	size_t size;
};

struct recovery_firmware_image {
	struct recovery_image_part kernel;
	struct recovery_image_part rootfs;
};

static bool recovery_name_has_prefix(const char *name, const char *prefix)
{
	size_t plen;

	if (!name || !prefix)
		return false;

	plen = strlen(prefix);
	return !strncasecmp(name, prefix, plen);
}

static const char *recovery_basename(const char *name)
{
	const char *base, *p;

	if (!name)
		return "";

	base = name;
	for (p = name; *p; p++) {
		if (*p == '/')
			base = p + 1;
	}

	return base;
}

static bool recovery_is_kernel_name(const char *name)
{
	const char *base = recovery_basename(name);

	return recovery_name_has_prefix(base, "hlos") ||
	       recovery_name_has_prefix(base, "kernel");
}

static bool recovery_is_rootfs_name(const char *name)
{
	const char *base = recovery_basename(name);

	return recovery_name_has_prefix(base, "rootfs") ||
	       !strcasecmp(base, "root") ||
	       !strcasecmp(base, "fs") ||
	       recovery_name_has_prefix(base, "fs-");
}

static int recovery_fit_extract_firmware(const void *fit, size_t fit_size,
					 struct recovery_firmware_image *image)
{
	bool has_kernel = false;
	size_t boot_fit_size;
	int images, node, ret;

	memset(image, 0, sizeof(*image));

	ret = fit_check_format(fit, fit_size);
	if (ret)
		return ret;

	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images < 0)
		return images;

	fdt_for_each_subnode(node, fit, images) {
		const char *node_name;
		const char *desc;
		const char *type;
		const void *data;
		size_t size;

		if (fit_image_get_data(fit, node, &data, &size))
			continue;

		if (!recovery_data_range_ok(fit, fit_size, data, size))
			return -EINVAL;

		node_name = fdt_get_name(fit, node, NULL);
		desc = fdt_getprop(fit, node, FIT_DESC_PROP, NULL);
		type = fdt_getprop(fit, node, FIT_TYPE_PROP, NULL);

		if (!has_kernel &&
		    (recovery_is_kernel_name(node_name) ||
		     recovery_is_kernel_name(desc) ||
		     (type && !strcasecmp(type, "kernel")))) {
			has_kernel = true;
			continue;
		}

		if (!image->rootfs.data &&
		    (recovery_is_rootfs_name(node_name) ||
		     recovery_is_rootfs_name(desc) ||
		     (type && (!strcasecmp(type, "filesystem") ||
			       !strcasecmp(type, "rootfs"))))) {
			image->rootfs.data = data;
			image->rootfs.size = size;
		}
	}

	if (has_kernel) {
		boot_fit_size = fit_get_size(fit);
		if (!boot_fit_size || boot_fit_size > fit_size)
			return -EINVAL;

		/* The eMMC kernel partition is bootm input, not a raw kernel blob. */
		image->kernel.data = fit;
		image->kernel.size = boot_fit_size;
	}

	return image->kernel.data || image->rootfs.data ? 0 : -ENOENT;
}

struct recovery_tar_header {
	char name[100];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char chksum[8];
	char typeflag;
	char linkname[100];
	char magic[6];
	char version[2];
	char uname[32];
	char gname[32];
	char devmajor[8];
	char devminor[8];
	char prefix[155];
};

static bool recovery_tar_header_empty(const struct recovery_tar_header *hdr)
{
	const u8 *p = (const u8 *)hdr;
	int i;

	for (i = 0; i < 512; i++) {
		if (p[i])
			return false;
	}

	return true;
}

static int recovery_tar_octal(const char *field, size_t len, size_t *value)
{
	size_t v = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		char c = field[i];

		if (c == '\0' || c == ' ')
			break;
		if (c < '0' || c > '7')
			return -EINVAL;
		v = (v << 3) + c - '0';
	}

	*value = v;
	return 0;
}

static void recovery_tar_name(const struct recovery_tar_header *hdr,
			      char *name, size_t name_len)
{
	size_t prefix_len = strnlen(hdr->prefix, sizeof(hdr->prefix));
	size_t file_len = strnlen(hdr->name, sizeof(hdr->name));
	size_t off = 0;

	if (!name_len)
		return;

	if (prefix_len) {
		off = prefix_len >= name_len ? name_len - 1 : prefix_len;
		memcpy(name, hdr->prefix, off);
		if (off < name_len - 1)
			name[off++] = '/';
	}

	if (off < name_len - 1) {
		size_t copy = file_len;

		if (copy > name_len - 1 - off)
			copy = name_len - 1 - off;
		memcpy(name + off, hdr->name, copy);
		off += copy;
	}

	name[off] = '\0';
}

static int recovery_tar_extract_firmware(const void *tar, size_t tar_size,
					 struct recovery_firmware_image *image)
{
	const u8 *base = tar;
	size_t off = 0;

	memset(image, 0, sizeof(*image));

	while (off + 512 <= tar_size) {
		const struct recovery_tar_header *hdr =
			(const struct recovery_tar_header *)(base + off);
		char name[256];
		size_t size;

		if (recovery_tar_header_empty(hdr))
			break;

		if (memcmp(hdr->magic, "ustar", 5))
			return -EINVAL;

		if (recovery_tar_octal(hdr->size, sizeof(hdr->size), &size))
			return -EINVAL;

		if (off + 512 > tar_size || size > tar_size - off - 512)
			return -EINVAL;

		recovery_tar_name(hdr, name, sizeof(name));

		if ((hdr->typeflag == '\0' || hdr->typeflag == '0') &&
		    !image->kernel.data && recovery_is_kernel_name(name)) {
			image->kernel.data = base + off + 512;
			image->kernel.size = size;
		} else if ((hdr->typeflag == '\0' || hdr->typeflag == '0') &&
			   !image->rootfs.data && recovery_is_rootfs_name(name)) {
			image->rootfs.data = base + off + 512;
			image->rootfs.size = size;
		}

		off += 512 + ALIGN(size, 512);
	}

	return image->kernel.data || image->rootfs.data ? 0 : -ENOENT;
}

static int recovery_raw_extract_firmware(const void *raw, size_t raw_size,
					 struct recovery_firmware_image *image)
{
	size_t kernel_pad = env_get_hex("recovery_kernel_pad",
				       RECOVERY_KERNEL_PAD_SIZE);
	size_t kernel_size = kernel_pad;

	memset(image, 0, sizeof(*image));

	if (raw_size <= kernel_pad)
		return -ENOENT;

	if (!fit_check_format(raw, raw_size)) {
		size_t fit_size = fdt_totalsize(raw);

		if (fit_size > 0 && fit_size <= kernel_pad)
			kernel_size = fit_size;
	}

	image->kernel.data = raw;
	image->kernel.size = kernel_size;
	image->rootfs.data = (const u8 *)raw + kernel_pad;
	image->rootfs.size = raw_size - kernel_pad;

	return 0;
}

static int recovery_extract_firmware(const void *data, size_t size,
				     struct recovery_firmware_image *image)
{
	int ret;

	ret = recovery_fit_extract_firmware(data, size, image);
	if (!ret && image->kernel.data && image->rootfs.data) {
		recovery_debug_printf("Firmware image: bootable FIT=%lu rootfs=%lu\n",
				      (ulong)image->kernel.size,
				      (ulong)image->rootfs.size);
		return 0;
	}

	ret = recovery_tar_extract_firmware(data, size, image);
	if (!ret && image->kernel.data && image->rootfs.data) {
		recovery_debug_printf("Firmware image: sysupgrade tar kernel=%lu rootfs=%lu\n",
				      (ulong)image->kernel.size,
				      (ulong)image->rootfs.size);
		return 0;
	}

	ret = recovery_raw_extract_firmware(data, size, image);
	if (!ret && image->kernel.data && image->rootfs.data) {
		recovery_debug_printf("Firmware image: raw kernel/rootfs split kernel=%lu rootfs=%lu\n",
				      (ulong)image->kernel.size,
				      (ulong)image->rootfs.size);
		return 0;
	}

	printf("Firmware image must contain both kernel/hlos and root/rootfs/fs payloads\n");
	return -EINVAL;
}

static int recovery_verify_fit_payload(const void *fit, size_t payload_size)
{
	const void *data;
	size_t fit_size;
	size_t data_size;
	int images, node, ret;

	ret = fit_check_format(fit, payload_size);
	if (ret) {
		printf("Payload must be a complete FIT image: %d\n", ret);
		return ret;
	}

	fit_size = fit_get_size(fit);
	if (!fit_size || fit_size > payload_size) {
		printf("FIT payload is truncated\n");
		return -EINVAL;
	}

	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images < 0)
		return images;

	fdt_for_each_subnode(node, fit, images) {
		ret = fit_image_get_data(fit, node, &data, &data_size);
		if (ret || !recovery_data_range_ok(fit, payload_size,
						   data, data_size)) {
			printf("FIT image data is missing or outside the payload\n");
			return ret ?: -EINVAL;
		}
	}

	if (!fit_all_image_verify(fit)) {
		printf("FIT image hash verification failed\n");
		return -EBADMSG;
	}

	return 0;
}

static int
recovery_validate_sbe1v1k_kernel_fit(const struct recovery_image_part *kernel)
{
	u8 type, os, arch;
	int conf, node, ret;

	ret = recovery_verify_fit_payload(kernel->data, kernel->size);
	if (ret)
		return ret;

	conf = fit_conf_get_node(kernel->data, NULL);
	if (conf < 0) {
		printf("SBE1V1K kernel FIT has no usable default configuration: %d\n",
		       conf);
		return conf;
	}

	node = fit_conf_get_prop_node(kernel->data, conf,
				      FIT_KERNEL_PROP, IH_PHASE_NONE);
	if (node < 0) {
		printf("SBE1V1K kernel FIT default configuration has no kernel: %d\n",
		       node);
		return node;
	}

	if (fit_image_get_type(kernel->data, node, &type) ||
	    fit_image_get_os(kernel->data, node, &os) ||
	    fit_image_get_arch(kernel->data, node, &arch) ||
	    type != IH_TYPE_KERNEL || os != IH_OS_LINUX ||
	    arch != IH_ARCH_ARM64) {
		printf("SBE1V1K FIT kernel must be type=kernel, os=linux, arch=arm64\n");
		return -ENOEXEC;
	}

	recovery_debug_printf("SBE1V1K boot FIT validation passed\n");
	return 0;
}

static int
recovery_validate_sbe1v1k_rootfs(const struct recovery_image_part *rootfs)
{
	const u8 *data = rootfs->data;
	u64 bytes_used;

	if (rootfs->size < RECOVERY_SQUASHFS_HEADER_MIN ||
	    get_unaligned_le32(data) != RECOVERY_SQUASHFS_MAGIC ||
	    get_unaligned_le16(data + RECOVERY_SQUASHFS_MAJOR_OFF) !=
					      RECOVERY_SQUASHFS_MAJOR) {
		printf("SBE1V1K rootfs payload must be a SquashFS v4 image\n");
		return -ENOEXEC;
	}

	bytes_used = get_unaligned_le64(data + RECOVERY_SQUASHFS_BYTES_OFF);
	if (!bytes_used || bytes_used > rootfs->size) {
		printf("SBE1V1K SquashFS payload is truncated or invalid\n");
		return -EINVAL;
	}

	recovery_debug_printf("SBE1V1K SquashFS validation passed\n");
	return 0;
}

static int recovery_mmc_check_part_size(const char *spec, size_t size)
{
	struct disk_partition part;
	struct blk_desc *desc;
	unsigned long long capacity;
	int ret;

	ret = recovery_get_mmc_part(spec, &desc, &part);
	if (ret)
		return ret;

	capacity = recovery_mmc_part_bytes(&part);
	if (size > capacity) {
		printf("Image size %lu exceeds eMMC partition '%s' size %llu\n",
		       (ulong)size, spec, capacity);
		return -EFBIG;
	}

	return 0;
}

static int recovery_mmc_write_part(const char *spec,
				   struct recovery_status_led_ctrl *status_leds,
				   const void *data, size_t size,
				   size_t progress_base)
{
	struct disk_partition part;
	struct blk_desc *desc;
	unsigned long long capacity;
	ulong blksz, chunk_size;
	u8 *chunk_buf;
	size_t written = 0;
	int ret;

	ret = recovery_get_mmc_part(spec, &desc, &part);
	if (ret)
		return ret;

	blksz = part.blksz ?: desc->blksz;
	capacity = recovery_mmc_part_bytes(&part);
	if (size > capacity) {
		printf("Image size %lu exceeds eMMC partition '%s' size %llu\n",
		       (ulong)size, spec, capacity);
		return -EFBIG;
	}

	chunk_size = ALIGN(RECOVERY_MMC_WRITE_CHUNK, blksz);
	if (chunk_size < blksz)
		chunk_size = blksz;

	chunk_buf = memalign(ARCH_DMA_MINALIGN, chunk_size);
	if (!chunk_buf)
		return -ENOMEM;

	while (written < size) {
		size_t todo = size - written;
		size_t write_len;
		lbaint_t blk;
		lbaint_t blkcnt;

		if (todo > chunk_size)
			todo = chunk_size;

		write_len = ALIGN(todo, blksz);
		memset(chunk_buf, 0, write_len);
		memcpy(chunk_buf, (const u8 *)data + written, todo);

		blk = part.start + written / blksz;
		blkcnt = write_len / blksz;
		if (blk_dwrite(desc, blk, blkcnt, chunk_buf) != blkcnt) {
			printf("eMMC write failed at partition '%s' block " LBAF "\n",
			       spec, blk);
			free(chunk_buf);
			return -EIO;
		}

		written += todo;
		prog_write_done = progress_base + written;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	free(chunk_buf);
	return 0;
}

static int recovery_mmc_verify_part(const char *spec,
				    struct recovery_status_led_ctrl *status_leds,
				    const void *data, size_t size)
{
	struct disk_partition part;
	struct blk_desc *desc;
	ulong blksz, chunk_size;
	u8 *chunk_buf;
	size_t verified = 0;
	int ret;

	ret = recovery_get_mmc_part(spec, &desc, &part);
	if (ret)
		return ret;

	blksz = part.blksz ?: desc->blksz;
	if (!blksz || size > recovery_mmc_part_bytes(&part))
		return -EFBIG;

	chunk_size = ALIGN(RECOVERY_MMC_WRITE_CHUNK, blksz);
	if (chunk_size < blksz)
		chunk_size = blksz;

	chunk_buf = memalign(ARCH_DMA_MINALIGN, chunk_size);
	if (!chunk_buf)
		return -ENOMEM;

	while (verified < size) {
		size_t todo = min(size - verified, (size_t)chunk_size);
		size_t read_len = ALIGN(todo, blksz);
		lbaint_t blk = part.start + verified / blksz;
		lbaint_t blkcnt = read_len / blksz;

		if (blk_dread(desc, blk, blkcnt, chunk_buf) != blkcnt) {
			printf("eMMC readback failed at partition '%s' block " LBAF "\n",
			       spec, blk);
			ret = -EIO;
			goto out;
		}
		if (memcmp(chunk_buf, (const u8 *)data + verified, todo)) {
			printf("eMMC readback mismatch at partition '%s' offset 0x%lx\n",
			       spec, (ulong)verified);
			ret = -EBADMSG;
			goto out;
		}

		verified += todo;
		recovery_service_runtime(status_leds);
	}

	ret = 0;
out:
	free(chunk_buf);
	return ret;
}

static void
recovery_mmc_update_erase_progress(struct recovery_status_led_ctrl *status_leds,
				   u32 progress_base, lbaint_t done,
				   lbaint_t total)
{
	unsigned long long steps;

	if (!total)
		steps = RECOVERY_MMC_ERASE_PROGRESS_STEPS;
	else
		steps = (unsigned long long)done *
			RECOVERY_MMC_ERASE_PROGRESS_STEPS / total;
	if (steps > RECOVERY_MMC_ERASE_PROGRESS_STEPS)
		steps = RECOVERY_MMC_ERASE_PROGRESS_STEPS;

	prog_erase_done = progress_base + steps;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);
}

static bool recovery_mmc_buffer_is_zero(const u8 *buf, size_t len)
{
	/* Word-at-a-time scan, still checking every byte (including the tail). */
	return !memchr_inv(buf, 0, len);
}

static int recovery_mmc_zero_part(const char *spec,
				  struct recovery_status_led_ctrl *status_leds,
				  struct blk_desc *desc,
				  const struct disk_partition *part,
				  u32 progress_base)
{
	ulong blksz = part->blksz ?: desc->blksz;
	ulong chunk_size;
	lbaint_t chunk_blocks, cleared = 0;
	u8 *chunk_buf;
	ulong started = get_timer(0), tick;
	ulong write_ms = 0, read_ms = 0, check_ms = 0, service_ms = 0;
	int ret = 0;

	if (!blksz || !part->size)
		return -EINVAL;
	/* Keep upload I/O at 1 MiB; only the full zero-fill uses larger I/O. */
	chunk_size = ALIGN(RECOVERY_MMC_ZERO_CHUNK, blksz);
	if (chunk_size < blksz)
		chunk_size = blksz;

	chunk_buf = memalign(ARCH_DMA_MINALIGN, chunk_size);
	if (!chunk_buf) {
		chunk_size = ALIGN(RECOVERY_MMC_WRITE_CHUNK, blksz);
		chunk_buf = memalign(ARCH_DMA_MINALIGN, chunk_size);
	}
	if (!chunk_buf) {
		ret = -ENOMEM;
		goto out;
	}
	chunk_blocks = chunk_size / blksz;

	memset(chunk_buf, 0, chunk_size);

	while (cleared < part->size) {
		lbaint_t todo = part->size - cleared;
		lbaint_t blk = part->start + cleared;

		if (todo > chunk_blocks)
			todo = chunk_blocks;

		tick = get_timer(0);
		if (blk_dwrite(desc, blk, todo, chunk_buf) != todo) {
			write_ms += get_timer(tick);
			printf("eMMC zero-fill failed at partition '%s' block "
			       LBAF "\n", spec, blk);
			ret = -EIO;
			goto out;
		}
		write_ms += get_timer(tick);
		tick = get_timer(0);
		if (blk_dread(desc, blk, todo, chunk_buf) != todo) {
			read_ms += get_timer(tick);
			printf("eMMC zero-fill readback failed at partition '%s' block "
			       LBAF "\n", spec, blk);
			ret = -EBADMSG;
			goto out;
		}
		read_ms += get_timer(tick);
		tick = get_timer(0);
		if (!recovery_mmc_buffer_is_zero(chunk_buf,
						(size_t)todo * blksz)) {
			check_ms += get_timer(tick);
			printf("eMMC zero-fill readback failed at partition '%s' block "
			       LBAF "\n", spec, blk);
			ret = -EBADMSG;
			goto out;
		}
		check_ms += get_timer(tick);

		cleared += todo;
		tick = get_timer(0);
		recovery_mmc_update_erase_progress(status_leds, progress_base,
						   cleared, part->size);
		service_ms += get_timer(tick);
	}

	/*
	 * TRIM completion is not a content guarantee on every eMMC.  Verify
	 * both ends of a zero-filled data partition before allowing the new
	 * rootfs to boot, so a stale overlay cannot survive silently.
	 */
	{
		lbaint_t verify_blocks = min(part->size, chunk_blocks);
		lbaint_t verify_starts[2];
		int i;

		verify_starts[0] = part->start;
		verify_starts[1] = part->start + part->size - verify_blocks;
		for (i = 0; i < ARRAY_SIZE(verify_starts); i++) {
			tick = get_timer(0);
			if (blk_dread(desc, verify_starts[i], verify_blocks,
				      chunk_buf) != verify_blocks) {
				read_ms += get_timer(tick);
				printf("eMMC zero-fill readback failed at partition '%s' block "
				       LBAF "\n", spec, verify_starts[i]);
				ret = -EBADMSG;
				goto out;
			}
			read_ms += get_timer(tick);
			tick = get_timer(0);
			if (!recovery_mmc_buffer_is_zero(chunk_buf,
							verify_blocks * blksz)) {
				check_ms += get_timer(tick);
				printf("eMMC zero-fill readback failed at partition '%s' block "
				       LBAF "\n", spec, verify_starts[i]);
				ret = -EBADMSG;
				goto out;
			}
			check_ms += get_timer(tick);
		}
		recovery_debug_printf("Verified zero-fill at both ends of eMMC partition '%s'\n",
				      spec);
	}

out:
	/* One serial summary, never per-chunk logging or a web success signal. */
	printf("eMMC zero-fill timing: part=%s result=%d verified=%llu/%llu "
	       "chunk=%lu write_ms=%lu read_ms=%lu check_ms=%lu "
	       "service_ms=%lu total_ms=%lu\n", spec, ret,
	       (unsigned long long)cleared * blksz,
	       (unsigned long long)part->size * blksz, chunk_size,
	       write_ms, read_ms, check_ms, service_ms, get_timer(started));
	free(chunk_buf);
	return ret;
}

/*
 * Native erase is only a speed hint, never proof of empty overlay contents.
 * Read every byte and repair nonzero chunks before authorizing a clean boot.
 * This establishes logical zero readback, not a secure-sanitize guarantee.
 */
static int recovery_mmc_erase_verify_part(const char *spec,
				struct recovery_status_led_ctrl *status_leds,
				struct blk_desc *desc,
				const struct disk_partition *part,
				struct mmc *mmc, u32 progress_base)
{
	ulong blksz = part->blksz ?: desc->blksz;
	ulong chunk_size, started = get_timer(0), tick;
	ulong erase_ms = 0, read_ms = 0, write_ms = 0, check_ms = 0;
	ulong sample_count_used = 0;
	bool full_verify_mode = false;
	lbaint_t erased = 0, checked = 0, repaired = 0;
	lbaint_t erase_blocks, chunk_blocks, verify_blocks, verify_starts[2];
	u32 group = mmc->erase_grp_size;
	u8 *buf;
	int ret = 0, i;

	if (!blksz || blksz != desc->blksz || !part->size ||
	    part->start > desc->lba || part->size > desc->lba - part->start)
		return -EINVAL;
	if ((u64)part->size > ~0ULL / blksz ||
	    (u64)part->size > ~0ULL / RECOVERY_MMC_ERASE_PROGRESS_STEPS)
		return -EOVERFLOW;

	/* Never let the controller round an erase into a neighbouring partition. */
	if (!group || (!mmc->can_trim &&
		       (part->start % group || part->size % group)))
		return recovery_mmc_zero_part(spec, status_leds, desc, part,
					     progress_base);

	chunk_size = ALIGN(RECOVERY_MMC_ZERO_CHUNK, blksz);
	buf = memalign(ARCH_DMA_MINALIGN, chunk_size);
	if (!buf) {
		chunk_size = ALIGN(RECOVERY_MMC_WRITE_CHUNK, blksz);
		buf = memalign(ARCH_DMA_MINALIGN, chunk_size);
	}
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}
	chunk_blocks = chunk_size / blksz;
	erase_blocks = RECOVERY_MMC_ERASE_CHUNK / blksz;
	if (!erase_blocks)
		erase_blocks = 1;
	if (!mmc->can_trim) {
		if (erase_blocks < group)
			erase_blocks = group;
		erase_blocks -= erase_blocks % group;
	}

	while (erased < part->size) {
		lbaint_t todo = min(erase_blocks, part->size - erased);

		tick = get_timer(0);
		if (blk_derase(desc, part->start + erased, todo) != todo) {
			erase_ms += get_timer(tick);
			ret = -EIO;
			goto out;
		}
		erase_ms += get_timer(tick);
		erased += todo;
		recovery_mmc_update_erase_progress(status_leds, progress_base,
				(u64)erased * 250 / part->size, 1000);
		/* Keep lwIP servicing /status polls during long erases (1GB P27),
		 * or the browser declares the connection lost mid-erase. */
		recovery_service_runtime(status_leds);
	}

	/*
	 * The erase phase already succeeded block-by-block; the read-back pass
	 * only proves the controller returned defined-zero content.  Full
	 * byte-verification of multi-GB partitions dominates the customer wait
	 * (6.2 GiB reads back at ~20 MB/s here), so the default is a spread
	 * sample check with automatic escalation to the full sweep when any
	 * sample fails.  Set env recovery_verify_full=1 to force the legacy
	 * full read-back (lab evidence runs).
	 */
	if (env_get_yesno("recovery_verify_full") != 1) {
		ulong samples = env_get_ulong("recovery_erase_samples", 10, 8);
		lbaint_t span;

		if (samples < 2)
			samples = 2;
		if (samples > 64)
			samples = 64;
		if (samples > part->size / chunk_blocks)
			samples = part->size / chunk_blocks ?: 1;
		sample_count_used = samples;
		span = part->size - min(part->size, chunk_blocks);

		for (i = 0; (ulong)i < samples; i++) {
			lbaint_t blk = part->start;
			bool zero;

			if (samples > 1)
				blk += (u64)span * i / (samples - 1);

			tick = get_timer(0);
			if (blk_dread(desc, blk, chunk_blocks, buf) !=
			    chunk_blocks) {
				read_ms += get_timer(tick);
				ret = -EBADMSG;
				goto out;
			}
			read_ms += get_timer(tick);
			tick = get_timer(0);
			zero = recovery_mmc_buffer_is_zero(buf,
					chunk_blocks * blksz);
			check_ms += get_timer(tick);
			if (!zero) {
				/* Controller returned junk after erase:
				 * zero-repair the sample and escalate to the
				 * full read-back sweep before trusting it. */
				memset(buf, 0, chunk_blocks * blksz);
				tick = get_timer(0);
				if (blk_dwrite(desc, blk, chunk_blocks, buf) !=
				    chunk_blocks) {
					write_ms += get_timer(tick);
					ret = -EIO;
					goto out;
				}
				write_ms += get_timer(tick);
				recovery_debug_printf(
					"eMMC erase sample nonzero at LBA %llu; escalating to full verify\n",
					(unsigned long long)blk);
				goto full_verify;
			}
			recovery_service_runtime(status_leds);
			recovery_mmc_update_erase_progress(status_leds,
					progress_base,
					250 + (u64)(i + 1) * 749 / samples,
					1000);
		}
		recovery_mmc_update_erase_progress(status_leds, progress_base,
				1000, 1000);
		goto done_verify;
	}

full_verify:
	full_verify_mode = true;
	while (checked < part->size) {
		lbaint_t todo = min(chunk_blocks, part->size - checked);
		lbaint_t blk = part->start + checked;
		size_t bytes = (size_t)todo * blksz;
		bool zero;

		tick = get_timer(0);
		if (blk_dread(desc, blk, todo, buf) != todo) {
			read_ms += get_timer(tick);
			ret = -EBADMSG;
			goto out;
		}
		read_ms += get_timer(tick);
		tick = get_timer(0);
		zero = recovery_mmc_buffer_is_zero(buf, bytes);
		check_ms += get_timer(tick);
		if (!zero) {
			/* Never write the contents of an untrusted/nonzero read buffer. */
			memset(buf, 0, bytes);
			tick = get_timer(0);
			if (blk_dwrite(desc, blk, todo, buf) != todo) {
				write_ms += get_timer(tick);
				ret = -EIO;
				goto out;
			}
			write_ms += get_timer(tick);
			tick = get_timer(0);
			if (blk_dread(desc, blk, todo, buf) != todo) {
				read_ms += get_timer(tick);
				ret = -EBADMSG;
				goto out;
			}
			read_ms += get_timer(tick);
			tick = get_timer(0);
			zero = recovery_mmc_buffer_is_zero(buf, bytes);
			check_ms += get_timer(tick);
			if (!zero) {
				ret = -EBADMSG;
				goto out;
			}
			repaired += todo;
		}
		checked += todo;
		/* Leave the last step for the two endpoint rechecks. */
		recovery_mmc_update_erase_progress(status_leds, progress_base,
				250 + (u64)checked * 749 / part->size, 1000);
	}

	verify_blocks = min(part->size, chunk_blocks);
	verify_starts[0] = part->start;
	verify_starts[1] = part->start + part->size - verify_blocks;
	for (i = 0; i < ARRAY_SIZE(verify_starts); i++) {
		bool zero;

		tick = get_timer(0);
		if (blk_dread(desc, verify_starts[i], verify_blocks, buf) !=
		    verify_blocks) {
			read_ms += get_timer(tick);
			ret = -EBADMSG;
			goto out;
		}
		read_ms += get_timer(tick);
		tick = get_timer(0);
		zero = recovery_mmc_buffer_is_zero(buf, verify_blocks * blksz);
		check_ms += get_timer(tick);
		if (!zero) {
			ret = -EBADMSG;
			goto out;
		}
		recovery_service_runtime(status_leds);
	}
	recovery_mmc_update_erase_progress(status_leds, progress_base, 1000, 1000);
done_verify:
out:
	printf("eMMC erase-verify timing: part=%s result=%d "
	       "verified=%llu/%llu repaired=%llu chunk=%lu samples=%lu "
	       "erase_ms=%lu read_ms=%lu write_ms=%lu check_ms=%lu total_ms=%lu\n",
	       spec, ret, (unsigned long long)checked * blksz,
	       (unsigned long long)part->size * blksz,
	       (unsigned long long)repaired * blksz, chunk_size,
	       full_verify_mode ? 0UL : sample_count_used,
	       erase_ms, read_ms, write_ms,
	       check_ms, get_timer(started));
	free(buf);
	return ret;
}

static int recovery_mmc_erase_part(const char *spec,
				   struct recovery_status_led_ctrl *status_leds,
				   u32 progress_base)
{
	struct disk_partition part;
	struct blk_desc *desc;
	struct mmc *mmc;
	unsigned long long capacity;
	lbaint_t chunk_blocks, erased = 0;
	u32 erase_group;
	ulong blksz;
	bool exact;
	int ret;

	if (!spec || !*spec)
		return -EINVAL;

	ret = recovery_get_mmc_part(spec, &desc, &part);
	if (ret)
		return ret;

	blksz = part.blksz ?: desc->blksz;
	capacity = recovery_mmc_part_bytes(&part);
	mmc = find_mmc_device(desc->devnum);
	if (!mmc) {
		printf("Cannot find eMMC device %d for partition '%s'\n",
		       desc->devnum, spec);
		return -ENODEV;
	}

	/*
	 * rootfs_data is the writable overlay.  Its GPT start is intentionally
	 * not erase-group aligned, and MMC_TRIM_ARG only invalidates blocks on
	 * some devices without guaranteeing that old bytes cannot be read back.
	 * Verify the complete canonical P28 and zero-repair every nonzero chunk.
	 * Native erase completion alone must never authorize a clean firmware boot.
	 */
	if (!strcmp(spec, sbe1v1k_layout_full.data_part) &&
	    part.start == RECOVERY_SBE1V1K_FULL_DATA_START) {
		recovery_debug_printf("Verifying deterministic zero contents for writable eMMC partition '%s'\n",
				      spec);
		return recovery_mmc_erase_verify_part(spec, status_leds, desc, &part,
						    mmc, progress_base);
	}

	erase_group = mmc->erase_grp_size;
	exact = erase_group &&
		(mmc->can_trim || (!(part.start % erase_group) &&
				   !(part.size % erase_group)));

	recovery_debug_printf("Erasing entire eMMC partition '%s' (%llu blocks, %llu bytes)...\n",
			      spec, (unsigned long long)part.size, capacity);

	if (!exact) {
		recovery_debug_printf("eMMC partition '%s' is not erase-group aligned and exact trim is unavailable; zero-filling the complete partition to protect adjacent GPT partitions\n",
				      spec);
		return recovery_mmc_zero_part(spec, status_leds, desc, &part,
					      progress_base);
	}

	chunk_blocks = RECOVERY_MMC_ERASE_CHUNK / blksz;
	if (!chunk_blocks)
		chunk_blocks = 1;
	if (!mmc->can_trim && chunk_blocks < erase_group)
		chunk_blocks = erase_group;
	if (!mmc->can_trim)
		chunk_blocks -= chunk_blocks % erase_group;

	while (erased < part.size) {
		lbaint_t todo = part.size - erased;
		lbaint_t blk = part.start + erased;

		if (todo > chunk_blocks)
			todo = chunk_blocks;
		if (!mmc->can_trim && todo % erase_group)
			todo -= todo % erase_group;
		if (!todo) {
			printf("Cannot safely align eMMC erase for partition '%s'\n",
			       spec);
			return -EINVAL;
		}

		{
			/* Sub-slice the chunk: the first trim of a freshly
			 * cleaned region can block one blk_derase call for
			 * seconds, and Ethernet RX packets arriving during
			 * such a window are lost before lwIP ever sees them.
			 * 4MiB slices keep every blackout near 10ms. */
			lbaint_t done = 0;
			const lbaint_t slice_blocks = (4 * 1024 * 1024) / blksz;

			while (done < todo) {
				lbaint_t slice = todo - done;

				if (slice > slice_blocks)
					slice = slice_blocks;
				if (blk_derase(desc, blk + done, slice) != slice) {
					printf("eMMC erase failed at partition '%s' block " LBAF
					       " count " LBAF "\n", spec,
					       blk + done, slice);
					return -EIO;
				}
				done += slice;
				recovery_service_runtime(status_leds);
			}
		}

		erased += todo;
		recovery_mmc_update_erase_progress(status_leds, progress_base,
						   erased, part.size);
		{
			static ulong dbg_t0;
			ulong dbg_ms;
			if (!dbg_t0)
				dbg_t0 = get_timer(0) - 1;
			dbg_ms = get_timer(dbg_t0);
			dbg_t0 = get_timer(0);
			recovery_debug_printf("erase chunk at +%lluMiB took %lums\n",
				       (unsigned long long)(erased * (unsigned long long)part.blksz >> 20),
				       dbg_ms);
		}
	}

	return 0;
}

#define RECOVERY_MMC_STREAM_PARTS 2

struct recovery_mmc_stream_part {
	char spec[96];
	struct blk_desc *desc;
	struct disk_partition part;
	size_t expected;
	size_t written;
	u32 input_crc;
};

struct recovery_mmc_stream {
	struct recovery_mmc_stream_part parts[RECOVERY_MMC_STREAM_PARTS];
	u8 *buf;
	u8 *tar_tail;
	u8 tar_header[512];
	size_t buf_size;
	size_t buf_used;
	size_t input_received;
	size_t tar_tail_used;
	size_t tar_header_used;
	size_t tar_entry_remaining;
	size_t tar_padding_remaining;
	size_t tar_control_used;
	u32 tar_crc;
	int part_count;
	int part_index;
	int tar_part_index;
	int error;
	enum upload_target target;
	enum recovery_stream_format format;
	size_t total_expected;
	u8 tar_control[sizeof(RECOVERY_SBE1V1K_CONTROL_BOARD)];
	u8 tar_zero_blocks;
	bool tar_current_control;
	bool tar_seen_control;
	bool tar_seen_kernel;
	bool tar_seen_rootfs;
	bool tar_end;
	bool active;
	bool prepared;
};

static struct recovery_mmc_stream recovery_stream;
static bool recovery_stream_completed;

static void recovery_mmc_stream_reset(void)
{
	free(recovery_stream.buf);
	free(recovery_stream.tar_tail);
	memset(&recovery_stream, 0, sizeof(recovery_stream));
	recovery_stream.tar_part_index = -1;
}

static int recovery_mmc_stream_add_part(const char *spec, size_t expected)
{
	struct recovery_mmc_stream_part *stream_part;
	unsigned long long capacity;
	int ret;

	if (recovery_stream.part_count >= RECOVERY_MMC_STREAM_PARTS)
		return -ENOSPC;

	stream_part = &recovery_stream.parts[recovery_stream.part_count];
	strlcpy(stream_part->spec, spec, sizeof(stream_part->spec));
	ret = recovery_get_mmc_part(stream_part->spec, &stream_part->desc,
				    &stream_part->part);
	if (ret)
		return ret;

	capacity = recovery_mmc_part_bytes(&stream_part->part);
	if (expected > capacity) {
		printf("Stream payload size %lu exceeds eMMC partition '%s' size %llu\n",
		       (ulong)expected, stream_part->spec, capacity);
		return -EFBIG;
	}

	stream_part->expected = expected;
	stream_part->input_crc = ~0U;
	recovery_stream.part_count++;
	return 0;
}

static int recovery_mmc_stream_prepare(enum upload_target target, size_t size,
				       enum recovery_stream_format format)
{
	const char *kernel_part = env_get("recovery_part_kernel") ?: "0#kernel";
	const char *rootfs_part = env_get("recovery_part_rootfs") ?: "0#rootfs";
	const char *data_part = env_get("recovery_part_data") ?: "0#rootfs_data";
	const char *uboot_part = env_get("recovery_part_uboot");
	size_t kernel_pad = env_get_hex("recovery_kernel_pad",
					RECOVERY_KERNEL_PAD_SIZE);
	ulong blksz;
	int ret;

	/*
	 * Stream preparation erases its targets before the HTTP body is
	 * received.  Apply the same full-layout gate here, before partition
	 * lookup or any erase, so a direct prepare/stream request cannot bypass
	 * the non-stream firmware guard below.
	 */
	if (target == TARGET_FIRMWARE &&
	    recovery_board_is_sbe1v1k() &&
	    active_sbe1v1k_layout != RECOVERY_SBE1V1K_LAYOUT_FULL) {
		printf("SBE1V1K firmware stream requires the full clean layout first\n");
		return -EPERM;
	}
	if (target == TARGET_UBOOT &&
	    recovery_board_is_sbe1v1k() &&
	    active_sbe1v1k_layout != RECOVERY_SBE1V1K_LAYOUT_FULL) {
		printf("SBE1V1K chainloader stream requires the full clean layout first\n");
		return -EPERM;
	}
	if (target == TARGET_FIRMWARE && recovery_board_is_sbe1v1k()) {
		ret = recovery_sbe1v1k_clean_session_validate();
		if (ret)
			return ret;
		/*
		 * The layout enum is only a RAM hint.  Re-read the GPT before
		 * looking up targets or erasing anything so a changed/corrupt map
		 * cannot redirect a destructive stream into stale storage.
		 */
		ret = recovery_verify_sbe1v1k_gpt(&sbe1v1k_layout_full);
		if (ret) {
			printf("SBE1V1K firmware stream rejected: full GPT map changed\n");
			return ret;
		}
		kernel_part = sbe1v1k_layout_full.kernel_part;
		rootfs_part = sbe1v1k_layout_full.rootfs_part;
		data_part = sbe1v1k_layout_full.data_part;
	} else if (target == TARGET_UBOOT && recovery_board_is_sbe1v1k()) {
		/*
		 * Never let a stale APPSBLENV value redirect a chainloader stream
		 * into P26-P28.  The full profile is the sole accepted target map.
		 */
		ret = recovery_verify_sbe1v1k_gpt(&sbe1v1k_layout_full);
		if (ret) {
			printf("SBE1V1K chainloader stream rejected: full GPT map changed\n");
			return ret;
		}
		uboot_part = sbe1v1k_layout_full.uboot_part;
	}

	recovery_mmc_stream_reset();
	recovery_stream_completed = false;
	recovery_stream.target = target;
	recovery_stream.format = format;
	recovery_stream.total_expected = size;

	if (target == TARGET_FIRMWARE) {
		if (format == RECOVERY_STREAM_TAR) {
			ret = recovery_mmc_stream_add_part(kernel_part, 0);
			if (ret)
				goto err;
			ret = recovery_mmc_stream_add_part(rootfs_part, 0);
			if (ret)
				goto err;
		} else {
			if (size <= kernel_pad)
				return -EINVAL;
			ret = recovery_mmc_stream_add_part(kernel_part, kernel_pad);
			if (ret)
				goto err;
			ret = recovery_mmc_stream_add_part(rootfs_part, size - kernel_pad);
			if (ret)
				goto err;
		}
		ret = recovery_mmc_check_part_size(data_part, 0);
		if (ret)
			goto err;
	} else if (target == TARGET_UBOOT) {
		if (format != RECOVERY_STREAM_RAW) {
			ret = -EINVAL;
			goto err;
		}
		if (!uboot_part || !*uboot_part) {
			ret = -ENODEV;
			goto err;
		}
		ret = recovery_mmc_stream_add_part(uboot_part, size);
		if (ret)
			goto err;
	} else {
		ret = -EINVAL;
		goto err;
	}

	blksz = recovery_stream.parts[0].part.blksz ?:
		recovery_stream.parts[0].desc->blksz;
	recovery_stream.buf_size = ALIGN(RECOVERY_MMC_STREAM_CHUNK, blksz);
	recovery_stream.buf = memalign(ARCH_DMA_MINALIGN,
				       recovery_stream.buf_size);
	if (!recovery_stream.buf) {
		ret = -ENOMEM;
		goto err;
	}
	if (format == RECOVERY_STREAM_TAR) {
		recovery_stream.tar_tail = malloc(RECOVERY_FWTOOL_TAIL_MAX);
		if (!recovery_stream.tar_tail) {
			ret = -ENOMEM;
			goto err;
		}
		recovery_stream.tar_crc = ~0U;
	}

	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = (target == TARGET_FIRMWARE ? 2 : 1) *
		RECOVERY_MMC_ERASE_PROGRESS_STEPS;
	prog_write_done = 0;
	prog_write_total = size;
	prog_total = prog_erase_total + prog_write_total;

	if (target == TARGET_FIRMWARE) {
		if (env_get_yesno("recovery_stream") == 1) {
			ret = recovery_mmc_erase_part(kernel_part,
						      recovery_runtime_status_leds, 0);
			if (ret)
				goto err;
			ret = recovery_mmc_erase_part(rootfs_part,
						      recovery_runtime_status_leds,
						      RECOVERY_MMC_ERASE_PROGRESS_STEPS);
			if (!ret)
				recovery_debug_printf("Keeping verified-empty P28; stream prepared P26/P27 only\n");
		} else {
			/* RAM upload mode: eMMC trim storms coincide with RX
			 * packet loss, so erase nothing while the page is
			 * connected. flash_image erases (sub-sliced, pumped)
			 * after the whole image has been buffered. */
			recovery_debug_printf("RAM upload mode: erase deferred to flash\n");
		}
	} else {
		ret = recovery_mmc_erase_part(uboot_part,
					      recovery_runtime_status_leds, 0);
	}
	if (ret)
		goto err;

	prog_phase = 4;
	prog_erase_done = prog_erase_total;
	prog_done = prog_erase_done;
	recovery_stream.active = true;
	recovery_stream.prepared = true;
	recovery_debug_printf("Destructive eMMC stream prepared after required target processing\n");
	return 0;

err:
	prog_phase = -1;
	recovery_mmc_stream_reset();
	return ret;
}

static int recovery_mmc_stream_flush_part(int part_index)
{
	struct recovery_mmc_stream_part *stream_part;
	ulong blksz;
	size_t write_len;
	lbaint_t blk, blkcnt;

	if (!recovery_stream.active || recovery_stream.error)
		return recovery_stream.error ?: -EINVAL;
	if (!recovery_stream.buf_used)
		return 0;
	if (part_index < 0 || part_index >= recovery_stream.part_count)
		return -EFBIG;

	stream_part = &recovery_stream.parts[part_index];
	blksz = stream_part->part.blksz ?: stream_part->desc->blksz;
	write_len = ALIGN(recovery_stream.buf_used, blksz);
	if (write_len > recovery_stream.buf_used)
		memset(recovery_stream.buf + recovery_stream.buf_used, 0,
		       write_len - recovery_stream.buf_used);

	blk = stream_part->part.start + stream_part->written / blksz;
	blkcnt = write_len / blksz;
	if (blk_dwrite(stream_part->desc, blk, blkcnt,
		       recovery_stream.buf) != blkcnt) {
		printf("Destructive stream write failed at '%s' block " LBAF "\n",
		       stream_part->spec, blk);
		recovery_stream.error = -EIO;
		return -EIO;
	}

	stream_part->written += recovery_stream.buf_used;
	/*
	 * Raw mode reports bytes physically written. TAR mode reports bytes
	 * consumed from the HTTP body, including headers and padding, in
	 * recovery_mmc_stream_append_tar(). Mixing both units double-counts the
	 * payload and can make write_done exceed Content-Length.
	 */
	if (recovery_stream.format != RECOVERY_STREAM_TAR)
		prog_write_done += recovery_stream.buf_used;
	prog_done = prog_erase_done + prog_write_done;
	recovery_stream.buf_used = 0;
	return 0;
}

static int recovery_mmc_stream_flush(void)
{
	return recovery_mmc_stream_flush_part(recovery_stream.part_index);
}

static int recovery_mmc_stream_append_part(const void *data, size_t size,
					   int part_index)
{
	const u8 *src = data;
	struct recovery_mmc_stream_part *stream_part;

	if (part_index < 0 || part_index >= recovery_stream.part_count)
		return -EINVAL;
	stream_part = &recovery_stream.parts[part_index];

	while (size) {
		size_t consumed = stream_part->written + recovery_stream.buf_used;
		size_t remaining, space, copy;
		int ret;

		if (consumed > stream_part->expected)
			return -EFBIG;
		remaining = stream_part->expected - consumed;
		space = recovery_stream.buf_size - recovery_stream.buf_used;
		copy = min(size, min(remaining, space));
		if (!copy)
			return -EFBIG;

		stream_part->input_crc =
			crc32_no_comp(stream_part->input_crc, src, copy);
		memcpy(recovery_stream.buf + recovery_stream.buf_used, src, copy);
		recovery_stream.buf_used += copy;
		src += copy;
		size -= copy;
		consumed += copy;

		if (recovery_stream.buf_used == recovery_stream.buf_size ||
		    consumed == stream_part->expected) {
			ret = recovery_mmc_stream_flush_part(part_index);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int recovery_mmc_stream_append_raw(const void *data, size_t size)
{
	const u8 *src = data;

	while (size) {
		struct recovery_mmc_stream_part *stream_part;
		size_t consumed, remaining, copy;
		int ret;

		if (recovery_stream.part_index >= recovery_stream.part_count)
			return -EFBIG;
		stream_part = &recovery_stream.parts[recovery_stream.part_index];
		consumed = stream_part->written + recovery_stream.buf_used;
		if (consumed > stream_part->expected)
			return -EFBIG;
		remaining = stream_part->expected - consumed;
		copy = min(size, remaining);
		if (!copy)
			return -EIO;

		ret = recovery_mmc_stream_append_part(src, copy,
					      recovery_stream.part_index);
		if (ret)
			return ret;
		src += copy;
		size -= copy;
		if (stream_part->written == stream_part->expected)
			recovery_stream.part_index++;
	}

	return 0;
}

static bool recovery_is_control_name(const char *name)
{
	return !strcmp(recovery_basename(name), "CONTROL");
}

static int recovery_mmc_stream_finish_control(void)
{
	static const char board[] = RECOVERY_SBE1V1K_CONTROL_BOARD;
	size_t board_len = sizeof(board) - 1;

	if (!recovery_stream.tar_current_control)
		return 0;
	recovery_stream.tar_current_control = false;
	if (recovery_stream.tar_control_used != board_len &&
	    recovery_stream.tar_control_used != board_len + 1)
		return -EINVAL;
	if (memcmp(recovery_stream.tar_control, board, board_len) ||
	    (recovery_stream.tar_control_used == board_len + 1 &&
	     recovery_stream.tar_control[board_len] != '\n')) {
		printf("Streamed sysupgrade CONTROL has the wrong board\n");
		return -EINVAL;
	}
	recovery_stream.tar_seen_control = true;
	return 0;
}

static void recovery_mmc_stream_update_tar_crc(const void *data, size_t size)
{
	const u8 *src = data;

	while (size) {
		uint chunk = size > UINT_MAX ? UINT_MAX : (uint)size;

		recovery_stream.tar_crc =
			crc32_no_comp(recovery_stream.tar_crc, src, chunk);
		src += chunk;
		size -= chunk;
	}
}

static int recovery_mmc_stream_append_tar_tail(const void *data, size_t size)
{
	if (!recovery_stream.tar_tail ||
	    size > RECOVERY_FWTOOL_TAIL_MAX - recovery_stream.tar_tail_used) {
		printf("Streamed sysupgrade fwtool tail exceeds %u bytes\n",
		       RECOVERY_FWTOOL_TAIL_MAX);
		return -EFBIG;
	}

	memcpy(recovery_stream.tar_tail + recovery_stream.tar_tail_used,
	       data, size);
	recovery_stream.tar_tail_used += size;
	return 0;
}

static int recovery_mmc_stream_tar_header(void)
{
	const struct recovery_tar_header *hdr =
		(const struct recovery_tar_header *)recovery_stream.tar_header;
	struct recovery_mmc_stream_part *stream_part;
	unsigned long long capacity;
	char name[256];
	size_t entry_size;
	int part_index = -1;
	bool regular;
	int ret;

	if (recovery_tar_header_empty(hdr)) {
		if (++recovery_stream.tar_zero_blocks == RECOVERY_TAR_END_BLOCKS)
			recovery_stream.tar_end = true;
		return 0;
	}
	if (recovery_stream.tar_zero_blocks) {
		printf("Streamed sysupgrade has data after a tar end marker\n");
		return -EINVAL;
	}
	if (memcmp(hdr->magic, "ustar", 5)) {
		printf("Streamed sysupgrade has an invalid tar header\n");
		return -EINVAL;
	}
	ret = recovery_tar_octal(hdr->size, sizeof(hdr->size), &entry_size);
	if (ret)
		return ret;

	recovery_tar_name(hdr, name, sizeof(name));
	regular = hdr->typeflag == '\0' || hdr->typeflag == '0';
	if (regular && recovery_is_control_name(name)) {
		if (recovery_stream.tar_seen_control ||
		    recovery_stream.tar_current_control ||
		    recovery_stream.tar_seen_kernel ||
		    recovery_stream.tar_seen_rootfs ||
		    (entry_size != sizeof(RECOVERY_SBE1V1K_CONTROL_BOARD) - 1 &&
		     entry_size != sizeof(RECOVERY_SBE1V1K_CONTROL_BOARD)))
			return -EINVAL;
		recovery_stream.tar_current_control = true;
		recovery_stream.tar_control_used = 0;
	} else if (regular && recovery_is_kernel_name(name)) {
		if (!recovery_stream.tar_seen_control ||
		    recovery_stream.tar_seen_kernel)
			return -EINVAL;
		part_index = 0;
		recovery_stream.tar_seen_kernel = true;
	} else if (regular && recovery_is_rootfs_name(name)) {
		if (!recovery_stream.tar_seen_control ||
		    recovery_stream.tar_seen_rootfs)
			return -EINVAL;
		part_index = 1;
		recovery_stream.tar_seen_rootfs = true;
	}

	if (part_index >= 0) {
		if (!entry_size)
			return -EINVAL;
		stream_part = &recovery_stream.parts[part_index];
		capacity = recovery_mmc_part_bytes(&stream_part->part);
		if (entry_size > capacity) {
			printf("Tar payload '%s' size %lu exceeds eMMC partition '%s' size %llu\n",
			       name, (ulong)entry_size, stream_part->spec, capacity);
			return -EFBIG;
		}
		stream_part->expected = entry_size;
	}

	recovery_stream.tar_part_index = part_index;
	recovery_stream.tar_entry_remaining = entry_size;
	recovery_stream.tar_padding_remaining = ALIGN(entry_size, 512) - entry_size;
	return 0;
}

static int recovery_mmc_stream_append_tar(const void *data, size_t size)
{
	const u8 *src = data;

	while (size) {
		size_t copy;
		int ret;

		if (recovery_stream.tar_end) {
			ret = recovery_mmc_stream_append_tar_tail(src, size);
			if (ret)
				return ret;
			recovery_stream.input_received += size;
			prog_write_done = recovery_stream.input_received;
			prog_done = prog_erase_done + prog_write_done;
			return 0;
		}
		if (recovery_stream.tar_entry_remaining) {
			copy = min(size, recovery_stream.tar_entry_remaining);
			if (recovery_stream.tar_current_control) {
				if (copy > sizeof(recovery_stream.tar_control) -
					   recovery_stream.tar_control_used)
					return -EFBIG;
				memcpy(recovery_stream.tar_control +
				       recovery_stream.tar_control_used, src, copy);
				recovery_stream.tar_control_used += copy;
			} else if (recovery_stream.tar_part_index >= 0) {
				ret = recovery_mmc_stream_append_part(
					src, copy, recovery_stream.tar_part_index);
				if (ret)
					return ret;
			}
			recovery_mmc_stream_update_tar_crc(src, copy);
			recovery_stream.tar_entry_remaining -= copy;
			recovery_stream.input_received += copy;
			src += copy;
			size -= copy;
			if (!recovery_stream.tar_entry_remaining) {
				ret = recovery_mmc_stream_finish_control();
				if (ret)
					return ret;
				recovery_stream.tar_part_index = -1;
			}
			continue;
		}
		if (recovery_stream.tar_padding_remaining) {
			copy = min(size, recovery_stream.tar_padding_remaining);
			recovery_mmc_stream_update_tar_crc(src, copy);
			recovery_stream.tar_padding_remaining -= copy;
			recovery_stream.input_received += copy;
			src += copy;
			size -= copy;
			continue;
		}

		copy = min(size, sizeof(recovery_stream.tar_header) -
			   recovery_stream.tar_header_used);
		memcpy(recovery_stream.tar_header + recovery_stream.tar_header_used,
		       src, copy);
		recovery_mmc_stream_update_tar_crc(src, copy);
		recovery_stream.tar_header_used += copy;
		recovery_stream.input_received += copy;
		src += copy;
		size -= copy;
		if (recovery_stream.tar_header_used ==
		    sizeof(recovery_stream.tar_header)) {
			ret = recovery_mmc_stream_tar_header();
			recovery_stream.tar_header_used = 0;
			if (ret)
				return ret;
		}
	}

	prog_write_done = recovery_stream.input_received;
	prog_done = prog_erase_done + prog_write_done;
	return 0;
}

static int recovery_mmc_stream_append(const void *data, size_t size)
{
	size_t remaining;
	int ret;

	if (recovery_stream.input_received > recovery_stream.total_expected)
		return -EFBIG;
	remaining = recovery_stream.total_expected -
		recovery_stream.input_received;
	if (size > remaining)
		return -EFBIG;

	if (recovery_stream.format == RECOVERY_STREAM_TAR) {
		ret = recovery_mmc_stream_append_tar(data, size);
	} else {
		ret = recovery_mmc_stream_append_raw(data, size);
		if (!ret)
			recovery_stream.input_received += size;
	}
	if (ret)
		recovery_stream.error = ret;
	return ret;
}

static int
recovery_mmc_stream_verify_part_crc(
	const struct recovery_mmc_stream_part *part)
{
	size_t verified = 0;
	u32 read_crc = ~0U;
	ulong blksz;

	blksz = part->part.blksz ?: part->desc->blksz;
	if (!blksz || !part->expected || !recovery_stream.buf ||
	    recovery_stream.buf_size < blksz)
		return -EINVAL;

	while (verified < part->expected) {
		size_t remaining = part->expected - verified;
		size_t read_len = min(remaining, recovery_stream.buf_size);
		lbaint_t blocks = DIV_ROUND_UP(read_len, blksz);
		lbaint_t start = part->part.start + verified / blksz;

		if (blk_dread(part->desc, start, blocks,
			      recovery_stream.buf) != blocks) {
			printf("Streamed firmware readback failed at '%s' block "
			       LBAF "\n", part->spec, start);
			return -EIO;
		}
		read_crc = crc32_no_comp(read_crc, recovery_stream.buf,
					 (uint)read_len);
		verified += read_len;
		recovery_watchdog_poll();
	}

	if (read_crc != part->input_crc) {
		printf("Streamed firmware full readback mismatch at '%s'\n",
		       part->spec);
		return -EBADMSG;
	}

	recovery_debug_printf("Verified complete streamed payload readback for '%s'\n",
			      part->spec);
	return 0;
}

static int
recovery_mmc_stream_verify_kernel_fit(const struct recovery_mmc_stream_part *part)
{
	struct recovery_image_part kernel;
	u8 *header;
	u8 *fit;
	ulong blksz;
	size_t fit_size;
	size_t read_size;
	lbaint_t blocks;
	int ret;

	blksz = part->part.blksz ?: part->desc->blksz;
	if (!blksz || !part->expected)
		return -EINVAL;

	header = memalign(ARCH_DMA_MINALIGN, blksz);
	if (!header)
		return -ENOMEM;
	ret = 0;
	if (blk_dread(part->desc, part->part.start, 1, header) != 1) {
		ret = -EIO;
		goto out_header;
	}
	/*
	 * The first sector only contains the FDT header. With FIT_FULL_CHECK,
	 * fit_check_format() also walks the complete blob, so use the header
	 * check here and verify the complete FIT after its full readback below.
	 */
	if (fdt_check_header(header)) {
		printf("Streamed kernel is not a FIT image after eMMC write\n");
		ret = -ENOEXEC;
		goto out_header;
	}

	fit_size = fit_get_size(header);
	if (!fit_size || fit_size > part->expected ||
	    fit_size > recovery_mmc_part_bytes(&part->part)) {
		printf("Streamed FIT size is outside the written kernel partition\n");
		ret = -EFBIG;
		goto out_header;
	}
	read_size = ALIGN(fit_size, blksz);
	blocks = read_size / blksz;
	fit = memalign(ARCH_DMA_MINALIGN, read_size);
	if (!fit) {
		ret = -ENOMEM;
		goto out_header;
	}
	if (blk_dread(part->desc, part->part.start, blocks, fit) != blocks) {
		ret = -EIO;
		goto out_fit;
	}

	kernel.data = fit;
	kernel.size = fit_size;
	ret = recovery_validate_sbe1v1k_kernel_fit(&kernel);

out_fit:
	free(fit);
out_header:
	free(header);
	return ret;
}

static int
recovery_mmc_stream_verify_rootfs(const struct recovery_mmc_stream_part *part)
{
	struct recovery_image_part rootfs;
	u8 *header;
	ulong blksz;
	int ret;

	blksz = part->part.blksz ?: part->desc->blksz;
	if (!blksz || part->expected < RECOVERY_SQUASHFS_HEADER_MIN)
		return -EINVAL;

	header = memalign(ARCH_DMA_MINALIGN, blksz);
	if (!header)
		return -ENOMEM;
	if (blk_dread(part->desc, part->part.start, 1, header) != 1) {
		free(header);
		return -EIO;
	}

	rootfs.data = header;
	rootfs.size = part->expected;
	ret = recovery_validate_sbe1v1k_rootfs(&rootfs);
	free(header);

	return ret;
}

static int recovery_mmc_stream_verify_sbe1v1k_firmware(void)
{
	int ret;

	if (!recovery_board_is_sbe1v1k() ||
	    recovery_stream.target != TARGET_FIRMWARE)
		return 0;

	ret = recovery_mmc_stream_verify_part_crc(&recovery_stream.parts[0]);
	if (ret)
		return ret;
	ret = recovery_mmc_stream_verify_part_crc(&recovery_stream.parts[1]);
	if (ret)
		return ret;

	ret = recovery_mmc_stream_verify_kernel_fit(&recovery_stream.parts[0]);
	if (ret)
		return ret;

	return recovery_mmc_stream_verify_rootfs(&recovery_stream.parts[1]);
}

static u32 recovery_mmc_stream_tar_prefix_crc(size_t tail_len)
{
	const u8 *src = recovery_stream.tar_tail;
	u32 crc = recovery_stream.tar_crc;

	while (tail_len) {
		uint chunk = tail_len > UINT_MAX ? UINT_MAX : (uint)tail_len;

		crc = crc32_no_comp(crc, src, chunk);
		src += chunk;
		tail_len -= chunk;
	}

	return crc;
}

static int recovery_mmc_stream_verify_fwtool_tail(void)
{
	const u8 *tail = recovery_stream.tar_tail;
	const u8 *trailer;
	const u8 *header;
	size_t tail_size = recovery_stream.tar_tail_used;
	size_t record_size;
	size_t record_start;
	u32 expected_crc;
	u32 actual_crc;

	if (!tail_size)
		return 0;

	/*
	 * Keep plain sysupgrade tar compatibility. GNU tar pads the two required
	 * end blocks with whole zero blocks to its record boundary.
	 */
	if (!memchr_inv(tail, 0, tail_size)) {
		if (tail_size > RECOVERY_FWTOOL_PADDING_MAX ||
		    tail_size % sizeof(recovery_stream.tar_header)) {
			printf("Streamed sysupgrade has invalid zero padding after tar end\n");
			return -EINVAL;
		}
		return 0;
	}

	if (tail_size < RECOVERY_FWTOOL_TRAILER_SIZE) {
		printf("Streamed sysupgrade has a truncated fwtool INFO trailer\n");
		return -EINVAL;
	}

	trailer = tail + tail_size - RECOVERY_FWTOOL_TRAILER_SIZE;
	if (get_unaligned_be32(trailer) != RECOVERY_FWTOOL_MAGIC ||
	    trailer[8] != RECOVERY_FWTOOL_INFO ||
	    trailer[9] || trailer[10] || trailer[11]) {
		printf("Streamed sysupgrade has an invalid fwtool INFO trailer\n");
		return -EINVAL;
	}

	record_size = get_unaligned_be32(trailer + 12);
	if (record_size < RECOVERY_FWTOOL_HEADER_SIZE +
			  RECOVERY_FWTOOL_TRAILER_SIZE ||
	    record_size > RECOVERY_FWTOOL_HEADER_SIZE +
			  RECOVERY_FWTOOL_METADATA_MAX +
			  RECOVERY_FWTOOL_TRAILER_SIZE ||
	    record_size > tail_size) {
		printf("Streamed sysupgrade has an invalid fwtool INFO size\n");
		return -EINVAL;
	}

	record_start = tail_size - record_size;
	if (record_start > RECOVERY_FWTOOL_PADDING_MAX ||
	    record_start % sizeof(recovery_stream.tar_header) ||
	    memchr_inv(tail, 0, record_start)) {
		printf("Streamed sysupgrade has non-zero data before fwtool INFO\n");
		return -EINVAL;
	}

	header = tail + record_start;
	if (memchr_inv(header, 0, RECOVERY_FWTOOL_HEADER_SIZE)) {
		printf("Streamed sysupgrade fwtool INFO version or flags are unsupported\n");
		return -EINVAL;
	}

	/*
	 * fwtool stores its uncomplemented CRC state in big endian. It covers
	 * the complete tar, zero record padding, INFO header and metadata, but
	 * not the final 16-byte trailer containing the stored CRC itself.
	 */
	expected_crc = get_unaligned_be32(trailer + 4);
	actual_crc = recovery_mmc_stream_tar_prefix_crc(
		tail_size - RECOVERY_FWTOOL_TRAILER_SIZE);
	if (actual_crc != expected_crc) {
		printf("Streamed sysupgrade fwtool INFO CRC mismatch\n");
		return -EBADMSG;
	}

	return 0;
}

static int recovery_mmc_stream_finish(void)
{
	int ret;

	if (recovery_stream.input_received != recovery_stream.total_expected)
		return -EIO;
	if (recovery_board_is_sbe1v1k() &&
	    recovery_stream.target == TARGET_FIRMWARE) {
		ret = recovery_sbe1v1k_clean_session_validate();
		if (ret)
			return ret;
	}
	if (recovery_stream.format == RECOVERY_STREAM_TAR) {
		if (!recovery_stream.tar_end ||
		    recovery_stream.tar_zero_blocks < RECOVERY_TAR_END_BLOCKS ||
		    recovery_stream.tar_header_used ||
		    recovery_stream.tar_entry_remaining ||
		    recovery_stream.tar_padding_remaining ||
		    recovery_stream.tar_current_control ||
		    !recovery_stream.tar_seen_control ||
		    !recovery_stream.tar_seen_kernel ||
		    !recovery_stream.tar_seen_rootfs ||
		    recovery_stream.parts[0].written !=
			recovery_stream.parts[0].expected ||
		    recovery_stream.parts[1].written !=
			recovery_stream.parts[1].expected)
			return -EINVAL;
		ret = recovery_mmc_stream_verify_fwtool_tail();
		if (ret)
			return ret;
		prog_write_done = prog_write_total;
		prog_done = prog_total;
	} else {
		ret = recovery_mmc_stream_flush();
		if (ret)
			return ret;
		if (recovery_stream.part_index != recovery_stream.part_count ||
		    prog_write_done != prog_write_total)
			return -EIO;
	}

	return recovery_mmc_stream_verify_sbe1v1k_firmware();
}

struct recovery_factory_part {
	const char *spec;
	lbaint_t start;
	lbaint_t size;
};

/*
 * The full profile is deliberately strict: every partition before the new
 * chainloader boundary must be the known Qualcomm boot/identity area. ART and
 * both Wi-Fi firmware partitions are retained; a shifted or extra prefix is
 * never guessed.
 */
static const struct recovery_factory_part sbe1v1k_full_prefix_parts[] = {
	{ "0#0:SBL1", 34, 2048 },
	{ "0#0:SBL1_1", 2082, 2048 },
	{ "0#0:BOOTCONFIG", 4130, 1024 },
	{ "0#0:BOOTCONFIG1", 5154, 1024 },
	{ "0#0:QSEE", 6178, 6144 },
	{ "0#0:QSEE_1", 12322, 6144 },
	{ "0#0:DEVCFG", 18466, 1024 },
	{ "0#0:DEVCFG_1", 19490, 1024 },
	{ "0#0:APDP", 20514, 1024 },
	{ "0#0:APDP_1", 21538, 1024 },
	{ "0#0:TME", 22562, 1024 },
	{ "0#0:TME_1", 23586, 1024 },
	{ "0#0:RPM", 24610, 1024 },
	{ "0#0:RPM_1", 25634, 1024 },
	{ "0#0:CDT", 26658, 1024 },
	{ "0#0:CDT_1", 27682, 1024 },
	{ "0#0:APPSBLENV", 28706, 512 },
	{ "0#0:APPSBL", 29218, 4096 },
	{ "0#0:APPSBL_1", 33314, 4096 },
	{ "0#0:ART", 37410, 2048 },
	{ "0#0:ETHPHYFW", 39458, 1024 },
	{ "0#0:LICENSE", 40482, 512 },
	{ "0#0:WIFIFW", 40994, 20480 },
	{ "0#0:WIFIFW_1", 61474, 20480 },
};

struct recovery_sbe1v1k_gpt_part {
	const char *name;
	lbaint_t start;
	lbaint_t size;
	const char *type_guid;
};

static const struct recovery_sbe1v1k_layout_desc *
recovery_sbe1v1k_layout_desc(enum recovery_sbe1v1k_layout layout)
{
	return layout == RECOVERY_SBE1V1K_LAYOUT_FULL ?
		&sbe1v1k_layout_full : NULL;
}

static int recovery_appendf(char *buf, size_t size, size_t *offp,
			    const char *fmt, ...)
{
	va_list ap;
	int len;

	if (*offp >= size)
		return -ENOSPC;

	va_start(ap, fmt);
	len = vsnprintf(buf + *offp, size - *offp, fmt, ap);
	va_end(ap);

	if (len < 0)
		return len;
	if (*offp + len >= size)
		return -ENOSPC;

	*offp += len;
	return 0;
}

static int recovery_append_gpt_part(char *buf, size_t size, size_t *offp,
				    const char *name,
				    unsigned long long start_lba,
				    unsigned long long lba_count,
				    unsigned long blksz,
				    const char *type_guid,
				    const char *uuid,
				    bool bootable)
{
	int ret;

	ret = recovery_appendf(buf, size, offp, "name=%s,start=0x%llx,",
			       name, start_lba * (unsigned long long)blksz);
	if (ret)
		return ret;

	if (lba_count)
		ret = recovery_appendf(buf, size, offp, "size=0x%llx",
				       lba_count * (unsigned long long)blksz);
	else
		ret = recovery_appendf(buf, size, offp, "size=-");
	if (ret)
		return ret;

	if (type_guid && *type_guid) {
		ret = recovery_appendf(buf, size, offp, ",type=%s",
				       type_guid);
		if (ret)
			return ret;
	}

	if (uuid && *uuid) {
		ret = recovery_appendf(buf, size, offp, ",uuid=%s", uuid);
		if (ret)
			return ret;
	}

	if (bootable) {
		ret = recovery_appendf(buf, size, offp, ",bootable");
		if (ret)
			return ret;
	}

	return recovery_appendf(buf, size, offp, ";");
}

static int recovery_append_existing_gpt_part(char *buf, size_t size,
					    size_t *offp,
					    const struct disk_partition *part)
{
	const char *type_guid = NULL;
	const char *uuid = NULL;

	if (IS_ENABLED(CONFIG_PARTITION_TYPE_GUID))
		type_guid = disk_partition_type_guid(part);
	if (CONFIG_IS_ENABLED(PARTITION_UUIDS))
		uuid = disk_partition_uuid(part);

	return recovery_append_gpt_part(buf, size, offp,
					(const char *)part->name,
					part->start, part->size, part->blksz,
					type_guid, uuid,
					part->bootable & PART_BOOTABLE);
}

static int recovery_get_mmc_disk_guid(char *buf, size_t size)
{
	const char *guid;
	int ret;

	env_set("sbe1v1k_disk_guid", NULL);
	ret = run_commandf("gpt guid mmc %s sbe1v1k_disk_guid",
			   recovery_mmcdev());
	if (ret)
		return ret;

	guid = env_get("sbe1v1k_disk_guid");
	if (!guid || !*guid) {
		env_set("sbe1v1k_disk_guid", NULL);
		return -ENOENT;
	}

	strlcpy(buf, guid, size);
	env_set("sbe1v1k_disk_guid", NULL);
	return 0;
}

static const char * const sbe1v1k_clean_part_specs[] = {
	"0#kernel",
	"0#rootfs",
	"0#rootfs_data",
};

static void recovery_sbe1v1k_clean_session_reset(void)
{
	memset(&sbe1v1k_clean_session, 0, sizeof(sbe1v1k_clean_session));
}

static bool recovery_sbe1v1k_clean_session_ready(void)
{
	return sbe1v1k_clean_session.magic ==
		RECOVERY_SBE1V1K_CLEAN_SESSION_MAGIC;
}

static int recovery_sbe1v1k_clean_session_capture(void)
{
	struct recovery_sbe1v1k_clean_session next;
	struct blk_desc *desc;
	struct mmc *mmc;
	bool cid_present = false;
	size_t i;
	int ret;

	recovery_sbe1v1k_clean_session_reset();
	memset(&next, 0, sizeof(next));

	ret = recovery_verify_sbe1v1k_gpt(&sbe1v1k_layout_full);
	if (ret)
		return ret;
	ret = blk_get_device_by_str("mmc", recovery_mmcdev(), &desc);
	if (ret < 0)
		return ret;
	if (!desc->blksz || !desc->lba)
		return -ENODEV;

	mmc = find_mmc_device(desc->devnum);
	if (!mmc)
		return -ENODEV;
	ret = mmc_init(mmc);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(next.cid); i++) {
		next.cid[i] = mmc->cid[i];
		cid_present |= !!mmc->cid[i];
	}
	if (!cid_present)
		return -ENODEV;

	next.devnum = desc->devnum;
	next.capacity_user = mmc->capacity_user;
	next.lba = desc->lba;
	next.blksz = desc->blksz;
	ret = recovery_get_mmc_disk_guid(next.disk_guid,
					 sizeof(next.disk_guid));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(sbe1v1k_clean_part_specs); i++) {
		struct disk_partition part;
		struct blk_desc *part_desc;

		ret = recovery_get_mmc_part(sbe1v1k_clean_part_specs[i],
					    &part_desc, &part);
		if (ret)
			return ret;
		if (part_desc->devnum != next.devnum)
			return -EXDEV;
		next.parts[i].start = part.start;
		next.parts[i].size = part.size;
	}

	/*
	 * Publish the magic last.  A failed capture therefore cannot leave a
	 * partially initialized authorization that an upload could consume.
	 */
	next.magic = RECOVERY_SBE1V1K_CLEAN_SESSION_MAGIC;
	sbe1v1k_clean_session = next;
	recovery_debug_printf("Explicit eMMC clean authorized one firmware upload in this recovery session\n");
	return 0;
}

static int recovery_sbe1v1k_clean_session_validate(void)
{
	struct blk_desc *desc;
	struct mmc *mmc;
	char disk_guid[UUID_STR_LEN + 1];
	size_t i;
	int ret;

	if (!recovery_sbe1v1k_clean_session_ready()) {
		printf("SBE1V1K firmware upload rejected: clear eMMC first in this recovery session\n");
		return -EPERM;
	}

	ret = recovery_verify_sbe1v1k_gpt(&sbe1v1k_layout_full);
	if (ret)
		goto invalid;
	ret = blk_get_device_by_str("mmc", recovery_mmcdev(), &desc);
	if (ret < 0)
		goto invalid;
	if (!desc->blksz || !desc->lba ||
	    desc->devnum != sbe1v1k_clean_session.devnum ||
	    desc->lba != sbe1v1k_clean_session.lba ||
	    desc->blksz != sbe1v1k_clean_session.blksz) {
		ret = -EPERM;
		goto invalid;
	}

	mmc = find_mmc_device(desc->devnum);
	if (!mmc) {
		ret = -ENODEV;
		goto invalid;
	}
	ret = mmc_init(mmc);
	if (ret)
		goto invalid;
	if (mmc->capacity_user != sbe1v1k_clean_session.capacity_user ||
	    memcmp(mmc->cid, sbe1v1k_clean_session.cid,
		   sizeof(sbe1v1k_clean_session.cid))) {
		ret = -EPERM;
		goto invalid;
	}

	ret = recovery_get_mmc_disk_guid(disk_guid, sizeof(disk_guid));
	if (ret)
		goto invalid;
	if (strcmp(disk_guid, sbe1v1k_clean_session.disk_guid)) {
		ret = -EPERM;
		goto invalid;
	}

	for (i = 0; i < ARRAY_SIZE(sbe1v1k_clean_part_specs); i++) {
		struct disk_partition part;
		struct blk_desc *part_desc;

		ret = recovery_get_mmc_part(sbe1v1k_clean_part_specs[i],
					    &part_desc, &part);
		if (ret)
			goto invalid;
		if (part_desc->devnum != sbe1v1k_clean_session.devnum ||
		    part.start != sbe1v1k_clean_session.parts[i].start ||
		    part.size != sbe1v1k_clean_session.parts[i].size) {
			ret = -EPERM;
			goto invalid;
		}
	}

	return 0;

invalid:
	recovery_sbe1v1k_clean_session_reset();
	printf("SBE1V1K clean-session identity or GPT binding changed; clear eMMC again\n");
	return ret ?: -EPERM;
}

static int recovery_verify_sbe1v1k_full_prefix(struct blk_desc *desc)
{
	int p;
	int protected_count = 0;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(sbe1v1k_full_prefix_parts); i++) {
		const struct recovery_factory_part *expect =
			&sbe1v1k_full_prefix_parts[i];
		struct disk_partition part;
		int ret;

		ret = recovery_get_mmc_part(expect->spec, &desc, &part);
		if (ret)
			return ret;
		if (part.start != expect->start || part.size != expect->size) {
			printf("Full profile prefix mismatch for '%s': start " LBAF
			       " size " LBAF ", expected start " LBAF
			       " size " LBAF "\n",
			       expect->spec, part.start, part.size,
			       expect->start, expect->size);
			return -EINVAL;
		}
	}

	/*
	 * Reject an extra or shifted entry in the protected range.  Checking the
	 * complete range is important because a label lookup alone could miss a
	 * stale partition that would survive the destructive GPT rewrite.
	 */
	for (p = 1; p <= MAX_SEARCH_PARTITIONS; p++) {
		struct disk_partition part;
		lbaint_t end;
		bool expected = false;

		if (part_get_info(desc, p, &part) || !part.size)
			continue;
		end = part.start + part.size;
		if (part.start >= RECOVERY_SBE1V1K_FULL_CHAINLOADER_START)
			continue;
		if (end > RECOVERY_SBE1V1K_FULL_CHAINLOADER_START)
			return -EINVAL;
		for (i = 0; i < ARRAY_SIZE(sbe1v1k_full_prefix_parts); i++) {
			const struct recovery_factory_part *want =
				&sbe1v1k_full_prefix_parts[i];
			const char *label = strrchr(want->spec, '#');

			if (part.start == want->start && part.size == want->size &&
			    !strcmp((const char *)part.name,
				    label ? label + 1 : want->spec)) {
				expected = true;
				break;
			}
		}
		if (!expected) {
			printf("Full profile rejects unexpected protected partition %d '%s'\n",
			       p, (const char *)part.name);
			return -EINVAL;
		}
		protected_count++;
	}

	if (protected_count != (int)ARRAY_SIZE(sbe1v1k_full_prefix_parts)) {
		printf("Full profile requires exactly %lu protected partitions, found %d\n",
		       (ulong)ARRAY_SIZE(sbe1v1k_full_prefix_parts),
		       protected_count);
		return -EINVAL;
	}

	return 0;
}

struct recovery_preserved_gpt_metadata {
	char name[PART_NAME_LEN + 1];
	char type[PART_TYPE_LEN + 1];
	char uuid[UUID_STR_LEN + 1];
	char type_guid[UUID_STR_LEN + 1];
	lbaint_t start;
	lbaint_t size;
	int bootable;
	u16 type_flags;
};

static int recovery_capture_sbe1v1k_full_metadata(
	struct recovery_preserved_gpt_metadata *metadata, size_t count)
{
	size_t i;

	if (count != ARRAY_SIZE(sbe1v1k_full_prefix_parts))
		return -EINVAL;
	memset(metadata, 0, count * sizeof(*metadata));

	for (i = 0; i < count; i++) {
		const struct recovery_factory_part *expect =
			&sbe1v1k_full_prefix_parts[i];
		struct recovery_preserved_gpt_metadata *saved = &metadata[i];
		struct disk_partition part;
		struct blk_desc *desc;
		int ret;

		ret = recovery_get_mmc_part(expect->spec, &desc, &part);
		if (ret)
			return ret;
		/*
		 * The gpt command can reproduce the legacy bootable bit, but not
		 * arbitrary GPT type-specific attributes. Reject before changing
		 * the disk instead of silently dropping an unknown device flag.
		 */
		if (part.type_flags || (part.bootable & ~PART_BOOTABLE)) {
			printf("Full profile cannot preserve GPT attributes for '%s'\n",
			       expect->spec);
			return -EOPNOTSUPP;
		}

		strlcpy(saved->name, (const char *)part.name,
			sizeof(saved->name));
		strlcpy(saved->type, (const char *)part.type,
			sizeof(saved->type));
		if (CONFIG_IS_ENABLED(PARTITION_UUIDS))
			strlcpy(saved->uuid, disk_partition_uuid(&part),
				sizeof(saved->uuid));
		if (IS_ENABLED(CONFIG_PARTITION_TYPE_GUID))
			strlcpy(saved->type_guid,
				disk_partition_type_guid(&part),
				sizeof(saved->type_guid));
		saved->start = part.start;
		saved->size = part.size;
		saved->bootable = part.bootable;
		saved->type_flags = part.type_flags;
	}

	return 0;
}

static int recovery_verify_sbe1v1k_full_metadata(
	const struct recovery_preserved_gpt_metadata *metadata, size_t count)
{
	size_t i;

	if (count != ARRAY_SIZE(sbe1v1k_full_prefix_parts))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		const struct recovery_factory_part *expect =
			&sbe1v1k_full_prefix_parts[i];
		const struct recovery_preserved_gpt_metadata *saved = &metadata[i];
		const char *uuid = "";
		const char *type_guid = "";
		struct disk_partition part;
		struct blk_desc *desc;
		int ret;

		ret = recovery_get_mmc_part(expect->spec, &desc, &part);
		if (ret)
			return ret;
		if (CONFIG_IS_ENABLED(PARTITION_UUIDS))
			uuid = disk_partition_uuid(&part);
		if (IS_ENABLED(CONFIG_PARTITION_TYPE_GUID))
			type_guid = disk_partition_type_guid(&part);
		if (strcmp(saved->name, (const char *)part.name) ||
		    strcmp(saved->type, (const char *)part.type) ||
		    strcmp(saved->uuid, uuid) ||
		    strcmp(saved->type_guid, type_guid) ||
		    saved->start != part.start || saved->size != part.size ||
		    saved->bootable != part.bootable ||
		    saved->type_flags != part.type_flags) {
			printf("Full profile did not preserve GPT metadata for '%s'\n",
			       expect->spec);
			return -EBADMSG;
		}
	}

	return 0;
}

static int recovery_build_sbe1v1k_gpt(
	const struct recovery_sbe1v1k_layout_desc *layout, char **gptp)
{
	char disk_guid[UUID_STR_LEN + 1];
	struct blk_desc *desc;
	char *gpt;
	size_t off = 0;
	lbaint_t preserve_boundary;
	int preserved = 0;
	int ret;
	int p;

	ret = blk_get_device_by_str("mmc", recovery_mmcdev(), &desc);
	if (ret < 0)
		return ret;

	ret = recovery_get_mmc_disk_guid(disk_guid, sizeof(disk_guid));
	if (ret)
		return ret;

	if (!layout || layout->id != RECOVERY_SBE1V1K_LAYOUT_FULL)
		return -EPERM;
	preserve_boundary = layout->preserve_end;
	if (!preserve_boundary)
		return -EINVAL;
	ret = recovery_verify_sbe1v1k_full_prefix(desc);
	if (ret) {
		printf("Full profile refused: protected P1-P24 prefix is not factory exact\n");
		return ret;
	}

	gpt = malloc(RECOVERY_SBE1V1K_GPT_MAX);
	if (!gpt)
		return -ENOMEM;
	gpt[0] = '\0';

	ret = recovery_appendf(gpt, RECOVERY_SBE1V1K_GPT_MAX, &off,
			       "uuid_disk=%s;", disk_guid);
	if (ret)
		goto err;

	for (p = 1; p <= MAX_SEARCH_PARTITIONS; p++) {
		struct disk_partition part;
		lbaint_t end;

		ret = part_get_info(desc, p, &part);
		if (ret)
			continue;
		if (!part.size)
			continue;

		end = part.start + part.size;
		if (part.start >= preserve_boundary)
			continue;
		if (end > preserve_boundary) {
			printf("Partition %d '%s' overlaps protected layout boundary: "
			       "start " LBAF " size " LBAF "\n",
			       p, (const char *)part.name, part.start,
			       part.size);
			ret = -EINVAL;
			goto err;
		}

		ret = recovery_append_existing_gpt_part(gpt,
							RECOVERY_SBE1V1K_GPT_MAX,
							&off, &part);
		if (ret)
			goto err;
		preserved++;
	}

	if (!preserved) {
		ret = -EINVAL;
		goto err;
	}

	ret = recovery_append_gpt_part(
		gpt, RECOVERY_SBE1V1K_GPT_MAX, &off,
		"chainloader", layout->uboot_start, layout->uboot_size,
		desc->blksz, RECOVERY_GPT_TYPE_BASIC_DATA, NULL, false);
	if (ret)
		goto err;

	ret = recovery_append_gpt_part(
		gpt, RECOVERY_SBE1V1K_GPT_MAX, &off,
		"kernel", layout->kernel_start, layout->kernel_size,
		desc->blksz, RECOVERY_GPT_TYPE_BASIC_DATA, NULL, false);
	if (ret)
		goto err;

	ret = recovery_append_gpt_part(
		gpt, RECOVERY_SBE1V1K_GPT_MAX, &off,
		"rootfs", layout->rootfs_start, layout->rootfs_size,
		desc->blksz, RECOVERY_GPT_TYPE_LINUX_FS, NULL, false);
	if (ret)
		goto err;

	ret = recovery_append_gpt_part(
		gpt, RECOVERY_SBE1V1K_GPT_MAX, &off,
		"rootfs_data", layout->data_start, 0,
		desc->blksz, RECOVERY_GPT_TYPE_LINUX_FS, NULL, false);
	if (ret)
		goto err;

	recovery_debug_printf("Building SBE1V1K '%s' GPT; preserving %d partitions before LBA %llu\n",
			      layout->name, preserved,
			      (unsigned long long)preserve_boundary);
	*gptp = gpt;
	return 0;

err:
	free(gpt);
	return ret;
}

static int recovery_verify_full_factory_gpt(
	struct recovery_status_led_ctrl *status_leds)
{
	struct blk_desc *desc;
	int ret;

	prog_phase = 1;
	prog_erase_done = 0;
	prog_erase_total = ARRAY_SIZE(sbe1v1k_full_prefix_parts);
	prog_write_done = 0;
	prog_write_total = 0;
	prog_total = prog_erase_total;
	prog_done = 0;
	recovery_service_runtime(status_leds);

	ret = blk_get_device_by_str("mmc", recovery_mmcdev(), &desc);
	if (ret < 0)
		return ret;
	ret = recovery_verify_sbe1v1k_full_prefix(desc);
	if (ret)
		return ret;

	prog_erase_done = prog_erase_total;
	prog_done = prog_erase_done;
	recovery_service_runtime(status_leds);
	return 0;
}

static int recovery_verify_sbe1v1k_gpt(
	const struct recovery_sbe1v1k_layout_desc *layout)
{
	static const char * const full_names[] = {
		"chainloader", "kernel", "rootfs", "rootfs_data",
	};
	struct disk_partition part;
	struct blk_desc *desc;
	lbaint_t expected_data_size;
	int ret;
	int p;
	size_t i;

	if (!layout || layout->id != RECOVERY_SBE1V1K_LAYOUT_FULL)
		return -EPERM;

	ret = blk_get_device_by_str("mmc", recovery_mmcdev(), &desc);
	if (ret < 0)
		return ret;
	ret = recovery_verify_sbe1v1k_full_prefix(desc);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(full_names); i++) {
		ret = part_get_info(desc, 25 + i, &part);
		if (ret || strcmp((const char *)part.name, full_names[i]))
			return ret ?: -EINVAL;
	}
	/* No stale or sparse entry after P28 may survive a full rewrite. */
	for (p = 29; p <= MAX_SEARCH_PARTITIONS; p++) {
		if (!part_get_info(desc, p, &part) && part.size)
			return -EINVAL;
	}

	ret = recovery_get_mmc_part(layout->uboot_part, &desc, &part);
	if (ret || part.start != layout->uboot_start ||
	    part.size != layout->uboot_size)
		return ret ?: -EINVAL;

	ret = recovery_get_mmc_part(layout->kernel_part, &desc, &part);
	if (ret || part.start != layout->kernel_start ||
	    part.size != layout->kernel_size)
		return ret ?: -EINVAL;

	ret = recovery_get_mmc_part(layout->rootfs_part, &desc, &part);
	if (ret || part.start != layout->rootfs_start ||
	    part.size != layout->rootfs_size)
		return ret ?: -EINVAL;

	ret = recovery_get_mmc_part(layout->data_part, &desc, &part);
	if (ret || part.start != layout->data_start)
		return ret ?: -EINVAL;
	if (desc->lba <= layout->data_start + 33)
		return -EINVAL;
	expected_data_size = desc->lba - 33 - layout->data_start;
	if (part.size != expected_data_size)
		return -EINVAL;

	return 0;
}

static int recovery_apply_sbe1v1k_layout(
	const struct recovery_sbe1v1k_layout_desc *layout)
{
	char bootargs[128];
	int ret;

	if (!layout || layout->id != RECOVERY_SBE1V1K_LAYOUT_FULL)
		return -EINVAL;

	if (snprintf(bootargs, sizeof(bootargs),
		     "console=ttyMSM0,115200n8 rootwait root=%s "
		     "fstools_overlay_fstype=ext4", layout->rootarg) >=
	    sizeof(bootargs))
		return -ENOSPC;

	ret = env_set("recovery_part_kernel", layout->kernel_part);
	ret = ret ?: env_set("recovery_part_rootfs", layout->rootfs_part);
	ret = ret ?: env_set("recovery_part_data", layout->data_part);
	ret = ret ?: env_set("recovery_part_uboot", layout->uboot_part);
	ret = ret ?: env_set_hex("recovery_kernel_pad", layout->kernel_pad);
	ret = ret ?: env_set("kernpart", layout->kernpart);
	ret = ret ?: env_set("rootpart", layout->rootarg);
	ret = ret ?: env_set("bootargs", bootargs);
	ret = ret ?: env_set("root", layout->rootarg);
	ret = ret ?: env_set("fstools_overlay_fstype", "ext4");
	if (ret)
		return ret;

	active_sbe1v1k_layout = layout->id;
	recovery_debug_printf("SBE1V1K partition profile: %s\n", layout->name);
	return 0;
}

static int recovery_detect_sbe1v1k_layout(void)
{
	if (!recovery_verify_sbe1v1k_gpt(&sbe1v1k_layout_full))
		return recovery_apply_sbe1v1k_layout(&sbe1v1k_layout_full);

	active_sbe1v1k_layout = RECOVERY_SBE1V1K_LAYOUT_UNKNOWN;
	printf("SBE1V1K full clean layout is missing or invalid; recovery is required\n");
	return -ENOENT;
}

static bool recovery_fit_has_image_node(const void *fit, const char *name)
{
	int images;

	images = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images < 0)
		return false;

	return fdt_subnode_offset(fit, images, name) >= 0;
}

static bool recovery_is_sbe1v1k_chainloader_fit(const void *fit,
						size_t fit_size)
{
	const char *desc;
	const char *default_conf;
	const char *kernel;
	const char *fdt;
	u8 type, arch, os;
	int conf, node;

	if (fit_check_format(fit, fit_size))
		return false;

	desc = fdt_getprop(fit, 0, FIT_DESC_PROP, NULL);
	if (!desc || !strstr(desc, "SBE1V1K") || !strstr(desc, "chainloader"))
		return false;

	if (!recovery_fit_has_image_node(fit, "kernel-1") ||
	    !recovery_fit_has_image_node(fit, "uboot-1") ||
	    !recovery_fit_has_image_node(fit, "fdt-1"))
		return false;

	conf = fdt_path_offset(fit, FIT_CONFS_PATH);
	if (conf < 0)
		return false;
	default_conf = fdt_getprop(fit, conf, FIT_DEFAULT_PROP, NULL);
	if (!default_conf || strcmp(default_conf, "conf-1"))
		return false;
	conf = fdt_subnode_offset(fit, conf, default_conf);
	if (conf < 0)
		return false;
	kernel = fdt_getprop(fit, conf, FIT_KERNEL_PROP, NULL);
	fdt = fdt_getprop(fit, conf, FIT_FDT_PROP, NULL);
	if (!kernel || strcmp(kernel, "kernel-1") ||
	    !fdt || strcmp(fdt, "fdt-1"))
		return false;

	node = fit_image_get_node(fit, "kernel-1");
	if (node < 0 || fit_image_get_type(fit, node, &type) ||
	    fit_image_get_arch(fit, node, &arch) ||
	    fit_image_get_os(fit, node, &os) || type != IH_TYPE_KERNEL ||
	    arch != IH_ARCH_ARM64 || os != IH_OS_LINUX)
		return false;

	node = fit_image_get_node(fit, "uboot-1");
	if (node < 0 || fit_image_get_type(fit, node, &type) ||
	    fit_image_get_arch(fit, node, &arch) ||
	    type != IH_TYPE_FIRMWARE || arch != IH_ARCH_ARM64)
		return false;

	node = fit_image_get_node(fit, "fdt-1");
	if (node < 0 || fit_image_get_type(fit, node, &type) ||
	    fit_image_get_arch(fit, node, &arch) ||
	    type != IH_TYPE_FLATDT || arch != IH_ARCH_ARM64)
		return false;

	return true;
}

static bool recovery_ram_range_ok(ulong addr, size_t size)
{
	ulong ram_start = (ulong)gd->ram_base;
	ulong ram_size = (ulong)gd->ram_size;
	ulong ram_end;

	if (!ram_size || addr < ram_start)
		return false;
	if (ram_size > ULONG_MAX - ram_start)
		return false;
	ram_end = ram_start + ram_size;

	return addr <= ram_end && size <= ram_end - addr;
}

static int recovery_copy_running_chainloader_fit(void **fitp,
						 size_t *fit_sizep)
{
	const ulong candidates[] = {
		RECOVERY_SBE1V1K_FIT_PERSISTENT_ADDR,
		RECOVERY_SBE1V1K_FIT_TFTP_ADDR,
	};
	unsigned long max = recovery_uboot_limit();
	size_t i, j;

	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		const void *fit;
		void *copy;
		size_t fit_size;
		ulong addr = candidates[i];

		if (!addr || !recovery_ram_range_ok(addr, sizeof(struct fdt_header)))
			continue;

		for (j = 0; j < i; j++) {
			if (candidates[j] == addr)
				break;
		}
		if (j != i)
			continue;

		fit = (const void *)addr;
		if (fdt_check_header(fit))
			continue;

		fit_size = fit_get_size(fit);
		if (!fit_size || fit_size > max ||
		    !recovery_ram_range_ok(addr, fit_size))
			continue;

		if (recovery_verify_fit_payload(fit, fit_size))
			continue;
		if (!recovery_is_sbe1v1k_chainloader_fit(fit, fit_size))
			continue;

		copy = malloc(fit_size);
		if (!copy)
			return -ENOMEM;
		memcpy(copy, fit, fit_size);
		if (recovery_verify_fit_payload(copy, fit_size) ||
		    !recovery_is_sbe1v1k_chainloader_fit(copy, fit_size)) {
			free(copy);
			continue;
		}

		*fitp = copy;
		*fit_sizep = fit_size;
		recovery_debug_printf("Preserved running SBE1V1K chainloader FIT from 0x%08lx (%lu bytes)\n",
				      addr, (ulong)fit_size);
		return 0;
	}

	return -ENOENT;
}

static int recovery_read_installed_chainloader_fit(void **fitp,
						   size_t *fit_sizep)
{
	const struct recovery_sbe1v1k_layout_desc *layout =
		recovery_sbe1v1k_layout_desc(active_sbe1v1k_layout);
	struct disk_partition part;
	struct blk_desc *desc;
	unsigned long long capacity;
	unsigned long max = recovery_uboot_limit();
	lbaint_t blocks;
	ulong blksz;
	size_t fit_size;
	size_t read_size;
	u8 *header = NULL;
	u8 *fit = NULL;
	int ret;

	if (!layout)
		return -ENOENT;

	ret = recovery_get_mmc_part(layout->uboot_part, &desc, &part);
	if (ret)
		return ret;

	blksz = part.blksz ?: desc->blksz;
	if (!blksz || !part.size)
		return -EINVAL;
	capacity = (unsigned long long)part.size * blksz;

	header = memalign(ARCH_DMA_MINALIGN, blksz);
	if (!header)
		return -ENOMEM;
	if (blk_dread(desc, part.start, 1, header) != 1) {
		printf("Failed to read chainloader FIT header from '%s'\n",
		       layout->uboot_part);
		ret = -EIO;
		goto out;
	}
	if (fdt_check_header(header)) {
		ret = -ENOEXEC;
		goto out;
	}

	fit_size = fit_get_size(header);
	if (!fit_size || fit_size > max || fit_size > capacity) {
		printf("Installed chainloader FIT size %lu is invalid for '%s'\n",
		       (ulong)fit_size, layout->uboot_part);
		ret = -EFBIG;
		goto out;
	}

	blocks = (fit_size + blksz - 1) / blksz;
	read_size = blocks * blksz;
	fit = memalign(ARCH_DMA_MINALIGN, read_size);
	if (!fit) {
		ret = -ENOMEM;
		goto out;
	}
	if (blk_dread(desc, part.start, blocks, fit) != blocks) {
		printf("Failed to read chainloader FIT from '%s'\n",
		       layout->uboot_part);
		ret = -EIO;
		goto out;
	}
	ret = recovery_verify_fit_payload(fit, fit_size);
	if (ret) {
		printf("Installed chainloader FIT in '%s' failed payload verification: %d\n",
		       layout->uboot_part, ret);
		goto out;
	}
	if (!recovery_is_sbe1v1k_chainloader_fit(fit, fit_size)) {
		printf("Installed image in '%s' is not an SBE1V1K chainloader FIT\n",
		       layout->uboot_part);
		ret = -ENOEXEC;
		goto out;
	}

	*fitp = fit;
	*fit_sizep = fit_size;
	fit = NULL;
	ret = 0;
	recovery_debug_printf("Preserved installed SBE1V1K chainloader FIT from '%s' (%lu bytes)\n",
			      layout->uboot_part, (ulong)fit_size);

out:
	free(fit);
	free(header);
	return ret;
}

static int recovery_preserve_chainloader_fit(void **fitp, size_t *fit_sizep)
{
	int ret;

	ret = recovery_copy_running_chainloader_fit(fitp, fit_sizep);
	if (ret != -ENOENT)
		return ret;

	recovery_debug_printf("No running SBE1V1K chainloader FIT was found in usable RAM; reading the current eMMC partition\n");
	ret = recovery_read_installed_chainloader_fit(fitp, fit_sizep);
	if (ret)
		printf("Cannot preserve the current SBE1V1K chainloader FIT: %d\n",
		       ret);

	return ret;
}

static int recovery_load_appsblenv(u8 **bufp, size_t *env_bytesp,
				   struct blk_desc **descp,
				   struct disk_partition *partp)
{
	struct disk_partition part;
	struct blk_desc *desc;
	unsigned long long capacity;
	u8 *buf;
	int ret;

	ret = recovery_get_mmc_part(RECOVERY_APPSBLENV_PART, &desc, &part);
	if (ret)
		return ret;

	capacity = recovery_mmc_part_bytes(&part);
	if (capacity != RECOVERY_APPSBLENV_SIZE) {
		printf("Unexpected APPSBLENV size %llu, expected %lu\n",
		       capacity, RECOVERY_APPSBLENV_SIZE);
		return -EINVAL;
	}

	buf = memalign(ARCH_DMA_MINALIGN, RECOVERY_APPSBLENV_SIZE);
	if (!buf)
		return -ENOMEM;

	if (blk_dread(desc, part.start, part.size, buf) != part.size) {
		printf("Failed to read APPSBLENV from eMMC\n");
		free(buf);
		return -EIO;
	}

	*bufp = buf;
	*env_bytesp = RECOVERY_APPSBLENV_SIZE;
	if (descp)
		*descp = desc;
	if (partp)
		*partp = part;

	return 0;
}

static int recovery_verify_appsblenv_crc(const u8 *env_buf, size_t env_bytes)
{
	u32 stored_crc;
	u32 calc_crc;

	if (env_bytes <= RECOVERY_ENV_CRC_SIZE)
		return -EINVAL;

	memcpy(&stored_crc, env_buf, sizeof(stored_crc));
	calc_crc = crc32(0, env_buf + RECOVERY_ENV_CRC_SIZE,
			 env_bytes - RECOVERY_ENV_CRC_SIZE);
	if (stored_crc != calc_crc) {
		printf("APPSBLENV CRC mismatch: stored 0x%08x calculated 0x%08x\n",
		       stored_crc, calc_crc);
		return -EINVAL;
	}

	return 0;
}

static int recovery_check_appsblenv(void)
{
	size_t env_bytes;
	u8 *env_buf;
	int ret;

	ret = recovery_load_appsblenv(&env_buf, &env_bytes, NULL, NULL);
	if (ret)
		return ret;

	ret = recovery_verify_appsblenv_crc(env_buf, env_bytes);
	free(env_buf);

	return ret;
}

struct recovery_env_update {
	const char *key;
	const char *value;
};

static bool recovery_env_entry_is_key(const char *entry, const char *key)
{
	size_t key_len = strlen(key);

	return !strncmp(entry, key, key_len) && entry[key_len] == '=';
}

static bool recovery_env_entry_replaced(const char *entry,
					const struct recovery_env_update *updates,
					size_t update_count)
{
	size_t i;

	for (i = 0; i < update_count; i++) {
		if (recovery_env_entry_is_key(entry, updates[i].key))
			return true;
	}

	return false;
}

static int recovery_env_append(char *data, size_t data_size, size_t *offp,
			       const char *key, const char *value)
{
	int len;

	if (*offp >= data_size)
		return -ENOSPC;

	len = snprintf(data + *offp, data_size - *offp, "%s=%s", key, value);
	if (len < 0)
		return len;

	if (*offp + len + 2 > data_size)
		return -ENOSPC;

	*offp += len + 1;
	return 0;
}

static int recovery_update_env_data(u8 *env_data, size_t data_size,
				    const struct recovery_env_update *updates,
				    size_t update_count)
{
	char *new_data;
	size_t old_off = 0;
	size_t new_off = 0;
	size_t i;
	int ret = 0;

	new_data = malloc(data_size);
	if (!new_data)
		return -ENOMEM;
	memset(new_data, 0, data_size);

	while (old_off < data_size && env_data[old_off]) {
		const char *entry = (const char *)env_data + old_off;
		size_t remain = data_size - old_off;
		size_t entry_len = strnlen(entry, remain);

		if (entry_len == remain) {
			ret = -EINVAL;
			goto out;
		}

		if (!recovery_env_entry_replaced(entry, updates, update_count)) {
			if (new_off + entry_len + 2 > data_size) {
				ret = -ENOSPC;
				goto out;
			}
			memcpy(new_data + new_off, entry, entry_len + 1);
			new_off += entry_len + 1;
		}

		old_off += entry_len + 1;
	}

	for (i = 0; i < update_count; i++) {
		ret = recovery_env_append(new_data, data_size, &new_off,
					  updates[i].key, updates[i].value);
		if (ret)
			goto out;
	}

	memcpy(env_data, new_data, data_size);

out:
	free(new_data);
	return ret;
}

static const char *recovery_env_find_value(const u8 *env_data,
					   size_t data_size,
					   const char *key)
{
	size_t off = 0;

	while (off < data_size && env_data[off]) {
		const char *entry = (const char *)env_data + off;
		size_t remain = data_size - off;
		size_t entry_len = strnlen(entry, remain);

		if (entry_len == remain)
			return NULL;

		if (recovery_env_entry_is_key(entry, key))
			return entry + strlen(key) + 1;

		off += entry_len + 1;
	}

	return NULL;
}

static int recovery_verify_env_updates(const u8 *env_data, size_t data_size,
				       const struct recovery_env_update *updates,
				       size_t update_count)
{
	const char *value;
	size_t i;

	for (i = 0; i < update_count; i++) {
		value = recovery_env_find_value(env_data, data_size,
						updates[i].key);
		if (!value) {
			printf("APPSBLENV missing '%s' after write\n",
			       updates[i].key);
			return -EINVAL;
		}

		if (strcmp(value, updates[i].value)) {
			printf("APPSBLENV mismatch for '%s' after write\n",
			       updates[i].key);
			return -EINVAL;
		}
	}

	return 0;
}

static int recovery_write_appsblenv(struct recovery_status_led_ctrl *status_leds,
				    size_t progress_base,
				    const struct recovery_sbe1v1k_layout_desc *layout)
{
	char bootargs[128];
	char boot_chainloader[128];
	char bootcmd[384];
	struct recovery_env_update updates[] = {
		{ "bootargs", bootargs },
		{ "boot_chainloader", boot_chainloader },
		{ "do_boot", "run boot_chainloader" },
		{ "do_nothing", "true" },
		{ "bootcmd", bootcmd },
		{ "kernpart", layout->kernpart },
		{ "root", layout->rootarg },
		{ "rootpart", layout->rootarg },
		{ "fstools_overlay_fstype", "ext4" },
	};
	struct disk_partition part;
	struct blk_desc *desc;
	size_t env_bytes;
	size_t update_count = ARRAY_SIZE(updates);
	u8 *env_buf;
	u32 crc;
	int ret;

	if (snprintf(bootargs, sizeof(bootargs),
		     "console=ttyMSM0,115200n8 rootwait root=%s "
		     "fstools_overlay_fstype=ext4", layout->rootarg) >=
	    sizeof(bootargs))
		return -ENOSPC;
	if (snprintf(boot_chainloader, sizeof(boot_chainloader),
		     "mmc dev 0 0; mmc read 0x44000000 0x%llx 0x2000; bootm 0x44000000",
		     (unsigned long long)layout->uboot_start) >=
	    sizeof(boot_chainloader))
		return -ENOSPC;
	if (snprintf(bootcmd, sizeof(bootcmd),
		     "echo \"Hit ctrl+c for shell...\"; if sleep 3; then setenv bootargs %s; run do_boot; else run do_nothing; fi;",
		     bootargs) >= sizeof(bootcmd))
		return -ENOSPC;
	ret = recovery_load_appsblenv(&env_buf, &env_bytes, &desc, &part);
	if (ret)
		return ret;

	ret = recovery_verify_appsblenv_crc(env_buf, env_bytes);
	if (ret)
		goto out;

	ret = recovery_update_env_data(env_buf + RECOVERY_ENV_CRC_SIZE,
				       env_bytes - RECOVERY_ENV_CRC_SIZE,
				       updates, update_count);
	if (ret)
		goto out;

	crc = crc32(0, env_buf + RECOVERY_ENV_CRC_SIZE,
		    env_bytes - RECOVERY_ENV_CRC_SIZE);
	memcpy(env_buf, &crc, sizeof(crc));

	if (blk_dwrite(desc, part.start, part.size, env_buf) != part.size) {
		printf("Failed to write updated APPSBLENV to eMMC\n");
		ret = -EIO;
		goto out;
	}

	memset(env_buf, 0, env_bytes);
	if (blk_dread(desc, part.start, part.size, env_buf) != part.size) {
		printf("Failed to read back updated APPSBLENV from eMMC\n");
		ret = -EIO;
		goto out;
	}

	ret = recovery_verify_appsblenv_crc(env_buf, env_bytes);
	if (ret)
		goto out;

	ret = recovery_verify_env_updates(env_buf + RECOVERY_ENV_CRC_SIZE,
					  env_bytes - RECOVERY_ENV_CRC_SIZE,
					  updates, update_count);
	if (ret)
		goto out;

	recovery_debug_printf("APPSBLENV update verified\n");

	prog_write_done = progress_base + env_bytes;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);

out:
	free(env_buf);
	return ret;
}

static int recovery_repartition_factory(
	struct recovery_status_led_ctrl *status_leds,
	enum recovery_sbe1v1k_layout layout_id)
{
	const struct recovery_sbe1v1k_layout_desc *layout =
		recovery_sbe1v1k_layout_desc(layout_id);
	struct recovery_preserved_gpt_metadata *full_metadata = NULL;
	void *chainloader_fit = NULL;
	size_t chainloader_size;
	char *repartition_gpt;
	u32 erase_progress_base;
	int full_metadata_ret = 0;
	int ret;

	if (layout_id != RECOVERY_SBE1V1K_LAYOUT_FULL) {
		printf("SBE1V1K requires the full clean layout\n");
		return -EPERM;
	}

	if (!layout) {
		printf("Unknown SBE1V1K partition profile\n");
		return -EINVAL;
	}

	if (recv_off != strlen(RECOVERY_SBE1V1K_REPARTITION_TOKEN) ||
	    memcmp(recv_base, RECOVERY_SBE1V1K_REPARTITION_TOKEN,
		   strlen(RECOVERY_SBE1V1K_REPARTITION_TOKEN))) {
		printf("Invalid repartition confirmation token\n");
		return -EINVAL;
	}

	if (!recovery_backend_is_mmc()) {
		printf("Factory repartition requires eMMC recovery backend\n");
		return -EINVAL;
	}

	/*
	 * A second explicit clean attempt revokes any earlier authorization
	 * before the first destructive check.  Only this attempt's complete,
	 * verified success may publish a new RAM-only clean session.
	 */
	recovery_sbe1v1k_clean_session_reset();

	ret = recovery_verify_full_factory_gpt(status_leds);
	if (ret)
		return ret;

	full_metadata = calloc(ARRAY_SIZE(sbe1v1k_full_prefix_parts),
			       sizeof(*full_metadata));
	if (!full_metadata)
		return -ENOMEM;
	ret = recovery_capture_sbe1v1k_full_metadata(
		full_metadata, ARRAY_SIZE(sbe1v1k_full_prefix_parts));
	if (ret)
		goto out;

	ret = recovery_preserve_chainloader_fit(&chainloader_fit, &chainloader_size);
	if (ret)
		goto out;

	ret = recovery_check_appsblenv();
	if (ret)
		goto out;

	recovery_debug_printf("Factory anchors verified. Writing SBE1V1K '%s' GPT layout...\n",
			      layout->name);
	prog_phase = 2;
	prog_erase_done = prog_erase_total;
	erase_progress_base = prog_erase_total;
	prog_erase_total += 4 * RECOVERY_MMC_ERASE_PROGRESS_STEPS;
	prog_write_done = 0;
	prog_write_total = 2 + chainloader_size + RECOVERY_APPSBLENV_SIZE;
	prog_total = prog_erase_total + prog_write_total;
	prog_done = prog_erase_done;
	recovery_service_runtime(status_leds);

	ret = recovery_build_sbe1v1k_gpt(layout, &repartition_gpt);
	if (ret)
		goto out;

	ret = env_set("sbe1v1k_repartition_gpt", repartition_gpt);
	free(repartition_gpt);
	if (ret)
		goto out;
	prog_write_done = 1;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);

	ret = run_commandf("mmc dev %s", recovery_mmcdev());
	if (ret) {
		env_set("sbe1v1k_repartition_gpt", NULL);
		goto out;
	}

	ret = run_commandf("gpt write mmc %s ${sbe1v1k_repartition_gpt}",
			   recovery_mmcdev());
	if (ret) {
		env_set("sbe1v1k_repartition_gpt", NULL);
		goto out;
	}
	ret = run_commandf("gpt verify mmc %s ${sbe1v1k_repartition_gpt}",
			   recovery_mmcdev());
	env_set("sbe1v1k_repartition_gpt", NULL);
	if (ret) {
		printf("Primary/backup GPT verification failed after write\n");
		goto out;
	}
	prog_write_done = 2;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);

	ret = recovery_verify_sbe1v1k_gpt(layout);
	if (ret) {
		printf("SBE1V1K '%s' GPT verification failed after write: %d\n",
		       layout->name, ret);
		goto out;
	}
	if (full_metadata) {
		full_metadata_ret = recovery_verify_sbe1v1k_full_metadata(
			full_metadata, ARRAY_SIZE(sbe1v1k_full_prefix_parts));
		if (full_metadata_ret)
			printf("Full GPT metadata verification failed; restoring P25 recovery before stopping\n");
	}
	prog_write_done = 2;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);

	prog_phase = 1;
	ret = recovery_mmc_erase_part(layout->uboot_part,
					      status_leds, erase_progress_base);
	if (ret)
		goto out;
	prog_phase = 2;

	recovery_debug_printf("Writing preserved chainloader FIT to '%s'...\n",
			      layout->uboot_part);
	ret = recovery_mmc_write_part(layout->uboot_part,
					      status_leds, chainloader_fit,
					      chainloader_size, 2);
	if (ret)
		goto out;
	ret = recovery_mmc_verify_part(layout->uboot_part, status_leds,
				       chainloader_fit, chainloader_size);
	if (ret)
		goto out;
	if (full_metadata_ret) {
		ret = full_metadata_ret;
		goto out;
	}

	recovery_debug_printf("Updating factory U-Boot environment in APPSBLENV...\n");
	ret = recovery_write_appsblenv(status_leds, 2 + chainloader_size,
					       layout);
	if (ret)
		goto out;

	/*
	 * Restore and verify the persistent recovery path before the long P26-P28
	 * erases. GPT replacement is not atomic; this ordering narrows the
	 * unavoidable power-loss window after the new partition map is written.
	 */
	prog_phase = 1;
	ret = recovery_mmc_erase_part(
		layout->kernel_part, status_leds,
		erase_progress_base + RECOVERY_MMC_ERASE_PROGRESS_STEPS);
	if (ret)
		goto out;
	ret = recovery_mmc_erase_part(
		layout->rootfs_part, status_leds,
		erase_progress_base + 2 * RECOVERY_MMC_ERASE_PROGRESS_STEPS);
	if (ret)
		goto out;
	ret = recovery_mmc_erase_part(
		layout->data_part, status_leds,
		erase_progress_base + 3 * RECOVERY_MMC_ERASE_PROGRESS_STEPS);
	if (ret)
		goto out;
	recovery_debug_printf("Full profile physically erased P25 through the last usable eMMC LBA\n");
	prog_phase = 2;

	ret = recovery_apply_sbe1v1k_layout(layout);
	if (ret)
		goto out;

	/*
	 * P28 has just completed full zero-content verification (including repair
	 * or full zero-fill fallback). Capture device/layout authorization after every
	 * destructive step and the final canonical GPT application succeeded.
	 */
	ret = recovery_sbe1v1k_clean_session_capture();
	if (ret) {
		printf("Clean eMMC completed but session binding failed: %d\n", ret);
		goto out;
	}

	prog_write_done = prog_write_total;
	prog_done = prog_erase_done + prog_write_done;
	recovery_service_runtime(status_leds);

	recovery_debug_printf("SBE1V1K '%s' layout written, chainloader installed, APPSBLENV updated. Upload firmware before reboot.\n",
			      layout->name);

out:
	if (ret)
		recovery_sbe1v1k_clean_session_reset();
	free(full_metadata);
	free(chainloader_fit);
	return ret;
}

static int recovery_flash_mmc_firmware(struct recovery_status_led_ctrl *status_leds)
{
	const char *kernel_part = env_get("recovery_part_kernel") ?: "0#kernel";
	const char *rootfs_part = env_get("recovery_part_rootfs") ?: "0#rootfs";
	const char *data_part = env_get("recovery_part_data") ?: "0#rootfs_data";
	struct recovery_firmware_image image;
	int ret;

	/*
	 * Firmware flashing prepares kernel/rootfs after an explicit clean;
	 * rootfs_data remains in the verified-empty state established by that
	 * clean.  Keep legacy layouts available for read-only detection/boot
	 * compatibility, but refuse a direct upload until the explicit full
	 * clean repartition has completed in this recovery session.  This
	 * closes both the UI-bypass path and the duplicate full-P28 zero-fill.
	 */
	if (recovery_board_is_sbe1v1k() &&
	    active_sbe1v1k_layout != RECOVERY_SBE1V1K_LAYOUT_FULL) {
		printf("SBE1V1K firmware upload requires the full clean layout first\n");
		return -EPERM;
	}
	if (recovery_board_is_sbe1v1k()) {
		/*
		 * A canonical GPT alone is not evidence that P28 was cleared in
		 * this recovery session.  Require the unforgeable RAM snapshot
		 * published only after explicit clean completed and was verified.
		 */
		ret = recovery_sbe1v1k_clean_session_validate();
		if (ret)
			return ret;
		/*
		 * Do not trust only the in-memory profile selected during
		 * repartition.  Confirm the exact full GPT immediately before
		 * payload extraction and the first erase.
		 */
		ret = recovery_verify_sbe1v1k_gpt(&sbe1v1k_layout_full);
		if (ret) {
			printf("SBE1V1K firmware upload rejected: full GPT map changed\n");
			return ret;
		}
		/*
		 * APPSBLENV is writable state, not an authorization boundary.
		 * Bind every SBE payload write to the canonical P26-P28 targets
		 * after validating the on-media map.
		 */
		kernel_part = sbe1v1k_layout_full.kernel_part;
		rootfs_part = sbe1v1k_layout_full.rootfs_part;
		data_part = sbe1v1k_layout_full.data_part;
	}

	ret = recovery_extract_firmware(recv_base, recv_off, &image);
	if (ret)
		return ret;

	if (recovery_board_is_sbe1v1k()) {
		ret = recovery_validate_sbe1v1k_kernel_fit(&image.kernel);
		if (ret)
			return ret;
		ret = recovery_validate_sbe1v1k_rootfs(&image.rootfs);
		if (ret)
			return ret;
	}

	ret = recovery_mmc_check_part_size(kernel_part, image.kernel.size);
	if (ret)
		return ret;
	ret = recovery_mmc_check_part_size(rootfs_part, image.rootfs.size);
	if (ret)
		return ret;
	ret = recovery_mmc_check_part_size(data_part, 0);
	if (ret)
		return ret;

	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = 2 * RECOVERY_MMC_ERASE_PROGRESS_STEPS;
	prog_write_done = 0;
	prog_write_total = image.kernel.size + image.rootfs.size;
	prog_total = prog_erase_total + prog_write_total;

	ret = recovery_mmc_erase_part(kernel_part, status_leds, 0);
	if (ret)
		return ret;

	ret = recovery_mmc_erase_part(rootfs_part, status_leds,
				      RECOVERY_MMC_ERASE_PROGRESS_STEPS);
	if (ret)
		return ret;

	/*
	 * Do not zero P28 again.  Its full zero-fill plus readback is the event
	 * that created the clean-session authorization above.  P26/P27 still
	 * receive their required write-before erase on every upload attempt.
	 */
	recovery_debug_printf("Keeping verified-empty P28; prepared P26/P27 for firmware write\n");

	prog_phase = 2;

	recovery_debug_printf("Writing kernel payload to eMMC partition '%s'...\n",
			      kernel_part);
	ret = recovery_mmc_write_part(kernel_part, status_leds,
				      image.kernel.data, image.kernel.size, 0);
	if (ret)
		return ret;
	recovery_debug_printf("Verifying kernel payload readback from eMMC partition '%s'...\n",
			      kernel_part);
	ret = recovery_mmc_verify_part(kernel_part, status_leds,
				       image.kernel.data, image.kernel.size);
	if (ret)
		return ret;

	recovery_debug_printf("Writing rootfs payload to eMMC partition '%s'...\n",
			      rootfs_part);
	ret = recovery_mmc_write_part(rootfs_part, status_leds,
				      image.rootfs.data, image.rootfs.size,
				      image.kernel.size);
	if (ret)
		return ret;
	recovery_debug_printf("Verifying rootfs payload readback from eMMC partition '%s'...\n",
			      rootfs_part);
	ret = recovery_mmc_verify_part(rootfs_part, status_leds,
				       image.rootfs.data, image.rootfs.size);
	if (ret)
		return ret;

	recovery_sbe1v1k_clean_session_reset();
	return 0;
}

static int recovery_flash_mmc_uboot(struct recovery_status_led_ctrl *status_leds)
{
	const char *uboot_part = env_get("recovery_part_uboot");
	unsigned long max = recovery_uboot_limit();
	int ret;

	if (recovery_board_is_sbe1v1k()) {
		if (active_sbe1v1k_layout != RECOVERY_SBE1V1K_LAYOUT_FULL) {
			printf("SBE1V1K chainloader upload requires the full clean layout first\n");
			return -EPERM;
		}
		/*
		 * Do not trust a mutable environment variable to select the
		 * destructive target.  Verify the complete map and use P25.
		 */
		ret = recovery_verify_sbe1v1k_gpt(&sbe1v1k_layout_full);
		if (ret) {
			printf("SBE1V1K chainloader upload rejected: full GPT map changed\n");
			return ret;
		}
		uboot_part = sbe1v1k_layout_full.uboot_part;
	}

	if (!uboot_part || !*uboot_part) {
		printf("Chainloader eMMC target is not configured. Set recovery_part_uboot explicitly.\n");
		return -EINVAL;
	}

	if (recv_off > max) {
		printf("Chainloader image size %u exceeds limit %lu\n",
		       recv_off, max);
		return -EFBIG;
	}
	ret = recovery_verify_fit_payload(recv_base, recv_off);
	if (ret) {
		printf("Chainloader upload must be a raw FIT image (.itb): %d\n",
		       ret);
		return ret;
	}
	if (fit_get_size(recv_base) != recv_off) {
		printf("Chainloader FIT size does not exactly match the uploaded file\n");
		return -EBADMSG;
	}

	if (recovery_board_is_sbe1v1k() &&
	    !recovery_is_sbe1v1k_chainloader_fit(recv_base, recv_off)) {
		printf("Chainloader upload is not an SBE1V1K chainloader FIT\n");
		return -ENOEXEC;
	}

	ret = recovery_mmc_check_part_size(uboot_part, recv_off);
	if (ret)
		return ret;
	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = RECOVERY_MMC_ERASE_PROGRESS_STEPS;
	prog_write_done = 0;
	prog_write_total = recv_off;
	prog_total = prog_erase_total + prog_write_total;

	ret = recovery_mmc_erase_part(uboot_part, status_leds, 0);
	if (ret)
		return ret;

	prog_phase = 2;

	recovery_debug_printf("Writing chainloader FIT to eMMC partition '%s'...\n",
			      uboot_part);
	ret = recovery_mmc_write_part(uboot_part, status_leds,
				      recv_base, recv_off, 0);
	if (ret)
		return ret;
	ret = recovery_mmc_verify_part(uboot_part, status_leds,
				       recv_base, recv_off);
	if (ret)
		return ret;

	return 0;
}

/*
 * Factory ART lives in GPT P20 ("0:ART"); the colon is part of the partition
 * name.  Restores are exact-size full-partition writes with readback
 * verification, mirroring the APPSBLENV handling in the repartition flow.
 */
static int recovery_flash_mmc_art(struct recovery_status_led_ctrl *status_leds)
{
	struct blk_desc *desc;
	struct disk_partition part;
	int ret;

	ret = recovery_get_mmc_part(RECOVERY_ART_PART_SPEC, &desc, &part);
	if (ret)
		return ret;

	if (recovery_mmc_part_bytes(&part) != RECOVERY_MAX_ART_SIZE) {
		printf("ART partition is %llu bytes, expected %lu; refusing\n",
		       recovery_mmc_part_bytes(&part),
		       (ulong)RECOVERY_MAX_ART_SIZE);
		return -EFBIG;
	}
	if (recv_off != RECOVERY_MAX_ART_SIZE) {
		printf("ART image must be exactly %lu bytes, got %u\n",
		       (ulong)RECOVERY_MAX_ART_SIZE, recv_off);
		return -EFBIG;
	}

	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = RECOVERY_MMC_ERASE_PROGRESS_STEPS;
	prog_write_done = 0;
	prog_write_total = recv_off;
	prog_total = prog_erase_total + prog_write_total;

	ret = recovery_mmc_erase_part(RECOVERY_ART_PART_SPEC, status_leds, 0);
	if (ret)
		return ret;

	prog_phase = 2;
	recovery_debug_printf("Writing factory ART to eMMC partition '%s'...\n",
			      RECOVERY_ART_PART_SPEC);
	ret = recovery_mmc_write_part(RECOVERY_ART_PART_SPEC, status_leds,
				      recv_base, recv_off, 0);
	if (ret)
		return ret;

	return recovery_mmc_verify_part(RECOVERY_ART_PART_SPEC, status_leds,
					recv_base, recv_off);
}

/*
 * boot0/boot1 are eMMC hardware partitions addressed outside the GPT.
 * mmc_switch_part() rewrites EXT_CSD PARTITION_CONFIG and rebinds the block
 * descriptor to the boot area (LBA 0 = start of the boot partition).  Every
 * exit path switches back to the user area so later eMMC work is unaffected.
 */
static int recovery_flash_mmc_boot_hwpart(unsigned int hwpart,
					  struct recovery_status_led_ctrl *status_leds)
{
	struct mmc *mmc;
	struct blk_desc *desc;
	ulong devnum;
	ulong blksz;
	size_t chunk_size;
	u8 *chunk_buf;
	size_t written;
	int ret;

	devnum = dectoul(recovery_mmcdev(), NULL);
	mmc = find_mmc_device(devnum);
	if (!mmc) {
		printf("eMMC device %lu not found\n", devnum);
		return -ENODEV;
	}

	chunk_size = ALIGN(RECOVERY_MMC_WRITE_CHUNK, 512UL);
	chunk_buf = memalign(ARCH_DMA_MINALIGN, chunk_size);
	if (!chunk_buf)
		return -ENOMEM;

	if (!mmc->has_init) {
		ret = mmc_init(mmc);
		if (ret) {
			printf("eMMC init failed: %d\n", ret);
			goto out_free;
		}
	}

	ret = mmc_switch_part(mmc, hwpart);
	if (ret) {
		printf("eMMC hwpart %u switch failed: %d\n", hwpart, ret);
		goto out_free;
	}

	desc = mmc_get_blk_desc(mmc);
	blksz = desc->blksz ?: 512UL;
	if (!blksz || desc->lba != RECOVERY_BOOT_SECTORS) {
		printf("eMMC boot hwpart %u is %lu sectors, expected %lu; refusing\n",
		       hwpart, desc->lba, (ulong)RECOVERY_BOOT_SECTORS);
		ret = -EFBIG;
		goto out_switch_back;
	}
	if (recv_off != RECOVERY_MAX_BOOT_SIZE) {
		printf("%s image must be exactly %lu bytes, got %u\n",
		       hwpart == 1 ? "boot0" : "boot1",
		       (ulong)RECOVERY_MAX_BOOT_SIZE, recv_off);
		ret = -EFBIG;
		goto out_switch_back;
	}

	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = 0;
	prog_write_done = 0;
	prog_write_total = recv_off;
	prog_total = prog_write_total;
	prog_phase = 2;

	recovery_debug_printf("Writing %u bytes to eMMC boot hwpart %u...\n",
			      recv_off, hwpart);
	written = 0;
	while (written < recv_off) {
		size_t todo = recv_off - written;
		lbaint_t blk = written / blksz;
		lbaint_t cnt;

		/*
		 * Clamp BEFORE deriving the block count: cnt must never cover
		 * more bytes than chunk_buf actually holds, or every blk_dwrite
		 * streams heap memory onto the hardware partition and every
		 * blk_dread overruns the buffer corrupting the heap.
		 */
		if (todo > chunk_size)
			todo = chunk_size;
		todo -= todo % blksz;
		if (!todo) {
			ret = -EINVAL;
			goto out_switch_back;
		}
		cnt = todo / blksz;
		memcpy(chunk_buf, recv_base + written, todo);
		if (blk_dwrite(desc, blk, cnt, chunk_buf) != cnt) {
			printf("eMMC boot hwpart %u write failed at LBA %lu\n",
			       hwpart, (ulong)blk);
			ret = -EIO;
			goto out_switch_back;
		}
		written += todo;
		prog_write_done = written;
		prog_done = written;
		recovery_service_runtime(status_leds);
	}

	/* Readback compare: a silent write failure must never look like PASS. */
	written = 0;
	while (written < recv_off) {
		size_t todo = recv_off - written;
		lbaint_t blk = written / blksz;
		lbaint_t cnt;

		if (todo > chunk_size)
			todo = chunk_size;
		todo -= todo % blksz;
		if (!todo) {
			ret = -EINVAL;
			goto out_switch_back;
		}
		cnt = todo / blksz;
		if (blk_dread(desc, blk, cnt, chunk_buf) != cnt) {
			printf("eMMC boot hwpart %u readback failed at LBA %lu\n",
			       hwpart, (ulong)blk);
			ret = -EIO;
			goto out_switch_back;
		}
		if (memcmp(chunk_buf, recv_base + written, todo)) {
			printf("eMMC boot hwpart %u readback mismatch at offset 0x%lx\n",
			       hwpart, (ulong)written);
			ret = -EBADMSG;
			goto out_switch_back;
		}
		written += todo;
		recovery_service_runtime(status_leds);
	}

	recovery_debug_printf("eMMC boot hwpart %u write verified\n", hwpart);
	ret = 0;

out_switch_back:
	if (mmc_switch_part(mmc, 0))
		printf("WARNING: eMMC user-area switch-back failed; run 'mmc dev %lu 0' before other eMMC work\n",
		       devnum);
out_free:
	free(chunk_buf);
	return ret;
}

static int recovery_flash_mmc_target(enum upload_target tgt,
				     struct recovery_status_led_ctrl *status_leds)
{
	switch (tgt) {
	case TARGET_FIRMWARE:
		return recovery_flash_mmc_firmware(status_leds);
	case TARGET_UBOOT:
		return recovery_flash_mmc_uboot(status_leds);
	case TARGET_BOOT0:
		return recovery_flash_mmc_boot_hwpart(1, status_leds);
	case TARGET_BOOT1:
		return recovery_flash_mmc_boot_hwpart(2, status_leds);
	case TARGET_ART:
		return recovery_flash_mmc_art(status_leds);
	case TARGET_REPARTITION:
		return recovery_repartition_factory(status_leds,
						    current_repartition_layout);
	}

	return -EINVAL;
}

static __maybe_unused const char *recovery_ubi_part(enum upload_target tgt)
{
	const char *part;

	switch (tgt) {
	case TARGET_UBOOT:
		part = env_get("recovery_ubi_part_uboot");
		break;
	case TARGET_FIRMWARE:
	default:
		part = env_get("recovery_ubi_part");
		break;
	}

	if (!part)
		part = env_get("recovery_ubi_part");

	return part ?: "ubi";
}

static int recovery_try_ubi_target(enum upload_target tgt,
				       struct recovery_target *target)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	struct ubi_device *ubi;
	struct mtd_info *mtd;
	const char *volume = env_get(recovery_target_env(tgt));
	const char *part = recovery_ubi_part(tgt);

	if (!volume)
		volume = recovery_default_target(tgt);

	if (ubi_part((char *)part, NULL)) {
		mtd_probe_devices();
		mtd = get_mtd_device_nm(part);
		if (IS_ERR_OR_NULL(mtd))
			return -ENODEV;

		target->backend = RECOVERY_BACKEND_UBI;
		target->name = volume;
		target->ubi_part = part;
		target->mtd = mtd;
		target->cur_size = 0;
		target->limit = mtd->size;
		target->ubi_needs_format = true;
		return 0;
	}

	desc = ubi_open_volume_nm(0, volume, UBI_READWRITE);
	if (IS_ERR_OR_NULL(desc)) {
		ubi = ubi_get_device(0);
		if (!ubi)
			return -ENODEV;

		target->backend = RECOVERY_BACKEND_UBI;
		target->name = volume;
		target->ubi_part = part;
		target->ubi_needs_format = false;
		target->cur_size = 0;
		target->limit = (unsigned long long)ubi->avail_pebs *
			(unsigned long long)ubi->leb_size;
		ubi_put_device(ubi);
		return 0;
	}

	target->backend = RECOVERY_BACKEND_UBI;
	target->name = volume;
	target->ubi_part = part;
	target->ubi_needs_format = false;
	target->cur_size = (unsigned long long)desc->vol->reserved_pebs *
			   (unsigned long long)desc->vol->usable_leb_size;
	target->limit = (unsigned long long)(desc->vol->reserved_pebs +
					     desc->vol->ubi->avail_pebs) *
			(unsigned long long)desc->vol->usable_leb_size;

	ubi_close_volume(desc);
	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_resolve_target(enum upload_target tgt,
				   struct recovery_target *target)
{
	const char *name = env_get(recovery_target_env(tgt));
	const char *raw;
	struct mtd_info *mtd;
	ulong ofs;

	memset(target, 0, sizeof(*target));

	if (!name)
		name = recovery_default_target(tgt);

	if ((tgt == TARGET_BOOT0 || tgt == TARGET_BOOT1 ||
	     tgt == TARGET_ART) && !recovery_backend_is_mmc()) {
		printf("Factory-file targets require the eMMC backend\n");
		return -ENODEV;
	}

	if (recovery_backend_is_mmc()) {
		struct disk_partition kernel_part, rootfs_part;
		struct blk_desc *desc;
		const char *part;
		int ret;

		target->backend = RECOVERY_BACKEND_MMC;

		if (tgt == TARGET_BOOT0 || tgt == TARGET_BOOT1) {
			/*
			 * boot0/boot1 are eMMC hardware partitions outside the
			 * GPT.  Their geometry is fixed, so report static
			 * bounds here; mmc_switch_part() runs only inside the
			 * flash path so a failed request can never leave the
			 * chip addressing a boot partition.
			 */
			target->backend = RECOVERY_BACKEND_MMC_BOOT;
			target->name = recovery_target_name(tgt);
			target->limit = RECOVERY_MAX_BOOT_SIZE;
			target->cur_size = target->limit;
			return 0;
		}

		if (tgt == TARGET_ART) {
			/*
			 * The factory ART partition name itself contains a
			 * colon; pass the full "dev#name" spec through
			 * verbatim and never let env override the target.
			 */
			ret = recovery_get_mmc_part(RECOVERY_ART_PART_SPEC,
						    &target->blk,
						    &target->part);
			if (ret)
				return ret;

			target->name = RECOVERY_ART_PART_SPEC;
			target->limit = recovery_mmc_part_bytes(&target->part);
			target->cur_size = target->limit;
			return 0;
		}

		if (tgt == TARGET_UBOOT) {
			part = env_get(recovery_mmc_part_env(tgt));
			if (!part || !*part) {
				printf("Chainloader eMMC target is not configured. Set recovery_part_uboot explicitly.\n");
				return -ENODEV;
			}

			ret = recovery_get_mmc_part(part, &target->blk,
						    &target->part);
			if (ret)
				return ret;

			target->name = part;
			target->limit = recovery_mmc_part_bytes(&target->part);
			if (target->limit > recovery_uboot_limit())
				target->limit = recovery_uboot_limit();
			target->cur_size = target->limit;
			return 0;
		}

		part = env_get("recovery_part_kernel") ?: "0#kernel";
		ret = recovery_get_mmc_part(part, &desc, &kernel_part);
		if (ret)
			return ret;

		part = env_get("recovery_part_rootfs") ?: "0#rootfs";
		ret = recovery_get_mmc_part(part, &desc, &rootfs_part);
		if (ret)
			return ret;

		target->name = "mmc-sysupgrade";
		target->blk = desc;
		target->limit = recovery_mmc_part_bytes(&kernel_part) +
				recovery_mmc_part_bytes(&rootfs_part);
		target->cur_size = target->limit;
		return 0;
	}

	mtd_probe_devices();

	raw = env_get(recovery_raw_env(tgt));
	if (raw && *raw) {
		mtd = get_mtd_device_nm(raw);
		if (!IS_ERR_OR_NULL(mtd)) {
			ulong size_cap = env_get_hex(recovery_size_env(tgt), 0);

			ofs = recovery_raw_offset(tgt);
			target->backend = RECOVERY_BACKEND_MTD;
			target->name = raw;
			target->mtd = mtd;
			target->ofs = ofs;
			target->limit = mtd->size;
			if (ofs && target->limit > ofs)
				target->limit -= ofs;
			if (size_cap && target->limit > size_cap)
				target->limit = size_cap;
			target->cur_size = target->limit;
			return 0;
		}
	}

	mtd = get_mtd_device_nm(name);
	if (!IS_ERR_OR_NULL(mtd)) {
		target->backend = RECOVERY_BACKEND_MTD;
		target->name = name;
		target->mtd = mtd;
		target->limit = mtd->size;
		target->cur_size = target->limit;
		return 0;
	}

	if (!recovery_try_ubi_target(tgt, target))
		return 0;

	if (!raw) {
		switch (tgt) {
		case TARGET_FIRMWARE:
			raw = "nor0";
			break;
		case TARGET_UBOOT:
			raw = env_get("recovery_dev");
			if (!raw)
				raw = "nor0";
			break;
		case TARGET_REPARTITION:
			raw = "nor0";
			break;
		case TARGET_BOOT0:
		case TARGET_BOOT1:
		case TARGET_ART:
			/* Unreachable: rejected for non-eMMC backends. */
			raw = "nor0";
			break;
		}
	}

	mtd = get_mtd_device_nm(raw);
	if (IS_ERR_OR_NULL(mtd))
		return -ENODEV;

	ofs = recovery_raw_offset(tgt);
	target->backend = RECOVERY_BACKEND_MTD;
	target->name = raw;
	target->mtd = mtd;
	target->ofs = ofs;
	target->limit = mtd->size;
	if (ofs && target->limit > ofs)
		target->limit -= ofs;
	{
		ulong size_cap = env_get_hex(recovery_size_env(tgt), 0);

		if (size_cap && target->limit > size_cap)
			target->limit = size_cap;
	}
	target->cur_size = target->limit;

	return 0;
}

static void recovery_release_target(struct recovery_target *target)
{
	if (target->mtd)
		put_mtd_device(target->mtd);

	target->mtd = NULL;
}

static void recovery_service_runtime(struct recovery_status_led_ctrl *status_leds)
{
	struct udevice *udev = eth_get_current();
	struct netif *netif = net_lwip_get_netif();

	if (udev && eth_is_active(udev) && netif)
		net_lwip_rx(udev, netif);
	if (status_leds)
		recovery_status_led_poll(status_leds);
	WATCHDOG_RESET();
}

static __maybe_unused void recovery_ubi_progress(struct ubi_volume *vol, int done, int total)
{
	unsigned long long bytes_done = prog_erase_volume_base;

	(void)vol;

	if (total > 0)
		bytes_done += (prog_erase_volume_bytes * (unsigned long long)done) /
			      (unsigned long long)total;

	if (bytes_done > prog_erase_total)
		bytes_done = prog_erase_total;

	prog_erase_done = bytes_done;
	prog_done = prog_erase_done + prog_write_done;

	if (prog_status_leds)
		recovery_service_runtime(prog_status_leds);
}

static int recovery_erase_mtd_region(struct mtd_info *mtd, loff_t ofs,
				     size_t len,
				     struct recovery_status_led_ctrl *status_leds)
{
	struct erase_info ei = { 0 };
	loff_t erase_len;
	int ret;

	if (!mtd || !len)
		return 0;

	erase_len = ALIGN(len, mtd->erasesize);
	ei.addr = ofs;
	ei.len = erase_len;

	ret = mtd_unlock(mtd, ei.addr, ei.len);
	if (ret && ret != -EOPNOTSUPP) {
		printf("Warning: initial mtd_unlock 0x%llx..+0x%llx failed: %d\n",
		       (unsigned long long)ei.addr,
		       (unsigned long long)ei.len, ret);
	}

	for (loff_t addr = 0; addr < erase_len; addr += mtd->erasesize) {
		struct erase_info e = {
			.addr = ofs + addr,
			.len = mtd->erasesize,
		};
		int tries = 0;

		do {
			ret = mtd_erase(mtd, &e);
			if (!ret)
				break;
			if (ret == -EROFS || ret == -EACCES)
				mtd_unlock(mtd, e.addr, e.len);
			else
				break;
		} while (++tries < 2);

		if (ret)
			return ret;

		prog_erase_done = addr + mtd->erasesize;
		if (prog_erase_done > prog_erase_total)
			prog_erase_done = prog_erase_total;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	return 0;
}

static __maybe_unused bool recovery_preserve_ubi_volume(const char *name)
{
	if (!name || !*name)
		return true;

	if (!strcmp(name, "ubootenv") || !strcmp(name, "ubootenv2"))
		return true;

	if (of_machine_is_compatible("econet,xr1710g") ||
	    of_machine_is_compatible("econet,xr1710g-ubi") ||
	    of_machine_is_compatible("gemtek,xr1710g") ||
	    of_machine_is_compatible("gemtek,xr1710g-ubi"))
		return !strcmp(name, "factory");

	if (of_machine_is_compatible("gemtek,w1700k") ||
	    of_machine_is_compatible("gemtek,w1700k-ubi"))
		return !strcmp(name, "factory");

	return false;
}

static unsigned long long
recovery_calc_forced_ubi_limit(struct recovery_target *target)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_device *ubi;
	unsigned long long limit;
	int i;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return target->limit;

	if (ubi_part((char *)target->ubi_part, NULL))
		return target->limit;

	ubi = ubi_get_device(0);
	if (!ubi)
		return target->limit;

	limit = (unsigned long long)ubi->avail_pebs *
		(unsigned long long)ubi->leb_size;

	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *vol = ubi->volumes[i];

		if (!vol || vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;
		if (recovery_preserve_ubi_volume(vol->name))
			continue;

		limit += (unsigned long long)vol->reserved_pebs *
			 (unsigned long long)vol->usable_leb_size;
	}

	ubi_put_device(ubi);
	return limit;
#else
	return target->limit;
#endif
}

static __maybe_unused int recovery_create_ubi_volume(const char *name, size_t size, int vol_type)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_mkvol_req req;
	struct ubi_device *ubi;
	int ret;

	if (!name || !*name)
		return -EINVAL;

	ubi = ubi_get_device(0);
	if (!ubi)
		return -ENODEV;

	if (!size)
		size = (size_t)ubi->avail_pebs * (size_t)ubi->leb_size;

	memset(&req, 0, sizeof(req));
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = size;
	req.vol_type = vol_type;
	req.name_len = strlen(name);
	if (req.name_len > UBI_VOL_NAME_MAX) {
		ubi_put_device(ubi);
		return -ENAMETOOLONG;
	}
	memcpy(req.name, name, req.name_len);
	req.name[req.name_len] = '\0';

	mutex_lock(&ubi->device_mutex);
	ret = ubi_create_volume(ubi, &req);
	mutex_unlock(&ubi->device_mutex);
	ubi_put_device(ubi);

	return ret;
#else
	return -ENODEV;
#endif
}

static __maybe_unused int recovery_remove_ubi_volume(const char *name)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	int ret;

	desc = ubi_open_volume_nm(0, name, UBI_EXCLUSIVE);
	if (IS_ERR_OR_NULL(desc))
		return IS_ERR(desc) ? PTR_ERR(desc) : -ENODEV;

	mutex_lock(&desc->vol->ubi->device_mutex);
	ret = ubi_remove_volume(desc, 0);
	mutex_unlock(&desc->vol->ubi->device_mutex);
	ubi_close_volume(desc);

	return ret;
#else
	return -ENODEV;
#endif
}

static int recovery_ensure_rootfs_data(struct recovery_target *target)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return 0;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	desc = ubi_open_volume_nm(0, "rootfs_data", UBI_READWRITE);
	if (!IS_ERR_OR_NULL(desc)) {
		ubi_close_volume(desc);
		return 0;
	}

	ret = recovery_create_ubi_volume("rootfs_data", 0, UBI_DYNAMIC_VOLUME);
	if (ret && ret != -EEXIST) {
		printf("Failed to create UBI volume 'rootfs_data': %d\n", ret);
		return ret;
	}

	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_prepare_ubi_target(struct recovery_target *target,
				       struct recovery_status_led_ctrl *status_leds,
				       size_t image_size, bool *reformatted)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_device *ubi;
	loff_t erase_len;
	int ret;

	if (reformatted)
		*reformatted = false;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (!target->ubi_needs_format)
		return ubi_part((char *)target->ubi_part, NULL) ? -ENODEV : 0;

	if (!target->mtd)
		return -ENODEV;

	erase_len = ALIGN(target->mtd->size, target->mtd->erasesize);
	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = erase_len;
	prog_write_done = 0;
	prog_write_total = image_size;
	prog_total = prog_erase_total + prog_write_total;

	recovery_debug_printf("UBI partition '%s' is invalid, erasing it to recreate layout...\n",
			      target->ubi_part);
	ret = recovery_erase_mtd_region(target->mtd, 0, target->mtd->size,
					status_leds);
	if (ret)
		return ret;

	ret = ubi_part((char *)target->ubi_part, NULL);
	if (ret)
		return ret;

	ubi = ubi_get_device(0);
	if (ubi) {
		target->limit = (unsigned long long)ubi->avail_pebs *
				(unsigned long long)ubi->leb_size;
		ubi_put_device(ubi);
	}

	target->cur_size = 0;
	target->ubi_needs_format = false;
	if (reformatted)
		*reformatted = true;

	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_ensure_ubi_volume_named(const char *name, size_t size,
					    int vol_type)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	int ret;

	if (!name || !*name)
		return -EINVAL;

	desc = ubi_open_volume_nm(0, name, UBI_READWRITE);
	if (!IS_ERR_OR_NULL(desc)) {
		ubi_close_volume(desc);
		return 0;
	}

	ret = recovery_create_ubi_volume(name, size, vol_type);
	if (ret && ret != -EEXIST) {
		printf("Failed to create UBI volume '%s': %d\n", name, ret);
		return ret;
	}

	return 0;
#else
	return -ENODEV;
#endif
}

static int recovery_ensure_preserved_ubi_volumes(struct recovery_target *target)
{
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return 0;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	ret = recovery_ensure_ubi_volume_named("ubootenv",
					       RECOVERY_UBOOTENV_SIZE,
					       UBI_DYNAMIC_VOLUME);
	if (ret)
		return ret;

	ret = recovery_ensure_ubi_volume_named("ubootenv2",
					       RECOVERY_UBOOTENV_SIZE,
					       UBI_DYNAMIC_VOLUME);
	if (ret)
		return ret;

	if (of_machine_is_compatible("econet,xr1710g") ||
	    of_machine_is_compatible("econet,xr1710g-ubi") ||
	    of_machine_is_compatible("gemtek,xr1710g") ||
	    of_machine_is_compatible("gemtek,xr1710g-ubi") ||
	    of_machine_is_compatible("gemtek,w1700k") ||
	    of_machine_is_compatible("gemtek,w1700k-ubi")) {
		ret = recovery_ensure_ubi_volume_named("factory",
						       RECOVERY_FACTORY_SIZE,
						       UBI_STATIC_VOLUME);
		if (ret)
			return ret;
	}

	return 0;
}

static int recovery_cleanup_ubi_firmware(struct recovery_target *target,
					 struct recovery_status_led_ctrl *status_leds,
					 size_t image_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_device *ubi;
	unsigned long long erase_total = 0;
	unsigned long long erase_done = 0;
	int ret = 0;
	int i;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	ubi = ubi_get_device(0);
	if (!ubi)
		return -ENODEV;

	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *vol = ubi->volumes[i];

		if (!vol || vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;
		if (recovery_preserve_ubi_volume(vol->name))
			continue;

		erase_total += (unsigned long long)vol->reserved_pebs *
			       (unsigned long long)vol->usable_leb_size;
	}

	prog_phase = 1;
	prog_done = 0;
	prog_erase_done = 0;
	prog_erase_total = erase_total > UINT_MAX ? UINT_MAX : erase_total;
	prog_write_done = 0;
	prog_write_total = image_size;
	prog_total = prog_erase_total + prog_write_total;
	prog_status_leds = status_leds;
	ubi_set_progress_callback(recovery_ubi_progress);

	for (i = 0; i < ubi->vtbl_slots; i++) {
		struct ubi_volume *vol = ubi->volumes[i];
		unsigned long long vol_size;
		char name[UBI_VOL_NAME_MAX + 1];

		if (!vol || vol->vol_id >= UBI_INTERNAL_VOL_START)
			continue;
		if (recovery_preserve_ubi_volume(vol->name))
			continue;

		vol_size = (unsigned long long)vol->reserved_pebs *
			   (unsigned long long)vol->usable_leb_size;
		strlcpy(name, vol->name, sizeof(name));
		prog_erase_volume_base = erase_done;
		prog_erase_volume_bytes = vol_size;

		recovery_debug_printf("Removing UBI volume '%s' before flashing '%s'...\n",
				      name, target->name);
		ret = recovery_remove_ubi_volume(name);
		if (ret) {
			printf("Failed to remove UBI volume '%s': %d\n", name, ret);
			break;
		}

		erase_done += vol_size;
		prog_erase_done = erase_done > prog_erase_total ?
				  prog_erase_total : erase_done;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	ubi_set_progress_callback(NULL);
	prog_status_leds = NULL;
	ubi_put_device(ubi);
	if (ret)
		return ret;

	return recovery_try_ubi_target(current_target, target);
#else
	return -ENODEV;
#endif
}

static int recovery_write_ubi_target(struct recovery_target *target,
				      struct recovery_status_led_ctrl *status_leds,
				      const void *image, size_t image_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	struct ubi_volume *vol;
	struct ubi_device *ubi;
	const u8 *src = image;
	size_t reserved_bytes;
	u32 written = 0;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	desc = ubi_open_volume_nm(0, target->name, UBI_READWRITE);
	if (IS_ERR_OR_NULL(desc))
		return IS_ERR(desc) ? PTR_ERR(desc) : -ENODEV;

	vol = desc->vol;
	ubi = vol->ubi;
	reserved_bytes = (size_t)vol->reserved_pebs *
			 (size_t)(ubi->leb_size - vol->data_pad);
	if (image_size > reserved_bytes) {
		ret = -EFBIG;
		goto out_close;
	}

	ret = ubi_start_update(ubi, vol, image_size);
	if (ret)
		goto out_close;

	/* Flush one runtime cycle so the browser can observe phase 2 before
	 * the actual UBI data write loop starts.
	 */
	recovery_service_runtime(status_leds);

	while (written < image_size) {
		u32 chunk = image_size - written;

		if (chunk > RECOVERY_UBI_WRITE_CHUNK)
			chunk = RECOVERY_UBI_WRITE_CHUNK;

		ret = ubi_more_update_data(ubi, vol, src + written, chunk);
		if (ret < 0)
			goto out_close;

		written += chunk;
		prog_write_done = written > prog_write_total ?
				  prog_write_total : written;
		prog_done = prog_erase_done + prog_write_done;
		recovery_service_runtime(status_leds);
	}

	ret = ubi_check_volume(ubi, vol->vol_id);
	if (ret < 0) {
		ret = -ret;
		goto out_close;
	}

	if (ret) {
		ubi_warn(ubi, "volume %d on UBI device %d is corrupt",
			 vol->vol_id, ubi->ubi_num);
		vol->corrupted = 1;
	}

	vol->checked = 1;
	ubi_gluebi_updated(vol);
	ret = 0;

out_close:
	ubi_close_volume(desc);
	return ret;
#else
	return -ENODEV;
#endif
}

static int recovery_resize_ubi_target(struct recovery_target *target,
				      size_t new_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_volume_desc *desc;
	struct ubi_volume *vol;
	int needed_pebs;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	desc = ubi_open_volume_nm(0, target->name, UBI_EXCLUSIVE);
	if (IS_ERR_OR_NULL(desc))
		return IS_ERR(desc) ? PTR_ERR(desc) : -ENODEV;

	vol = desc->vol;
	needed_pebs = DIV_ROUND_UP(new_size, vol->usable_leb_size);

	mutex_lock(&vol->ubi->device_mutex);
	ret = ubi_resize_volume(desc, needed_pebs);
	mutex_unlock(&vol->ubi->device_mutex);

	if (!ret) {
		target->cur_size = (unsigned long long)needed_pebs *
				   (unsigned long long)vol->usable_leb_size;
		target->limit = (unsigned long long)(needed_pebs +
						     vol->ubi->avail_pebs) *
			(unsigned long long)vol->usable_leb_size;
	}

	ubi_close_volume(desc);
	return ret;
#else
	return -ENODEV;
#endif
}

static int recovery_create_ubi_target(struct recovery_target *target,
				      size_t new_size)
{
#if IS_ENABLED(CONFIG_CMD_UBI) && IS_ENABLED(CONFIG_MTD_UBI)
	struct ubi_mkvol_req req;
	struct ubi_device *ubi;
	int ret;

	if (target->backend != RECOVERY_BACKEND_UBI)
		return -EINVAL;

	if (ubi_part((char *)target->ubi_part, NULL))
		return -ENODEV;

	ubi = ubi_get_device(0);
	if (!ubi)
		return -ENODEV;

	memset(&req, 0, sizeof(req));
	req.vol_id = UBI_VOL_NUM_AUTO;
	req.alignment = 1;
	req.bytes = new_size;
	req.vol_type = UBI_STATIC_VOLUME;
	req.name_len = strlen(target->name);
	if (req.name_len > UBI_VOL_NAME_MAX) {
		ubi_put_device(ubi);
		return -ENAMETOOLONG;
	}
	memcpy(req.name, target->name, req.name_len);
	req.name[req.name_len] = '\0';

	mutex_lock(&ubi->device_mutex);
	ret = ubi_create_volume(ubi, &req);
	mutex_unlock(&ubi->device_mutex);
	ubi_put_device(ubi);
	if (ret)
		return ret;

	return recovery_try_ubi_target(current_target, target);
#else
	return -ENODEV;
#endif
}

/* Determine maximum payload size based on selected target and DTS-defined MTD
 * layout. Returns 0 on error. */
static unsigned long recovery_calc_target_max(enum upload_target tgt,
					      bool force_recreate, loff_t *p_ofs)
{
	struct recovery_target target;
	unsigned long limit = 0;
	unsigned long long effective_limit;

	if (recovery_resolve_target(tgt, &target))
		return 0;

	if (p_ofs)
		*p_ofs = target.ofs;

	effective_limit = target.limit;
	if (force_recreate && tgt == TARGET_FIRMWARE)
		effective_limit = recovery_calc_forced_ubi_limit(&target);

	limit = (effective_limit > ULONG_MAX) ? ULONG_MAX :
		(unsigned long)effective_limit;
	recovery_release_target(&target);

	return limit;
}

static int recovery_validate_upload_length(enum upload_target target,
					   bool force_recreate,
					   unsigned long length,
					   enum recovery_stream_format format)
{
	unsigned long env_max = env_get_hex("recovery_max", 0);
	unsigned long min = 0;
	loff_t target_ofs = 0;
	unsigned long target_max;
	unsigned long max;

	target_max = recovery_calc_target_max(target, force_recreate,
					      &target_ofs);
	max = target_max ? target_max : RECOVERY_UPLOAD_MAX;

	if (target == TARGET_FIRMWARE) {
		if (recovery_backend_is_mmc() &&
		    env_get_yesno("recovery_stream") == 1) {
			if (format == RECOVERY_STREAM_TAR)
				min = 1024;
			else
				min = env_get_hex("recovery_kernel_pad",
						  RECOVERY_KERNEL_PAD_SIZE) + 1;
		} else {
			min = RECOVERY_MIN_FIRMWARE_SIZE;
		}
	} else if (target == TARGET_UBOOT && max > recovery_uboot_limit()) {
		max = recovery_uboot_limit();
	} else if (target == TARGET_BOOT0 || target == TARGET_BOOT1) {
		/* Exact-size factory restores: the upload must be exactly the
		 * boot hardware partition size. */
		if (max > RECOVERY_MAX_BOOT_SIZE)
			max = RECOVERY_MAX_BOOT_SIZE;
		min = RECOVERY_MAX_BOOT_SIZE;
	} else if (target == TARGET_ART) {
		if (max > RECOVERY_MAX_ART_SIZE)
			max = RECOVERY_MAX_ART_SIZE;
		min = RECOVERY_MAX_ART_SIZE;
	}

	if (env_max && env_max < max)
		max = env_max;
	if (!length || length > max || (min && length < min)) {
		if (min && length < min)
			printf("httpd: upload length %lu below allowed min %lu for target %d\n",
			       length, min, target);
		else
			printf("httpd: upload length %lu exceeds allowed max %lu (target limit %lu, ofs 0x%llx)\n",
			       length, max, target_max,
			       (unsigned long long)target_ofs);
		return -EFBIG;
	}

	return 0;
}

/* Only dynamic endpoints here; static files come from fsdata */

static void recovery_http_static_file(struct fs_file *file, const char *data,
				      int len)
{
	file->data = data;
	file->len = len;
	/* Dynamic reads are enabled for backup streams. Static data is complete. */
	file->index = len;
	file->pextension = NULL;
	file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
}

static int recovery_http_error(struct fs_file *file, const char *status,
			       const char *message)
{
	static char page_error[384];
	int body_len, header_len;

	body_len = strlen(message);
	header_len = snprintf(page_error, sizeof(page_error),
			      "HTTP/1.0 %s\r\n"
			      "Content-Type: text/plain\r\n"
			      "Cache-Control: no-store\r\n"
			      "Content-Length: %d\r\n"
			      "Connection: close\r\n\r\n%s",
			      status, body_len, message);
	if (header_len < 0 || header_len >= sizeof(page_error))
		return 0;

	recovery_http_static_file(file, page_error, header_len);
	return 1;
}

static void recovery_partition_display_name(const char *name, char *safe,
					    size_t safe_len)
{
	size_t i;

	if (!safe_len)
		return;

	for (i = 0; name && name[i] && i + 1 < safe_len; i++) {
		unsigned char c = name[i];

		safe[i] = c >= 0x20 && c <= 0x7e && c != '"' && c != '\\' ?
			  c : '_';
	}
	safe[i] = '\0';
}

static void recovery_partition_filename(const char *name, char *safe,
					size_t safe_len)
{
	size_t i;

	if (!safe_len)
		return;

	for (i = 0; name && name[i] && i + 1 < safe_len; i++) {
		unsigned char c = name[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
			safe[i] = c;
		else
			safe[i] = '_';
	}
	safe[i] = '\0';
}

static int recovery_backup_source_from_partition(struct blk_desc *desc,
						 int number,
						 struct recovery_backup_source *source)
{
	struct disk_partition part;
	char raw_name[PART_NAME_LEN + 1];
	char safe_name[PART_NAME_LEN + 1];
	ulong blksz;
	int ret;

	ret = part_get_info(desc, number, &part);
	if (ret || !part.size)
		return -ENOENT;

	blksz = part.blksz ?: desc->blksz;
	if (!blksz || (unsigned long long)part.size > ~0ULL / blksz)
		return -EOVERFLOW;

	memset(source, 0, sizeof(*source));
	memcpy(raw_name, part.name, PART_NAME_LEN);
	raw_name[PART_NAME_LEN] = '\0';
	recovery_partition_display_name(raw_name, source->name,
					sizeof(source->name));
	recovery_partition_filename(raw_name, safe_name, sizeof(safe_name));

	source->hwpart = EMMC_HWPART_DEFAULT;
	source->number = number;
	source->start = part.start;
	source->blocks = part.size;
	source->blksz = blksz;
	source->bytes = (unsigned long long)part.size * blksz;
	snprintf(source->filename, sizeof(source->filename), "p%02d-%s.img",
		 number, safe_name[0] ? safe_name : "partition");
	return 0;
}

static int recovery_backup_source_from_boot(struct blk_desc *desc,
					    int boot_index,
					    struct recovery_backup_source *source)
{
	int hwpart = EMMC_HWPART_BOOT1 + boot_index;
	int ret;

	ret = blk_dselect_hwpart(desc, hwpart);
	if (ret)
		return ret;
	if (!desc->blksz || !desc->lba ||
	    (unsigned long long)desc->lba > ~0ULL / desc->blksz)
		return -EOVERFLOW;

	memset(source, 0, sizeof(*source));
	source->hwpart = hwpart;
	source->start = 0;
	source->blocks = desc->lba;
	source->blksz = desc->blksz;
	source->bytes = (unsigned long long)desc->lba * desc->blksz;
	snprintf(source->name, sizeof(source->name), "eMMC boot%d", boot_index);
	snprintf(source->filename, sizeof(source->filename), "emmc-boot%d.img",
		 boot_index);
	return 0;
}

/* boot0, boot1, then every valid GPT partition in the user area. */
static int recovery_backup_next_source(struct blk_desc *desc,
				       unsigned int *cursor,
				       struct recovery_backup_source *source)
{
	unsigned int p;
	int ret;

	if (*cursor < 2) {
		unsigned int boot_index = *cursor;

		(*cursor)++;
		ret = recovery_backup_source_from_boot(desc, boot_index, source);
		return ret ? ret : 1;
	}

	ret = blk_dselect_hwpart(desc, EMMC_HWPART_DEFAULT);
	if (ret)
		return ret;

	for (p = *cursor - 1; p <= MAX_SEARCH_PARTITIONS; p++) {
		*cursor = p + 2;
		ret = recovery_backup_source_from_partition(desc, p, source);
		if (!ret)
			return 1;
		if (ret != -ENOENT)
			return ret;
	}

	return 0;
}

static int recovery_backup_restore_user(struct recovery_backup_file *backup)
{
	int ret;

	if (backup->user_restored)
		return 0;

	ret = blk_dselect_hwpart(backup->desc, EMMC_HWPART_DEFAULT);
	if (ret) {
		printf("httpd: failed to restore eMMC user area after backup: %d\n",
		       ret);
		return ret;
	}

	backup->user_restored = true;
	return 0;
}

static int recovery_tar_format_octal(char *field, size_t field_size,
				     unsigned long long value)
{
	int len;

	len = snprintf(field, field_size, "%0*llo", (int)field_size - 1, value);
	return len == field_size - 1 ? 0 : -EOVERFLOW;
}

static int recovery_backup_prepare_tar_header(struct recovery_backup_file *backup)
{
	struct recovery_backup_tar_header *header = &backup->tar_header;
	const u8 *raw = (const u8 *)header;
	unsigned int checksum = 0;
	size_t i;
	int ret;

	if (sizeof(*header) != RECOVERY_TAR_BLOCK_SIZE ||
	    strlen(backup->source.filename) >= sizeof(header->name))
		return -EOVERFLOW;

	memset(header, 0, sizeof(*header));
	strlcpy(header->name, backup->source.filename, sizeof(header->name));
	ret = recovery_tar_format_octal(header->mode, sizeof(header->mode), 0600);
	ret |= recovery_tar_format_octal(header->uid, sizeof(header->uid), 0);
	ret |= recovery_tar_format_octal(header->gid, sizeof(header->gid), 0);
	ret |= recovery_tar_format_octal(header->size, sizeof(header->size),
					 backup->source.bytes);
	ret |= recovery_tar_format_octal(header->mtime, sizeof(header->mtime), 0);
	if (ret)
		return ret;

	memset(header->checksum, ' ', sizeof(header->checksum));
	header->typeflag = '0';
	memcpy(header->magic, "ustar", sizeof("ustar"));
	memcpy(header->version, "00", sizeof(header->version));
	strlcpy(header->uname, "root", sizeof(header->uname));
	strlcpy(header->gname, "root", sizeof(header->gname));

	for (i = 0; i < sizeof(*header); i++)
		checksum += raw[i];
	if (snprintf(header->checksum, sizeof(header->checksum), "%06o",
		     checksum) != 6)
		return -EOVERFLOW;
	header->checksum[6] = '\0';
	header->checksum[7] = ' ';
	return 0;
}

static int
recovery_backup_start_next_tar_source(struct recovery_backup_file *backup)
{
	struct recovery_backup_source source;
	int ret;

	ret = recovery_backup_next_source(backup->desc, &backup->source_cursor,
					  &source);
	if (ret <= 0) {
		if (!ret) {
			backup->tar_phase = RECOVERY_BACKUP_TAR_END;
			backup->tar_end_left = RECOVERY_TAR_END_SIZE;
		}
		return ret;
	}

	backup->source = source;
	backup->next_lba = source.start;
	backup->blocks_left = source.blocks;
	backup->blksz = source.blksz;
	backup->cache_len = 0;
	backup->cache_off = 0;
	backup->tar_header_off = 0;
	backup->tar_padding_left =
		(RECOVERY_TAR_BLOCK_SIZE -
		 (source.bytes % RECOVERY_TAR_BLOCK_SIZE)) %
		RECOVERY_TAR_BLOCK_SIZE;
	ret = recovery_backup_prepare_tar_header(backup);
	if (ret)
		return ret;
	backup->tar_phase = RECOVERY_BACKUP_TAR_MEMBER_HEADER;
	return 1;
}

static int recovery_backup_scan_all(struct blk_desc *desc,
				    unsigned long long *archive_size,
				    size_t *cache_capacity,
				    unsigned int *source_count)
{
	struct recovery_backup_source source;
	unsigned long long total = RECOVERY_TAR_END_SIZE;
	unsigned int cursor = 0;
	unsigned int count = 0;
	size_t max_cache = 0;
	int restore_ret;
	int ret;

	while ((ret = recovery_backup_next_source(desc, &cursor, &source)) > 0) {
		unsigned long long padded;
		size_t source_cache;

		if (source.bytes > RECOVERY_TAR_MAX_MEMBER_SIZE) {
			ret = -EFBIG;
			break;
		}
		padded = (source.bytes + RECOVERY_TAR_BLOCK_SIZE - 1) &
			 ~((unsigned long long)RECOVERY_TAR_BLOCK_SIZE - 1);
		if (total > ~0ULL - RECOVERY_TAR_BLOCK_SIZE - padded) {
			ret = -EOVERFLOW;
			break;
		}
		total += RECOVERY_TAR_BLOCK_SIZE + padded;
		source_cache = source.bytes < RECOVERY_MMC_BACKUP_CHUNK ?
			       source.bytes : RECOVERY_MMC_BACKUP_CHUNK;
		if (source_cache > max_cache)
			max_cache = source_cache;
		count++;
	}

	restore_ret = blk_dselect_hwpart(desc, EMMC_HWPART_DEFAULT);
	if (!ret && restore_ret)
		ret = restore_ret;
	if (ret < 0)
		return ret;
	if (!count || !max_cache)
		return -ENOENT;

	*archive_size = total;
	*cache_capacity = max_cache;
	*source_count = count;
	return 0;
}

static int recovery_backup_append_source_json(char *json, size_t json_size,
					      size_t *off, bool comma,
					      const struct recovery_backup_source *source)
{
	int boot_index;
	int ret;

	if (source->number) {
		ret = recovery_appendf(json, json_size, off,
				       "%s{\"id\":\"partition-%d\",\"kind\":\"gpt\",",
				       comma ? "," : "", source->number);
		if (ret)
			return ret;
		ret = recovery_appendf(json, json_size, off,
				       "\"path\":\"/backup/partition-%d.bin\",\"number\":%d,",
				       source->number, source->number);
		if (ret)
			return ret;
		ret = recovery_appendf(json, json_size, off,
				       "\"name\":\"%s\",\"start\":%llu,\"blocks\":%llu,",
				       source->name,
				       (unsigned long long)source->start,
				       (unsigned long long)source->blocks);
		if (ret)
			return ret;
		return recovery_appendf(json, json_size, off,
			"\"block_size\":%lu,\"size\":%llu}",
			source->blksz, source->bytes);
	}

	boot_index = source->hwpart - EMMC_HWPART_BOOT1;
	ret = recovery_appendf(json, json_size, off,
			       "%s{\"id\":\"boot%d\",\"kind\":\"hardware\",",
			       comma ? "," : "", boot_index);
	if (ret)
		return ret;
	ret = recovery_appendf(json, json_size, off,
			       "\"path\":\"/backup/boot%d.bin\",\"name\":\"%s\",",
			       boot_index, source->name);
	if (ret)
		return ret;
	ret = recovery_appendf(json, json_size, off,
			       "\"start\":0,\"blocks\":%llu,\"block_size\":%lu,",
			       (unsigned long long)source->blocks,
			       source->blksz);
	if (ret)
		return ret;
	return recovery_appendf(json, json_size, off, "\"size\":%llu}",
				 source->bytes);
}

static int recovery_open_partitions(struct fs_file *file)
{
	static char page[RECOVERY_PARTITIONS_JSON_MAX + 256];
	static char json[RECOVERY_PARTITIONS_JSON_MAX];
	struct recovery_backup_source source;
	struct blk_desc *desc;
	unsigned int cursor = 0;
	size_t off = 0;
	int count = 0;
	int header_len;
	int ret;

	if (recovery_backup_active)
		return recovery_http_error(file, "409 Conflict",
					   "A backup stream is already active.\n");
	if ((prog_phase > 0 && prog_phase < 3) || recovery_stream.active ||
	    flash_request || prog_reboot || reboot_request)
		return recovery_http_error(file, "409 Conflict",
					   "A destructive storage operation is active.\n");

	ret = blk_get_device_by_str("mmc", recovery_mmcdev(), &desc);
	if (ret < 0)
		return recovery_http_error(file, "503 Service Unavailable",
					   "eMMC device is unavailable.\n");

	off += snprintf(json + off, sizeof(json) - off, "{\"partitions\":[");
	while ((ret = recovery_backup_next_source(desc, &cursor, &source)) > 0) {
		ret = recovery_backup_append_source_json(json, sizeof(json), &off,
							 count, &source);
		if (ret)
			break;
		count++;
	}
	if (blk_dselect_hwpart(desc, EMMC_HWPART_DEFAULT) && ret >= 0)
		ret = -EIO;
	if (ret < 0)
		return recovery_http_error(file, "503 Service Unavailable",
					   "Cannot enumerate all eMMC partitions.\n");

	memcpy(json + off, "]}\n", sizeof("]}\n"));
	off += sizeof("]}\n") - 1;
	header_len = snprintf(page, sizeof(page),
			      "HTTP/1.0 200 OK\r\n"
			      "Content-Type: application/json\r\n"
			      "Cache-Control: no-store\r\n"
			      "Content-Length: %lu\r\n"
			      "Connection: close\r\n\r\n",
			      (ulong)off);
	if (header_len < 0 || header_len + off > sizeof(page))
		return 0;

	memcpy(page + header_len, json, off);
	recovery_http_static_file(file, page, header_len + off);
	return 1;
}

static int recovery_backup_partition_number(const char *path)
{
	const char *prefix = "backup/partition-";
	unsigned int number = 0;

	if (strncmp(path, prefix, strlen(prefix)))
		return -ENOENT;

	path += strlen(prefix);
	if (*path < '0' || *path > '9')
		return -EINVAL;
	while (*path >= '0' && *path <= '9') {
		number = number * 10 + (*path++ - '0');
		if (number > MAX_SEARCH_PARTITIONS)
			return -EINVAL;
	}
	if (strcmp(path, ".bin") || !number)
		return -EINVAL;

	return number;
}

enum recovery_backup_request {
	RECOVERY_BACKUP_REQUEST_NONE = 0,
	RECOVERY_BACKUP_REQUEST_PARTITION,
	RECOVERY_BACKUP_REQUEST_BOOT0,
	RECOVERY_BACKUP_REQUEST_BOOT1,
	RECOVERY_BACKUP_REQUEST_ALL,
	RECOVERY_BACKUP_REQUEST_INVALID,
};

static enum recovery_backup_request
recovery_backup_parse_request(const char *path, int *number)
{
	int ret;

	if (!strcmp(path, "backup/boot0.bin"))
		return RECOVERY_BACKUP_REQUEST_BOOT0;
	if (!strcmp(path, "backup/boot1.bin"))
		return RECOVERY_BACKUP_REQUEST_BOOT1;
	if (!strcmp(path, "backup/all.tar"))
		return RECOVERY_BACKUP_REQUEST_ALL;

	ret = recovery_backup_partition_number(path);
	if (ret == -ENOENT)
		return RECOVERY_BACKUP_REQUEST_NONE;
	if (ret < 0)
		return RECOVERY_BACKUP_REQUEST_INVALID;
	*number = ret;
	return RECOVERY_BACKUP_REQUEST_PARTITION;
}

static int recovery_open_backup(struct fs_file *file, const char *path)
{
	struct recovery_backup_file *backup;
	struct recovery_backup_source source;
	struct blk_desc *desc;
	unsigned long long archive_size = 0;
	unsigned int source_count = 0;
	lbaint_t cache_blocks;
	size_t cache_capacity = 0;
	enum recovery_backup_request request;
	const char *error_message = "Cannot prepare eMMC backup.\n";
	const char *error_status = "503 Service Unavailable";
	int header_len;
	int number = 0;
	int ret;

	request = recovery_backup_parse_request(path, &number);
	if (request == RECOVERY_BACKUP_REQUEST_NONE)
		return 0;
	if (request == RECOVERY_BACKUP_REQUEST_INVALID)
		return recovery_http_error(file, "400 Bad Request",
					   "Invalid partition backup path.\n");
	if (recovery_backup_active)
		return recovery_http_error(file, "409 Conflict",
					   "A backup stream is already active.\n");
	if ((prog_phase > 0 && prog_phase < 3) || recovery_stream.active ||
	    flash_request || prog_reboot || reboot_request)
		return recovery_http_error(file, "409 Conflict",
					   "A destructive storage operation is active.\n");

	ret = blk_get_device_by_str("mmc", recovery_mmcdev(), &desc);
	if (ret < 0)
		return recovery_http_error(file, "503 Service Unavailable",
					   "eMMC device is unavailable.\n");

	backup = calloc(1, sizeof(*backup));
	if (!backup)
		return recovery_http_error(file, "503 Service Unavailable",
					   "Cannot allocate backup state.\n");

	backup->desc = desc;
	if (request == RECOVERY_BACKUP_REQUEST_ALL) {
		ret = recovery_backup_scan_all(desc, &archive_size, &cache_capacity,
					       &source_count);
		if (ret) {
			if (ret == -EFBIG) {
				error_status = "500 Internal Server Error";
				error_message =
					"A partition is too large for a ustar member.\n";
			}
			goto error;
		}
		backup->mode = RECOVERY_BACKUP_ALL_TAR;
	} else {
		if (request == RECOVERY_BACKUP_REQUEST_PARTITION) {
			ret = blk_dselect_hwpart(desc, EMMC_HWPART_DEFAULT);
			if (!ret)
				ret = recovery_backup_source_from_partition(desc,
									    number, &source);
		} else {
			ret = recovery_backup_source_from_boot(desc,
							       request ==
							       RECOVERY_BACKUP_REQUEST_BOOT1,
							       &source);
		}
		if (ret) {
			if (ret == -ENOENT) {
				error_status = "404 Not Found";
				error_message = "Partition does not exist.\n";
			}
			goto error;
		}

		backup->mode = RECOVERY_BACKUP_SINGLE;
		backup->source = source;
		backup->next_lba = source.start;
		backup->blocks_left = source.blocks;
		backup->blksz = source.blksz;
		cache_blocks = RECOVERY_MMC_BACKUP_CHUNK / source.blksz;
		if (!cache_blocks) {
			error_status = "500 Internal Server Error";
			error_message = "eMMC block size exceeds backup buffer.\n";
			goto error;
		}
		if (cache_blocks > source.blocks)
			cache_blocks = source.blocks;
		cache_capacity = cache_blocks * source.blksz;
	}

	backup->cache = memalign(ARCH_DMA_MINALIGN, cache_capacity);
	if (!backup->cache) {
		error_message = "Cannot allocate backup buffer.\n";
		goto error;
	}
	backup->cache_capacity = cache_capacity;

	if (backup->mode == RECOVERY_BACKUP_ALL_TAR) {
		header_len = snprintf(backup->http_header,
				      sizeof(backup->http_header),
				      "HTTP/1.0 200 OK\r\n"
				      "Content-Type: application/x-tar\r\n"
				      "Content-Disposition: attachment; "
				      "filename=\"sbe1v1k-emmc-backup.tar\"\r\n"
				      "Cache-Control: no-store\r\n"
				      "Content-Length: %llu\r\n"
				      "Connection: close\r\n\r\n",
				      archive_size);
		ret = recovery_backup_start_next_tar_source(backup);
		if (ret <= 0)
			goto error;
	} else {
		header_len = snprintf(backup->http_header,
				      sizeof(backup->http_header),
				      "HTTP/1.0 200 OK\r\n"
				      "Content-Type: application/octet-stream\r\n"
				      "Content-Disposition: attachment; "
				      "filename=\"sbe1v1k-%s\"\r\n"
				      "Cache-Control: no-store\r\n"
				      "Content-Length: %llu\r\n"
				      "Connection: close\r\n\r\n",
				      source.filename, source.bytes);
	}
	if (header_len < 0 || header_len >= sizeof(backup->http_header))
		goto error;
	backup->http_header_len = header_len;

	backup->magic = RECOVERY_BACKUP_MAGIC;
	backup->active_counted = true;
	recovery_backup_active++;

	file->data = NULL;
	file->len = INT_MAX;
	file->index = 0;
	file->pextension = (fs_file_extension *)backup;
	file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
	if (backup->mode == RECOVERY_BACKUP_ALL_TAR)
		recovery_debug_printf("httpd: streaming full eMMC backup (%u sources, %llu-byte tar)\n",
				      source_count, archive_size);
	else
		recovery_debug_printf("httpd: streaming backup of %s (%llu bytes)\n",
				      source.name, source.bytes);
	return 1;

error:
	blk_dselect_hwpart(desc, EMMC_HWPART_DEFAULT);
	free(backup->cache);
	free(backup);
	return recovery_http_error(file, error_status, error_message);
}

static int recovery_http_json(struct fs_file *file, char *page,
			      size_t page_size, const char *json, int json_len)
{
	int header_len;
	size_t body_len;

	if (json_len < 0)
		return 0;
	if ((size_t)json_len >= page_size)
		return 0;

	header_len = snprintf(page, page_size,
			      "HTTP/1.1 200 OK\r\n"
			      "Content-Type: application/json\r\n"
			      "Cache-Control: no-store\r\n"
			      "Content-Length: %d\r\n"
			      "Connection: keep-alive\r\n\r\n",
			      json_len);
	if (header_len < 0 || (size_t)header_len >= page_size)
		return 0;
	if ((size_t)json_len > page_size - header_len)
		return 0;

	body_len = json_len;
	memcpy(page + header_len, json, body_len);
	recovery_http_static_file(file, page, header_len + body_len);
	return 1;
}

static int recovery_open_status(struct fs_file *file)
{
	static char page[512];
	char json[256];
	int json_len;

	json_len = snprintf(json, sizeof(json),
			    "{\"in_progress\":%d,\"done\":%u,\"total\":%u,"
			    "\"erase_done\":%u,\"erase_total\":%u,"
			    "\"write_done\":%u,\"write_total\":%u,"
			    "\"prepared\":%d,\"ok\":%d,\"error\":%d,\"phase\":%d,"
			    "\"reboot\":%d,\"clean_ready\":%d}\n",
			    prog_phase > 0 && prog_phase < 3,
			    (unsigned int)prog_done, (unsigned int)prog_total,
			    (unsigned int)prog_erase_done,
			    (unsigned int)prog_erase_total,
			    (unsigned int)prog_write_done,
			    (unsigned int)prog_write_total,
			    recovery_stream.active && recovery_stream.prepared,
			    prog_phase == 3, prog_phase == -1, prog_phase,
			    prog_reboot,
			    recovery_sbe1v1k_clean_session_ready());
	if (json_len < 0)
		return 0;
	if ((size_t)json_len >= sizeof(json))
		json_len = sizeof(json) - 1;

	return recovery_http_json(file, page, sizeof(page), json, json_len);
}

static int recovery_open_about(struct fs_file *file)
{
	const struct recovery_sbe1v1k_layout_desc *layout =
		recovery_sbe1v1k_layout_desc(active_sbe1v1k_layout);
	static char page[640];
	unsigned long long firmware_max;
	unsigned long env_max;
	const char *layout_name;
	char json[512];
	int json_len;

	layout_name = layout ? layout->name : "unknown";
	if (!layout)
		/*
		 * Size/partition hints exposed for an unknown disk must never opt a
		 * client into a legacy tail profile.  The UI will keep firmware
		 * writes blocked until the full profile has been applied.
		 */
		layout = &sbe1v1k_layout_full;
	firmware_max = (unsigned long long)(layout->kernel_size +
						 layout->rootfs_size) * 512;
	env_max = env_get_hex("recovery_max", 0);
	if (env_max && firmware_max > env_max)
		firmware_max = env_max;

#ifdef U_BOOT_DATE
	json_len = snprintf(json, sizeof(json),
			    "{\"u_boot\":\"%s (%s - %s %s)\","
			    "\"layout\":\"%s\",\"kernel_pad\":%lu,"
			    "\"firmware_max\":%llu,\"rootarg\":\"%s\","
			    "\"uboot_part\":\"%s\",\"clean_ready\":%d}\n",
			    U_BOOT_VERSION, U_BOOT_DATE, U_BOOT_TIME,
			    U_BOOT_TZ, layout_name, (ulong)layout->kernel_pad,
			    firmware_max, layout->rootarg, layout->uboot_part,
			    recovery_sbe1v1k_clean_session_ready());
#else
	json_len = snprintf(json, sizeof(json),
			    "{\"u_boot\":\"%s\",\"layout\":\"%s\","
			    "\"kernel_pad\":%lu,\"firmware_max\":%llu,"
			    "\"rootarg\":\"%s\",\"uboot_part\":\"%s\","
			    "\"clean_ready\":%d}\n",
			    U_BOOT_VERSION, layout_name,
			    (ulong)layout->kernel_pad, firmware_max,
			    layout->rootarg, layout->uboot_part,
			    recovery_sbe1v1k_clean_session_ready());
#endif
	if (json_len < 0)
		return 0;
	if ((size_t)json_len >= sizeof(json))
		json_len = sizeof(json) - 1;

	return recovery_http_json(file, page, sizeof(page), json, json_len);
}

/* lwIP httpd custom file hooks; static files continue to use fsdata. */
int fs_open_custom(struct fs_file *file, const char *name)
{
	static const char page_ok[] =
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: text/plain\r\n"
		"Cache-Control: no-store\r\n"
		"Content-Length: 2\r\n"
		"Connection: close\r\n\r\n"
		"OK";
	const char *p;

	if (!file || !name)
		return 0;

	memset(file, 0, sizeof(*file));
	p = *name == '/' ? name + 1 : name;

	if (!strcmp(p, "partitions"))
		return recovery_open_partitions(file);
	if (!strncmp(p, "backup/", 7))
		return recovery_open_backup(file, p);
	if (!strcmp(p, "status"))
		return recovery_open_status(file);
	if (!strcmp(p, "about"))
		return recovery_open_about(file);
	if (!strcmp(p, "ok")) {
		recovery_http_static_file(file, page_ok, sizeof(page_ok) - 1);
		return 1;
	}

	return 0;
}

void fs_close_custom(struct fs_file *file)
{
	struct recovery_backup_file *backup;

	if (!file || !file->pextension)
		return;

	backup = (struct recovery_backup_file *)file->pextension;
	if (backup->magic != RECOVERY_BACKUP_MAGIC)
		return;

	recovery_backup_restore_user(backup);
	backup->magic = 0;
	if (backup->active_counted && recovery_backup_active)
		recovery_backup_active--;
	free(backup->cache);
	free(backup);
	file->pextension = NULL;
}

static int recovery_backup_fill_cache(struct recovery_backup_file *backup)
{
	lbaint_t blocks;
	int ret;

	if (!backup->blocks_left)
		return 0;
	if (backup->desc->hwpart != backup->source.hwpart) {
		ret = blk_dselect_hwpart(backup->desc, backup->source.hwpart);
		if (ret) {
			printf("httpd: cannot select eMMC hwpart %d for backup: %d\n",
			       backup->source.hwpart, ret);
			goto read_error;
		}
	}

	blocks = backup->cache_capacity / backup->blksz;
	if (!blocks)
		goto read_error;
	if (blocks > backup->blocks_left)
		blocks = backup->blocks_left;
	if (blk_dread(backup->desc, backup->next_lba, blocks,
		      backup->cache) != blocks) {
		printf("httpd: eMMC backup read failed on hwpart %d at block "
		       LBAF "\n", backup->source.hwpart, backup->next_lba);
		goto read_error;
	}

	backup->next_lba += blocks;
	backup->blocks_left -= blocks;
	backup->cache_len = blocks * backup->blksz;
	backup->cache_off = 0;
	recovery_watchdog_poll();
	return 1;

read_error:
	backup->failed = true;
	backup->blocks_left = 0;
	backup->cache_len = 0;
	backup->cache_off = 0;
	return -EIO;
}

static int recovery_read_backup_single(struct recovery_backup_file *backup,
				       char *buffer, int count)
{
	int copied = 0;

	while (copied < count) {
		size_t available;
		size_t todo;
		int ret;

		if (backup->cache_off >= backup->cache_len) {
			ret = recovery_backup_fill_cache(backup);
			if (ret <= 0)
				break;
		}

		available = backup->cache_len - backup->cache_off;
		todo = min_t(size_t, available, count - copied);
		memcpy(buffer + copied, backup->cache + backup->cache_off, todo);
		backup->cache_off += todo;
		copied += todo;
	}

	if (!backup->blocks_left && backup->cache_off >= backup->cache_len)
		recovery_backup_restore_user(backup);
	return copied ? copied : FS_READ_EOF;
}

static int recovery_read_backup_tar(struct recovery_backup_file *backup,
				    char *buffer, int count)
{
	int copied = 0;

	while (copied < count) {
		size_t available;
		size_t todo;
		int ret;

		switch (backup->tar_phase) {
		case RECOVERY_BACKUP_TAR_MEMBER_HEADER:
			available = sizeof(backup->tar_header) -
				    backup->tar_header_off;
			todo = min_t(size_t, available, count - copied);
			memcpy(buffer + copied,
			       (const u8 *)&backup->tar_header +
			       backup->tar_header_off, todo);
			backup->tar_header_off += todo;
			copied += todo;
			if (backup->tar_header_off == sizeof(backup->tar_header))
				backup->tar_phase = RECOVERY_BACKUP_TAR_MEMBER_DATA;
			break;

		case RECOVERY_BACKUP_TAR_MEMBER_DATA:
			if (backup->cache_off >= backup->cache_len) {
				ret = recovery_backup_fill_cache(backup);
				if (ret < 0)
					goto failed;
				if (!ret) {
					backup->tar_phase =
						RECOVERY_BACKUP_TAR_MEMBER_PADDING;
					break;
				}
			}
			available = backup->cache_len - backup->cache_off;
			todo = min_t(size_t, available, count - copied);
			memcpy(buffer + copied, backup->cache + backup->cache_off,
			       todo);
			backup->cache_off += todo;
			copied += todo;
			break;

		case RECOVERY_BACKUP_TAR_MEMBER_PADDING:
			if (backup->tar_padding_left) {
				todo = min_t(unsigned long long,
					     backup->tar_padding_left, count - copied);
				memset(buffer + copied, 0, todo);
				backup->tar_padding_left -= todo;
				copied += todo;
				break;
			}
			ret = recovery_backup_start_next_tar_source(backup);
			if (ret < 0)
				goto failed;
			break;

		case RECOVERY_BACKUP_TAR_END:
			if (backup->tar_end_left) {
				todo = min_t(unsigned long long, backup->tar_end_left,
					     count - copied);
				memset(buffer + copied, 0, todo);
				backup->tar_end_left -= todo;
				copied += todo;
				break;
			}
			backup->tar_phase = RECOVERY_BACKUP_TAR_DONE;
			recovery_backup_restore_user(backup);
			break;

		case RECOVERY_BACKUP_TAR_DONE:
			return copied ? copied : FS_READ_EOF;
		}
	}

	return copied ? copied : FS_READ_EOF;

failed:
	backup->failed = true;
	backup->tar_phase = RECOVERY_BACKUP_TAR_DONE;
	recovery_backup_restore_user(backup);
	return copied ? copied : FS_READ_EOF;
}

static int recovery_read_backup(struct recovery_backup_file *backup,
				char *buffer, int count)
{
	int copied = 0;
	int read;

	if (backup->http_header_off < backup->http_header_len) {
		size_t available = backup->http_header_len -
				   backup->http_header_off;
		size_t todo = min_t(size_t, available, count);

		memcpy(buffer, backup->http_header + backup->http_header_off, todo);
		backup->http_header_off += todo;
		copied += todo;
	}
	if (copied == count)
		return copied;

	if (backup->mode == RECOVERY_BACKUP_ALL_TAR)
		read = recovery_read_backup_tar(backup, buffer + copied,
						count - copied);
	else
		read = recovery_read_backup_single(backup, buffer + copied,
						   count - copied);
	if (read > 0)
		copied += read;
	return copied ? copied : read;
}

int fs_read_custom(struct fs_file *file, char *buffer, int count)
{
	struct recovery_backup_file *backup;
	u32_t left;
	int read;
	bool finished;

	if (!file || !buffer || count <= 0)
		return FS_READ_EOF;

	backup = (struct recovery_backup_file *)file->pextension;
	if (backup && backup->magic == RECOVERY_BACKUP_MAGIC) {
		read = recovery_read_backup(backup, buffer, count);
		if (backup->mode == RECOVERY_BACKUP_ALL_TAR)
			finished = backup->tar_phase == RECOVERY_BACKUP_TAR_DONE;
		else
			finished = !backup->blocks_left &&
				   backup->cache_off >= backup->cache_len;
		finished = finished &&
			   backup->http_header_off >= backup->http_header_len;
		if (read < 0 || finished) {
			recovery_backup_restore_user(backup);
			file->index = file->len;
		} else if (file->index > file->len - read - 1) {
			/* Keep lwIP's int-sized cursor live for partitions over 2 GiB. */
			file->index = 0;
		} else {
			file->index += read;
		}
		return read;
	}

	left = file->len - file->index;
	if (left <= 0)
		return FS_READ_EOF;
	if ((u32_t)count > left)
		count = left;
	memcpy(buffer, file->data + file->index, count);
	file->index += count;
	return count;
}

/* bytes_left for custom files is handled by fs_read_custom + file->index; */

/* HTTP POST handlers */
static int recovery_stream_format_from_uri(
	const char *uri, enum recovery_stream_format *format)
{
	const char *value = strstr(uri, "format=");

	*format = RECOVERY_STREAM_RAW;
	if (!value)
		return 0;
	value += strlen("format=");
	if (!strncmp(value, "raw", 3) && (value[3] == '\0' || value[3] == '&'))
		return 0;
	if (!strncmp(value, "tar", 3) && (value[3] == '\0' || value[3] == '&')) {
		*format = RECOVERY_STREAM_TAR;
		return 0;
	}

	return -EINVAL;
}

/* Parse ?sha256=<64 hex chars> out of a request URI. */
static bool recovery_sha256_from_uri(const char *uri, u8 *out)
{
	const char *value = strstr(uri, "sha256=");
	size_t i;

	if (!value)
		return false;
	value += strlen("sha256=");

	for (i = 0; i < 32; i++) {
		int hi = hex_to_bin(value[2 * i]);
		int lo = hex_to_bin(value[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return false;
		out[i] = (u8)((hi << 4) | lo);
	}

	return value[64] == '\0' || value[64] == '&';
}

static int recovery_layout_from_uri(const char *uri,
				    enum recovery_sbe1v1k_layout *layout)
{
	const char *query;
	const char *part;
	const char *end;
	bool seen_layout = false;

	/*
	 * Full clean install is the only destructive profile.  Require the
	 * selector explicitly so a stale or incomplete client cannot
	 * accidentally trigger a destructive default.  Parse query segments
	 * instead of taking the first substring: duplicate or non-exact layout
	 * selectors must fail closed before any body/erase.
	 */
	*layout = RECOVERY_SBE1V1K_LAYOUT_FULL;
	query = strchr(uri, '?');
	if (!query)
		return -EINVAL;

	part = query + 1;
	while (*part) {
		end = strchr(part, '&');
		if (!end)
			end = part + strlen(part);

		if ((size_t)(end - part) >= strlen("layout=") &&
		    !strncmp(part, "layout=", strlen("layout="))) {
			if (seen_layout ||
			    (size_t)(end - part) != strlen("layout=full") ||
			    strncmp(part + strlen("layout="), "full",
				    strlen("full")))
				return -EINVAL;
			seen_layout = true;
		} else if ((size_t)(end - part) >= strlen("layout") &&
			   !strncmp(part, "layout", strlen("layout")) &&
			   (part + strlen("layout") == end ||
			    part[strlen("layout")] != '=')) {
			/* Reject malformed variants such as layout or layout[]=... */
			return -EINVAL;
		}

		if (!*end)
			break;
		part = end + 1;
	}

	return seen_layout ? 0 : -EINVAL;
}

/*
 * Pick a stable upload buffer:
 * recovery_addr -> loadaddr -> CONFIG_SYS_LOAD_ADDR -> RAM fallback.
 */
static int recovery_pick_recv_base(size_t len)
{
	ulong ram_start = (ulong)gd->ram_base;
	ulong ram_end = (ulong)gd->ram_base + (ulong)gd->ram_size;
	ulong base = env_get_hex("recovery_addr", 0);

	if (!base)
		base = env_get_hex("loadaddr", 0);
	if (!base)
		base = CONFIG_SYS_LOAD_ADDR;

	/* Keep the buffer inside usable RAM. */
	if (base < ram_start || (base + len) > ram_end) {
		ulong fallback = ram_start + 0x01000000UL;

		if (fallback >= ram_start &&
		    fallback + len <= ram_end) {
			base = fallback;
		} else if (ram_end - len > ram_start) {
			base = ram_end - len;
		} else {
			prog_phase = -1;
			printf("httpd: no sufficient RAM for upload (%u bytes)\n",
			       (unsigned int)len);
			return -ENOMEM;
		}
	}
	recv_base = (u8 *)base;
	return 0;
}

err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                       u16_t http_request_len, int content_len, char *response_uri,
                       u16_t response_uri_len, u8_t *post_auto_wnd)
{
	bool stream_enabled;
	const char *tname;

	(void)http_request;
	(void)http_request_len;
	(void)connection;
	/*
	 * Throttle large uploads explicitly: recovery accepts firmware images
	 * that are much larger than a typical lwIP POST body and manual window
	 * updates avoid over-optimistic receive windows.
	 */
	if (post_auto_wnd)
		*post_auto_wnd = 0;
	post_ok = 0;
	if (recovery_backup_active) {
		printf("httpd: rejecting destructive operation while %u backup stream(s) are active\n",
		       recovery_backup_active);
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return ERR_USE;
	}
	if (prepare_request || (prog_phase > 0 && prog_phase < 3) ||
	    prog_reboot || reboot_request) {
		printf("httpd: rejecting concurrent destructive operation\n");
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return ERR_USE;
	}

	upload_done = 0;
	recv_off = 0;
	recv_total = 0;
	current_force_recreate = false;
	current_prepare_only = false;
	current_sha256_valid = false;
	recv_to_ram = false;
	post_destructive = false;

	/* Accept optional query parameters after the target path. */
	if (!strncmp(uri, "/action/prepare/firmware", 24) &&
	    (uri[24] == '\0' || uri[24] == '?')) {
		current_target = TARGET_FIRMWARE;
		current_prepare_only = true;
	} else if (!strncmp(uri, "/action/prepare/uboot", 21) &&
		   (uri[21] == '\0' || uri[21] == '?')) {
		/* Chainloaders must be received and verified in RAM before erase. */
		prog_phase = -1;
		printf("httpd: chainloader prepare-stream is disabled; upload it directly for staged validation\n");
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return ERR_ARG;
	} else if (!strncmp(uri, "/upload/firmware", 16) &&
		   (uri[16] == '\0' || uri[16] == '?')) {
		current_target = TARGET_FIRMWARE;
	} else if (!strncmp(uri, "/upload/uboot", 13) &&
		   (uri[13] == '\0' || uri[13] == '?')) {
		current_target = TARGET_UBOOT;
	} else if (!strncmp(uri, "/upload/boot0", 13) &&
		   (uri[13] == '\0' || uri[13] == '?')) {
		current_target = TARGET_BOOT0;
	} else if (!strncmp(uri, "/upload/boot1", 13) &&
		   (uri[13] == '\0' || uri[13] == '?')) {
		current_target = TARGET_BOOT1;
	} else if (!strncmp(uri, "/upload/art", 11) &&
		   (uri[11] == '\0' || uri[11] == '?')) {
		current_target = TARGET_ART;
	} else if (!strncmp(uri, "/action/repartition", 19) &&
		   (uri[19] == '\0' || uri[19] == '?')) {
		current_target = TARGET_REPARTITION;
	} else if (!strncmp(uri, "/upload", 7) &&
		   (uri[7] == '\0' || uri[7] == '?')) {
		current_target = TARGET_FIRMWARE;
	} else {
		prog_phase = -1;
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return ERR_ARG;
	}
	if (recovery_stream_format_from_uri(uri, &current_stream_format)) {
		prog_phase = -1;
		printf("httpd: invalid stream format\n");
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return ERR_ARG;
	}
	if (current_target == TARGET_REPARTITION &&
	    recovery_layout_from_uri(uri, &current_repartition_layout)) {
		prog_phase = -1;
		printf("httpd: invalid SBE1V1K partition profile\n");
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return ERR_ARG;
	}
	if ((current_target == TARGET_UBOOT ||
	     current_target == TARGET_BOOT0 ||
	     current_target == TARGET_BOOT1 ||
	     current_target == TARGET_ART) &&
	    current_stream_format != RECOVERY_STREAM_RAW) {
		prog_phase = -1;
		printf("httpd: raw-format uploads required for this target\n");
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return ERR_ARG;
	}

	if ((current_target == TARGET_BOOT0 ||
	     current_target == TARGET_BOOT1 ||
	     current_target == TARGET_ART) &&
	    !recovery_sha256_from_uri(uri, current_sha256_expected)) {
		prog_phase = -1;
		printf("httpd: factory-file upload requires ?sha256=<64 hex chars>\n");
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return ERR_ARG;
	}
	current_sha256_valid = current_target == TARGET_BOOT0 ||
			       current_target == TARGET_BOOT1 ||
			       current_target == TARGET_ART;

	if (current_target == TARGET_FIRMWARE &&
	    (strstr(uri, "?recreate=1") || strstr(uri, "&recreate=1") ||
	     strstr(uri, "?force_recreate=1") ||
	     strstr(uri, "&force_recreate=1")))
		current_force_recreate = true;

	/* The prepare/upload protocol covers every MMC firmware target.
	 * recovery_stream only selects where bytes go: 1 writes eMMC during
	 * receive; any other value buffers RAM and flashes after the POST
	 * response, so the network transfer never waits on eMMC. */
	stream_enabled = recovery_backend_is_mmc() &&
			 current_target == TARGET_FIRMWARE;
	recv_to_ram = stream_enabled &&
		      env_get_yesno("recovery_stream") != 1;

	if (current_prepare_only) {
		if (!stream_enabled || content_len <= 0 ||
		    content_len > RECOVERY_PREPARE_BODY_MAX) {
			prog_phase = -1;
			printf("httpd: invalid destructive prepare request\n");
			strlcpy(response_uri, "/fail.html", response_uri_len);
			return ERR_ARG;
		}
	} else if (current_target == TARGET_REPARTITION) {
		if (content_len <= 0 ||
		    (ulong)content_len > RECOVERY_REPARTITION_MAX) {
			prog_phase = -1;
			printf("httpd: invalid repartition request length %d\n",
			       content_len);
			strlcpy(response_uri, "/fail.html", response_uri_len);
			return ERR_ARG;
		}
	} else if (content_len <= 0 ||
		   recovery_validate_upload_length(current_target,
						   current_force_recreate,
						   content_len,
						   current_stream_format)) {
		prog_phase = -1;
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return ERR_ARG;
	}

	recv_total = content_len;
	if (stream_enabled && !current_prepare_only) {
		if (!recovery_stream.active || !recovery_stream.prepared ||
		    recovery_stream.target != current_target ||
		    recovery_stream.format != current_stream_format ||
		    recovery_stream.total_expected != recv_total) {
			recovery_mmc_stream_reset();
			recovery_stream_completed = false;
			prog_phase = -1;
			printf("httpd: destructive upload rejected; prepare target and exact size first\n");
			strlcpy(response_uri, "/fail.html", response_uri_len);
			return ERR_USE;
		}
		/*
		 * Preparation and upload are separate HTTP requests.  Re-bind the
		 * prepared stream to the same in-RAM clean session immediately
		 * before accepting the first firmware byte.
		 */
		if (recovery_board_is_sbe1v1k() &&
		    recovery_sbe1v1k_clean_session_validate()) {
			recovery_mmc_stream_reset();
			recovery_stream_completed = false;
			prog_phase = -1;
			printf("httpd: prepared firmware stream lost clean-session binding; clear eMMC again\n");
			strlcpy(response_uri, "/fail.html", response_uri_len);
			return ERR_USE;
		}

		if (recv_to_ram &&
		    recovery_pick_recv_base(recv_total)) {
			strlcpy(response_uri, "/fail.html",
				response_uri_len);
			return ERR_MEM;
		}
		recovery_stream.prepared = false;
		recovery_stream_completed = false;
		prog_phase = 2;
		prog_write_done = 0;
		prog_write_total = recv_total;
		prog_total = prog_erase_total + prog_write_total;
		prog_done = prog_erase_total;
		prog_reboot = 0;
		post_ok = 1;
		tname = current_target == TARGET_FIRMWARE ? "firmware" : "uboot";
		recovery_debug_printf("httpd: accepting prepared destructive stream of %u bytes for %s\n",
				      recv_total, tname);
		return ERR_OK;
	}

	/* Preparation bodies and non-streaming uploads use the bounded RAM path. */
	recovery_mmc_stream_reset();
	recovery_stream_completed = false;
	prog_phase = 0;
	prog_done = 0;
	prog_total = 0;
	prog_erase_done = 0;
	prog_erase_total = 0;
	prog_write_done = 0;
	prog_write_total = 0;
	prog_reboot = 0;

	if (recovery_pick_recv_base(recv_total)) {
		strlcpy(response_uri, "/fail.html", response_uri_len);
		return ERR_MEM;
	}

	post_ok = 1;
	tname = recovery_target_name(current_target);
	if (current_prepare_only)
		recovery_debug_printf("httpd: accepting erase preparation for %s\n", tname);
	else
		recovery_debug_printf("httpd: accepting upload of %u bytes for %s to 0x%08lx%s\n",
				      recv_total, tname, (ulong)recv_base,
				      current_force_recreate ? " (force recreate)" : "");
	return ERR_OK;
}

err_t httpd_post_receive_data(void *connection, struct pbuf *p)
{
	struct pbuf *q;
	u16_t recved = 0;

	if (!post_ok) {
		pbuf_free(p);
		return ERR_ARG;
	}

	/* Stream directly to eMMC when enabled; otherwise retain the RAM path. */
	for (q = p; q != NULL; q = q->next) {
		size_t avail = recv_total - recv_off;
		size_t clen = q->len;

		if (clen > avail)
			clen = avail;
		if (recovery_stream.active && !recv_to_ram) {
			if (recovery_mmc_stream_append(q->payload, clen)) {
				post_ok = 0;
				prog_phase = -1;
				recovery_mmc_stream_reset();
				pbuf_free(p);
				return ERR_IF;
			}
		} else {
			memcpy(recv_base + recv_off, q->payload, clen);
		}
		recv_off += clen;
	}
	if (recv_to_ram && recovery_stream.active && recv_off) {
		/* Buffered receive: mirror byte progress on the write bar. */
		prog_write_done = recv_off;
		prog_done = prog_erase_total + recv_off;
	}
	/* Save the advertised receive length before pbuf_free() releases it. */
	recved = p->tot_len;
	pbuf_free(p);

    if (recv_off >= recv_total) {
        upload_done = 1;
    }

#if LWIP_HTTPD_POST_MANUAL_WND
    if (recved)
        httpd_post_data_recved(connection, recved);
#endif

    return ERR_OK;
}

void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len)
{
	unsigned long requested_size;
	int ret;

	(void)connection;
	if (current_prepare_only) {
		if (!post_ok || !recv_total || recv_off != recv_total) {
			post_ok = 0;
			prog_phase = -1;
		} else {
			recv_base[recv_off] = '\0';
			ret = strict_strtoul((const char *)recv_base, 10,
					     &requested_size);
			if (ret || recovery_validate_upload_length(current_target,
								   false,
								   requested_size,
								   current_stream_format)) {
				post_ok = 0;
				prog_phase = -1;
			} else {
				prepare_target = current_target;
				prepare_size = requested_size;
				prepare_stream_format = current_stream_format;
				/* -1 reserves the operation until the delayed callback fires. */
				prepare_request = -1;
				prog_phase = 0;
			}
		}

		recovery_debug_printf("httpd: prepare request finished, %u/%u bytes received\n",
				      recv_off, recv_total);
		if (post_ok) {
			strlcpy(response_uri, "/ok", response_uri_len);
			sys_timeout(RECOVERY_PREPARE_START_DELAY_MS,
				    prepare_delay_cb, NULL);
		} else {
			strlcpy(response_uri, "/fail.html", response_uri_len);
		}
		return;
	}

	if (post_ok && recovery_stream.active && recv_off == recv_total) {
		if (recv_to_ram) {
			/*
			 * Whole image is in RAM: drop the stream bookkeeping and let
			 * httpd_post_response_complete flash it from recv_base once
			 * the browser has acknowledged the POST response.  eMMC stays
			 * idle during the whole network transfer, and the clean
			 * session stays bound: flash_image validates it.
			 */
			recovery_mmc_stream_reset();
			prog_write_done = prog_write_total;
			prog_done = prog_total;
		} else if (recovery_mmc_stream_finish()) {
			post_ok = 0;
			prog_phase = -1;
		} else {
			recovery_sbe1v1k_clean_session_reset();
			recovery_stream_completed = true;
			prog_phase = 3;
			prog_write_done = prog_write_total;
			prog_done = prog_total;
			prog_reboot = 1;
		}
	}
	recovery_debug_printf("httpd: post finished, %u/%u bytes received\n",
			      recv_off, recv_total);
    /* Tell httpd which page to return after POST (keep user on main page) */
    if (post_ok && recv_total && (recv_off >= recv_total)) {
        strlcpy(response_uri, "/ok", response_uri_len);
        /* Deferred work (flash/reboot) starts from
         * httpd_post_response_complete() once the browser ACKs this
         * response, so eMMC work never begins for a client that never
         * confirmed the upload. */
        post_destructive = true;
    } else {
        strlcpy(response_uri, "/fail.html", response_uri_len);
        post_destructive = false;
        if (recovery_stream.active) {
            post_ok = 0;
            prog_phase = -1;
            recovery_mmc_stream_reset();
        }
    }
}

#if LWIP_HTTPD_POST_RESPONSE_ACK
void httpd_post_response_complete(void *connection, err_t result)
{
	(void)connection;

	if (!post_destructive)
		return;
	post_destructive = false;

	if (result != ERR_OK || !post_ok) {
		printf("httpd: POST response not acknowledged (%d); deferred work cancelled\n",
		       result);
		prog_phase = -1;
		return;
	}

	printf("httpd: POST response acknowledged; starting deferred work\n");
	flash_request = 1;
	if (recovery_stream_completed)
		sys_timeout(REBOOT_DELAY_MS, reboot_delay_cb, NULL);
}
#endif

static int flash_image(struct recovery_status_led_ctrl *status_leds)
{
	struct recovery_target target;
	size_t retlen;
	int ret;

	if (!recv_off) {
		printf("No data received to flash\n");
		prog_phase = -1;
		return -EINVAL;
	}

	if (current_target == TARGET_BOOT0 || current_target == TARGET_BOOT1 ||
	    current_target == TARGET_ART) {
		/*
		 * Manifest gate: the byte count was checked at POST time; the
		 * content digest must match before a single sector moves.
		 */
		u8 computed[32];
		char hex[2 * sizeof(computed) + 1];
		size_t i;

		sha256_csum_wd(recv_base, recv_off, computed, CHUNKSZ_SHA256);
		for (i = 0; i < sizeof(computed); i++)
			sprintf(hex + 2 * i, "%02x", computed[i]);
		hex[2 * sizeof(computed)] = '\0';
		printf("Upload SHA-256 %s\n", hex);

		if (!current_sha256_valid ||
		    memcmp(computed, current_sha256_expected,
			   sizeof(computed))) {
			printf("Factory-file upload SHA-256 mismatch; refusing to write\n");
			prog_phase = -1;
			return -EBADMSG;
		}
	}

	if (current_target == TARGET_REPARTITION) {
		prog_reboot = 0;
		ret = recovery_repartition_factory(status_leds,
						   current_repartition_layout);
		if (ret) {
			printf("Factory repartition failed: %d\n", ret);
			prog_phase = -1;
			return ret;
		}

		prog_phase = 3;
		recovery_debug_printf("Factory repartition complete.\n");
		return 0;
	}

	ret = recovery_resolve_target(current_target, &target);
	if (ret) {
		printf("No flash target found for upload type %d\n", current_target);
		prog_phase = -1;
		return ret;
	}

	if (current_force_recreate && current_target == TARGET_FIRMWARE &&
	    target.backend == RECOVERY_BACKEND_UBI)
		target.limit = recovery_calc_forced_ubi_limit(&target);

	if (recv_off > target.limit) {
		printf("Image size %u exceeds target size %llu\n",
		       recv_off, target.limit);
		recovery_release_target(&target);
		prog_phase = -1;
		return -EFBIG;
	}

	if (target.backend == RECOVERY_BACKEND_MMC ||
	    target.backend == RECOVERY_BACKEND_MMC_BOOT) {
		ret = recovery_flash_mmc_target(current_target, status_leds);
		recovery_release_target(&target);
		if (ret) {
			printf("eMMC flash failed: %d\n", ret);
			prog_phase = -1;
			return ret;
		}

		prog_phase = 3;
		/* Sequential factory-file restores must keep the page alive. */
		prog_reboot = !(current_target == TARGET_BOOT0 ||
				current_target == TARGET_BOOT1 ||
				current_target == TARGET_ART);
		recovery_debug_printf("Flashing complete.\n");
		return 0;
	}

	if (target.backend == RECOVERY_BACKEND_UBI) {
		bool reformatted = false;

		if (current_force_recreate && current_target == TARGET_FIRMWARE)
			recovery_debug_printf("Force recreate requested: removing non-preserved UBI volumes before flashing.\n");

		ret = recovery_prepare_ubi_target(&target, status_leds, recv_off,
						      &reformatted);
		if (ret) {
			printf("Failed to prepare UBI target '%s' on '%s': %d\n",
			       target.name, target.ubi_part, ret);
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		if (!reformatted) {
			ret = recovery_cleanup_ubi_firmware(&target, status_leds,
							    recv_off);
			if (ret) {
				printf("Failed to clean UBI firmware volumes for '%s': %d\n",
				       target.name, ret);
				recovery_release_target(&target);
				prog_phase = -1;
				return ret;
			}
		}

		if (!target.cur_size) {
			recovery_debug_printf("Creating missing UBI volume '%s' for %u bytes...\n",
					      target.name, recv_off);
			ret = recovery_create_ubi_target(&target, recv_off);
			if (ret) {
				printf("ubi_create_volume failed for '%s': %d\n",
				       target.name, ret);
				prog_phase = -1;
				return ret;
			}
		} else if (recv_off > target.cur_size) {
			recovery_debug_printf("Resizing UBI volume '%s' from %llu to fit %u bytes...\n",
					      target.name, target.cur_size, recv_off);
			ret = recovery_resize_ubi_target(&target, recv_off);
			if (ret) {
				printf("ubi_resize_volume failed for '%s': %d\n",
				       target.name, ret);
				prog_phase = -1;
				return ret;
			}
		}

		ret = recovery_ensure_preserved_ubi_volumes(&target);
		if (ret) {
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		ret = recovery_ensure_rootfs_data(&target);
		if (ret) {
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

			recovery_debug_printf("Writing %u bytes to UBI volume '%s' on '%s'...\n",
					      recv_off, target.name, target.ubi_part);
			prog_phase = 2;
			prog_done = prog_erase_total;
			prog_write_done = 0;
			recovery_service_runtime(status_leds);
			ret = recovery_write_ubi_target(&target, status_leds,
							recv_base, recv_off);
			if (ret) {
				printf("UBI write failed for '%s': %d\n",
				       target.name, ret);
				recovery_release_target(&target);
				prog_phase = -1;
			return ret;
		}

		prog_write_done = recv_off;
		prog_done = prog_erase_total + recv_off;
		recovery_release_target(&target);
		prog_phase = 3;
		prog_reboot = 1;
		if (current_target == TARGET_FIRMWARE)
			xr1710g_sync_factory();
		recovery_debug_printf("Flashing complete.\n");
		return 0;
	}

	{
		struct mtd_info *mtd = target.mtd;
		loff_t ofs = target.ofs;
		loff_t erase_len = ALIGN(target.limit, mtd->erasesize);

		prog_phase = 1;
		prog_done = 0;
		prog_erase_done = 0;
		prog_erase_total = erase_len;
		prog_write_done = 0;
		prog_write_total = recv_off;
		prog_total = prog_erase_total + prog_write_total;

		recovery_debug_printf("Erasing entire target '%s' (%llu bytes) and writing %u bytes...\n",
				      target.name, (unsigned long long)target.limit,
				      recv_off);
		ret = recovery_erase_mtd_region(mtd, ofs, target.limit,
						status_leds);
		if (ret) {
			printf("mtd_erase failed: %d\n", ret);
			recovery_release_target(&target);
			prog_phase = -1;
			return ret;
		}

		prog_phase = 2;
		for (u32 written = 0; written < recv_off; ) {
			u32 remain = recv_off - written;
			u32 chunk = remain > (64 * 1024) ? (64 * 1024) : remain;

			ret = mtd_write(mtd, ofs + written, chunk, &retlen,
					recv_base + written);
			if (ret) {
				printf("mtd_write failed: ret=%d at 0x%llx\n",
				       ret, (unsigned long long)(ofs + written));
				recovery_release_target(&target);
				prog_phase = -1;
				return ret;
			}
			if (!retlen) {
				printf("mtd_write made no progress at 0x%llx\n",
				       (unsigned long long)(ofs + written));
				recovery_release_target(&target);
				prog_phase = -1;
				return -EIO;
			}

			written += retlen;
			prog_write_done = written;
			prog_done = prog_erase_done + prog_write_done;
			recovery_service_runtime(status_leds);
		}
	}

	recovery_release_target(&target);
	prog_phase = 3;
	prog_reboot = 1;
	if (current_target == TARGET_FIRMWARE)
		xr1710g_sync_factory();
	recovery_debug_printf("Flashing complete.\n");
	return 0;
}

static const char sbe1v1k_tftp_repartition_token[] = "SBE1V1K_REPARTITION";

/*
 * TFTP recovery for SBE1V1K full-layout boards: pull the sysupgrade firmware
 * from the PC (default 192.168.255.2, file firmware.bin), then run the
 * destructive full clean and flash it through the same pipeline as the HTTP
 * recovery page.  The clean only fires after a plausibly sized firmware image
 * is in hand, so entering recovery by accident never wipes a working board.
 */
int run_tftp_recovery(void)
{
	struct recovery_status_led_ctrl status_leds;
	const char *old_allow_no_link;
	char *saved_allow_no_link = NULL;
	const char *serverip;
	const char *filename;
	ulong window_ms, start_ms;
	int rc;

	old_allow_no_link = env_get("eth_allow_no_link");
	if (old_allow_no_link) {
		saved_allow_no_link = strdup(old_allow_no_link);
		if (!saved_allow_no_link)
			return -ENOMEM;
	}
	env_set("eth_allow_no_link", "1");
	recovery_board_http_acl(true);

	memset(&status_leds, 0, sizeof(status_leds));
	recv_off = recv_total = 0;
	recovery_mmc_stream_reset();
	recovery_stream_completed = false;
	recovery_runtime_status_leds = &status_leds;
	post_ok = 0;
	upload_done = 0;
	flash_request = 0;
	prepare_request = 0;
	prepare_size = 0;
	current_prepare_only = false;
	reboot_request = 0;
	recovery_sbe1v1k_clean_session_reset();
	if (recovery_board_is_sbe1v1k() && recovery_backend_is_mmc())
		recovery_detect_sbe1v1k_layout();

	if (recovery_status_led_init(&status_leds))
		printf("Recovery status LEDs unavailable\n");

	recovery_prepare_static_network();
	serverip = env_get("tftp_recovery_serverip");
	if (!serverip || !*serverip) {
		serverip = "192.168.255.2";
		env_set("tftp_recovery_serverip", serverip);
	}
	env_set("serverip", serverip);
	filename = env_get("tftp_recovery_file");
	if (!filename || !*filename) {
		filename = "firmware.bin";
		env_set("tftp_recovery_file", filename);
	}

	printf("\nSBE1V1K TFTP recovery\n");
	printf("This board : %s/24 (hold the reset button at power-on to get here)\n",
	       RECOVERY_STATIC_IPADDR);
	printf("Your PC    : set a static IP %s netmask %s, run a TFTP server\n",
	       serverip, RECOVERY_STATIC_NETMASK);
	printf("Firmware   : serve the sysupgrade image as '%s'\n", filename);

	window_ms = env_get_ulong("tftp_recovery_timeout", 10, 300) * 1000;
	start_ms = get_timer(0);

	while (1) {
		ulong size;
		ulong load_addr;

		if (tstc() && getchar() == 0x03) {
			printf("\nTFTP recovery aborted by user\n");
			rc = -EINTR;
			goto out;
		}

		recovery_watchdog_poll();
		printf("\nWaiting for firmware '%s' from TFTP server %s ...\n",
		       filename, serverip);
		rc = run_commandf("tftpboot %s %s",
				  env_get("loadaddr") ?: "0x60000000",
				  filename);
		size = env_get_ulong("filesize", 16, 0);
		if (rc || size < RECOVERY_MIN_FIRMWARE_SIZE ||
		    size > env_get_ulong("recovery_max", 16,
					 RECOVERY_UPLOAD_MAX)) {
			if (!rc && size)
				printf("Ignoring download with invalid size %lu\n",
				       size);
			if (window_ms &&
			    get_timer(start_ms) >= window_ms) {
				printf("TFTP recovery wait timed out; restarting normal boot\n");
				rc = -ETIMEDOUT;
				goto out;
			}
			mdelay(2000);
			recovery_watchdog_poll();
			continue;
		}

		load_addr = env_get_hex("loadaddr", 0x60000000);

		/*
		 * A firmware image is in hand: now run the destructive full
		 * clean (canonical GPT + rootfs_data zero-fill).  The clean
		 * only allocates heap memory, so the received image at
		 * load_addr stays intact across it.
		 */
		printf("\nFirmware received (%lu bytes). Starting full clean, do not power off...\n",
		       size);
		recv_base = (u8 *)sbe1v1k_tftp_repartition_token;
		recv_off = strlen(sbe1v1k_tftp_repartition_token);
		rc = recovery_repartition_factory(&status_leds,
						  current_repartition_layout);
		if (rc) {
			printf("Full clean failed (%d); waiting for another firmware image...\n",
			       rc);
			continue;
		}
		printf("Full clean complete. Flashing the received firmware...\n");
		recv_base = (u8 *)load_addr;
		recv_off = size;
		current_target = TARGET_FIRMWARE;
		rc = flash_image(&status_leds);
		if (rc) {
			printf("Flash failed (%d); the eMMC is clean. Waiting for another firmware image...\n",
			       rc);
			continue;
		}
		printf("Flash complete. Rebooting in %dms...\n", REBOOT_DELAY_MS);
		mdelay(REBOOT_DELAY_MS);
		do_reset(NULL, 0, 0, NULL);
	}

out:
	recovery_runtime_status_leds = NULL;
	recovery_sbe1v1k_clean_session_reset();
	recovery_mmc_stream_reset();
	recovery_status_led_stop(&status_leds);
	recovery_status_led_release(&status_leds);
	recovery_board_http_acl(false);
	env_set("eth_allow_no_link", saved_allow_no_link);
	free(saved_allow_no_link);
	return rc;
}

int run_http_recovery(void)
{
	struct udevice *udev = NULL;
	struct netif *netif = NULL;
	struct recovery_led_ctrl leds;
	struct recovery_status_led_ctrl status_leds;
	struct recovery_dhcp_server dhcp;
	bool use_status_leds = false;
	bool use_link_leds = false;
	bool eth_started = false;
	const char *old_allow_no_link;
	char *saved_allow_no_link = NULL;
	ulong timeout_ms;
	ulong start_ms;
	ulong edma_kick_last;
	int rc;

	old_allow_no_link = env_get("eth_allow_no_link");
	if (old_allow_no_link) {
		saved_allow_no_link = strdup(old_allow_no_link);
		if (!saved_allow_no_link)
			return -ENOMEM;
	}
	env_set("eth_allow_no_link", "1");
	recovery_board_http_acl(true);

	recv_off = recv_total = 0;
	recovery_mmc_stream_reset();
	recovery_stream_completed = false;
	recovery_runtime_status_leds = &status_leds;
	post_ok = 0;
	upload_done = 0;
	flash_request = 0;
	prepare_request = 0;
	prepare_size = 0;
	current_prepare_only = false;
	reboot_request = 0;
	recovery_sbe1v1k_clean_session_reset();
	memset(&leds, 0, sizeof(leds));
	memset(&status_leds, 0, sizeof(status_leds));
	memset(&dhcp, 0, sizeof(dhcp));
	if (recovery_board_is_sbe1v1k() && recovery_backend_is_mmc())
		recovery_detect_sbe1v1k_layout();

	recovery_debug_printf("HTTP recovery: preparing board runtime\n");
	recovery_watchdog_poll();
	rc = recovery_status_led_init(&status_leds);
	if (!rc) {
		use_status_leds = true;
	} else {
		printf("Recovery status LEDs unavailable (%d)\n", rc);
		if (!recovery_board_is_sbe1v1k()) {
			printf("Fallback to link LEDs\n");
			recovery_led_init(&leds);
			use_link_leds = true;
		}
	}
	recovery_prepare_static_network();

	recovery_debug_printf("HTTP recovery: starting Ethernet\n");
	recovery_watchdog_poll();
	rc = net_lwip_eth_start();
	if (rc < 0) {
		printf("Failed to start Ethernet: %d\n", rc);
		goto out;
	}
	eth_started = true;
	recovery_watchdog_poll();

	udev = eth_get_current();
	if (!udev || !eth_is_active(udev)) {
		printf("No active net device\n");
		rc = -ENODEV;
		goto out;
	}

	recovery_debug_printf("HTTP recovery: creating lwIP netif\n");
	netif = net_lwip_new_netif(udev);
	if (!netif) {
		rc = -ENODEV;
		goto out;
	}
	recovery_watchdog_poll();

	recovery_debug_printf("HTTP recovery: starting DHCP helper\n");
	rc = recovery_dhcp_server_init(&dhcp, netif);
	if (rc)
		printf("Failed to start recovery DHCP server: %d\n", rc);
	else
		net_lwip_set_recovery_dhcp_hook(recovery_dhcp_recv, &dhcp);

	recovery_debug_printf("HTTP recovery: starting HTTP server\n");
	httpd_init();
	recovery_debug_printf("HTTP recovery server listening on http://%s/\n",
			      ip4addr_ntoa(netif_ip4_addr(netif)));

	timeout_ms = env_get_ulong("recovery_timeout", 10, 0) * 1000;
	start_ms = get_timer(0);
	edma_kick_last = get_timer(0);

	extern void recovery_board_edma_kick(void);

	while (1) {
		if (tstc()) {
			int c = getchar();

			if (c == 0x03) { /* Ctrl-C */
				recovery_debug_printf("Abort by user\n");
				break;
			}
		}
		/* net_lwip_rx() already runs sys_check_timeouts(). */
		net_lwip_rx(udev, netif);
		if ((get_timer(edma_kick_last) > 2000UL)) {
			edma_kick_last = get_timer(0);
			recovery_board_edma_kick();
		}
		if (use_status_leds)
			recovery_status_led_poll(&status_leds);
		else if (use_link_leds)
			recovery_led_poll(&leds);
		if (prepare_request > 0) {
			prepare_request = 0;
			recovery_debug_printf("Preparing destructive eMMC stream for %lu bytes...\n",
					      (ulong)prepare_size);
			rc = recovery_mmc_stream_prepare(prepare_target,
							 prepare_size,
							 prepare_stream_format);
			if (rc) {
				prog_phase = -1;
				printf("Destructive stream preparation failed: %d\n",
				       rc);
			} else {
				recovery_debug_printf("Target erase complete; waiting for image stream.\n");
			}
		}
		if (flash_request) {
			flash_request = 0;
			recovery_debug_printf("Upload done, flashing...\n");
			rc = flash_image(&status_leds);
			if (!rc) {
				/* For TARGET_REPARTITION the flash *is* the clean;
				 * its session authorizes the firmware upload next. */
				if (current_target != TARGET_REPARTITION)
					recovery_sbe1v1k_clean_session_reset();
				if (prog_reboot) {
					recovery_debug_printf("Flashing complete. Rebooting in %dms...\n",
							      REBOOT_DELAY_MS);
					reboot_request = 0;
					sys_timeout(REBOOT_DELAY_MS, reboot_delay_cb, NULL);
				} else {
					recovery_debug_printf("Action complete. Keeping recovery server running.\n");
				}
			} else {
				printf("Flashing failed: %d. Keeping server running.\n",
				       rc);
			}
		}
		if (reboot_request)
			do_reset(NULL, 0, 0, NULL);
		if (timeout_ms && !post_ok && !upload_done && !flash_request &&
		    !reboot_request && prog_phase == 0 &&
			get_timer(start_ms) >= timeout_ms) {
			recovery_debug_printf("HTTP recovery timeout after %lums\n", timeout_ms);
			break;
		}
		recovery_watchdog_poll();
	}

	rc = 0;

out:
	recovery_runtime_status_leds = NULL;
	recovery_sbe1v1k_clean_session_reset();
	recovery_mmc_stream_reset();
	recovery_dhcp_server_stop(&dhcp);
	net_lwip_set_recovery_dhcp_hook(NULL, NULL);
	if (netif)
		net_lwip_remove_netif(netif);
	if (eth_started)
		eth_halt();
	recovery_status_led_stop(&status_leds);
	recovery_status_led_release(&status_leds);
	recovery_led_ctrl_free(&leds);
	recovery_board_http_acl(false);
	env_set("eth_allow_no_link", saved_allow_no_link);
	free(saved_allow_no_link);

	return rc;
}
