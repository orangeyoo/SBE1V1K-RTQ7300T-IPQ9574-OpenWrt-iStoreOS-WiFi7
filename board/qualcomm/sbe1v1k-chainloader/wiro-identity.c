// SPDX-License-Identifier: GPL-2.0+
/* SBE1V1K FULL: runtime identity for explicitly marked vendor kernels.
 * Reads SMEM and ART only. Never writes eMMC, environment or calibration.
 */
#include <command.h>
#include <memalign.h>
#include <dm.h>
#include <dm/uclass.h>
#include <fdt_support.h>
#include <smem.h>
#include <part.h>
#include <net.h>
#include <linux/err.h>
#include <asm/global_data.h>
#include <asm/cache.h>

DECLARE_GLOBAL_DATA_PTR;

struct sbe_identity {
	u32 cpu, major, minor;
	u8 mac[4][6];
};

static int sbe_read_identity(struct sbe_identity *id)
{
	struct udevice *dev;
	struct blk_desc *desc;
	struct disk_partition part;
	const __le32 *info;
	size_t size = 0;
	u32 version;
	int ret, i;
	ALLOC_CACHE_ALIGN_BUFFER(u8, art, 512);
	static const struct {
		int number;
		const char *name;
		lbaint_t start, size;
	} layout[] = {
		{20, "0:ART", 37410, 2048},
		{25, "chainloader", 81954, 8192},
		{26, "kernel", 90146, 65536},
		{27, "rootfs", 155682, 2097152},
		{28, "rootfs_data", 2252834, 13017021},
	};

	if (!of_machine_is_compatible("askey,sbe1v1k"))
		return -ENODEV;
	ret = uclass_get_device(UCLASS_SMEM, 0, &dev);
	if (ret)
		return ret;
	info = smem_get(dev, -1, 137, &size);
	if (IS_ERR_OR_NULL(info) || size < 44)
		return -ENODATA;
	/* The supported socinfo formats all share this fixed v1 prefix. */
	if (!le32_to_cpu(info[0]) || le32_to_cpu(info[0]) > 0xffff)
		return -EINVAL;
	id->cpu = le32_to_cpu(info[1]);
	version = le32_to_cpu(info[2]);
	id->major = version >> 16;
	id->minor = version & 0xffff;
	/* Scope is IPQ9570 major1, not every IPQ95xx board or future stepping. */
	if (id->cpu != 513 || id->major != 1)
		return -ENOTSUPP;
	ret = blk_get_device_by_str("mmc", "0", &desc);
	if (ret < 0)
		return ret;
	if (desc->blksz != 512 || desc->lba != 15269888)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(layout); i++) {
		ret = part_get_info(desc, layout[i].number, &part);
		if (ret || strcmp(part.name, layout[i].name) ||
		    part.start != layout[i].start || part.size != layout[i].size)
			return -EINVAL;
	}
	if (blk_dread(desc, layout[0].start, 1, art) != 1)
		return -EIO;
	for (i = 0; i < 4; i++) {
		if (!is_valid_ethaddr(art + i * 6))
			return -EINVAL;
		memcpy(id->mac[i], art + i * 6, 6);
	}
	return 0;
}

static int sbe_set_le32(void *blob, const char *name, u32 value)
{
	__le32 le = cpu_to_le32(value);

	return fdt_setprop(blob, 0, name, &le, sizeof(le));
}

int wiro_identity_fixup(void *blob, struct bd_info *bd)
{
	struct sbe_identity id;
	u64 start[CONFIG_NR_DRAM_BANKS], size[CONFIG_NR_DRAM_BANKS];
	const char *marker;
	char path[24];
	int len, ret, i, node, banks = 0;

	/* The Wiro-lineage FITs carry no marker property; gate on the board's
	 * root compatible instead. Foreign DTBs stay untouched (no-op). */
	if (fdt_node_check_compatible(blob, 0, "spectrum,sbe1v1k"))
		return 0;
	ret = sbe_read_identity(&id);
	if (ret) {
		printf("SBE Wiro: identity/layout validation failed (%d); refusing boot\n", ret);
		return ret;
	}
	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		if (!bd->bi_dram[i].size)
			continue;
		start[banks] = bd->bi_dram[i].start;
		size[banks++] = bd->bi_dram[i].size;
	}
	/* Supported FULL hardware has one 2GiB bank, obtained from prior boot DT. */
	if (banks != 1 || start[0] != 0x40000000ULL || size[0] != 0x80000000ULL)
		return -EINVAL;
	ret = fdt_fixup_memory_banks(blob, start, size, banks);
	if (ret)
		return ret;
	if ((ret = sbe_set_le32(blob, "soc_version_major", id.major)) ||
	    (ret = sbe_set_le32(blob, "soc_version_minor", id.minor)) ||
	    (ret = sbe_set_le32(blob, "cpu_type", id.cpu)) ||
	    /* Board/boot-medium identifiers, not per-device identity. */
	    (ret = sbe_set_le32(blob, "flash_type", 5)) ||
	    (ret = sbe_set_le32(blob, "machid", 0x08773001)))
		return ret;
	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "/soc/dp%d", i + 1);
		node = fdt_path_offset(blob, path);
		if (node < 0)
			return node;
		ret = fdt_setprop(blob, node, "local-mac-address", id.mac[i], 6);
		if (ret)
			return ret;
	}
	printf("SBE Wiro: dynamic SoC %u rev %u.%u; ART MACs and RAM applied\n",
	       id.cpu, id.major, id.minor);
	return 0;
}

static int do_sbe_identity(struct cmd_tbl *cmdtp, int flag, int argc,
			   char *const argv[])
{
	struct sbe_identity id;
	int ret = sbe_read_identity(&id);

	if (ret) {
		printf("SBE Wiro identity read-only preflight failed: %d\n", ret);
		return CMD_RET_FAILURE;
	}
	printf("SBE Wiro identity read-only preflight PASS: CPU %u revision %u.%u, FULL layout, four valid ART MACs\n",
	       id.cpu, id.major, id.minor);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(sbe_identity, 1, 0, do_sbe_identity,
	   "read-only SBE FULL SoC/ART/layout preflight", "");
