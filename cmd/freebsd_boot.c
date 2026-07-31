// SPDX-License-Identifier: GPL-2.0+

#include <blk.h>
#include <command.h>
#include <env.h>
#include <fs.h>
#include <malloc.h>
#include <mmc.h>
#include <nvme.h>
#include <part.h>
#include <scsi.h>
#include <usb.h>

#define FREEBSD_LOADER_PATH	"/EFI/FreeBSD/loader.efi"
#define FREEBSD_MAX_ENTRIES	96

static void freebsd_clear_menu(void)
{
	char name[16];
	int i;

	for (i = 0; i < 99; i++) {
		snprintf(name, sizeof(name), "bootmenu_%d", i);
		env_set(name, NULL);
	}
}

static int freebsd_add_entry(const char *label, const char *ifname,
			     int devnum, int part, const char *wanted,
			     int *index, int *default_index)
{
	char command[192];
	char devpart[24];
	char name[16];
	char target[32];
	char value[256];

	if (*index >= FREEBSD_MAX_ENTRIES)
		return -ENOSPC;

	snprintf(devpart, sizeof(devpart), "%d:%d", devnum, part);
	snprintf(target, sizeof(target), "%s%s", ifname, devpart);
	snprintf(name, sizeof(name), "bootmenu_%d", *index);
	snprintf(command, sizeof(command),
		 "setenv freebsd_iface %s; setenv freebsd_devpart %s; "
		 "run boot_freebsd_target", ifname, devpart);
	snprintf(value, sizeof(value), "FreeBSD - %s (%s)=%s",
		 label, target, command);

	if (env_set(name, value))
		return -ENOMEM;
	if (wanted && !strcmp(wanted, target))
		*default_index = *index;

	(*index)++;
	return 0;
}

static void freebsd_scan_desc(struct blk_desc *desc, const char *label,
			      const char *wanted, int *index,
			      int *default_index)
{
	struct disk_partition info;
	const char *ifname = blk_get_uclass_name(desc->uclass_id);
	int part;

	for (part = 1; part <= MAX_SEARCH_PARTITIONS; part++) {
		if (part_get_info(desc, part, &info))
			continue;
		if (fs_set_blk_dev_with_part(desc, part))
			continue;
		if (!fs_exists(FREEBSD_LOADER_PATH))
			continue;
		if (freebsd_add_entry(label, ifname, desc->devnum, part,
				      wanted, index, default_index))
			return;
	}
}

static void freebsd_scan_uclass(enum uclass_id id, const char *label,
				const char *wanted, int *index,
				int *default_index)
{
	struct blk_desc *desc;
	int devnum;
	int max;

	max = blk_find_max_devnum(id);
	for (devnum = 0; devnum <= max; devnum++) {
		desc = blk_get_devnum_by_uclass_id(id, devnum);
		if (desc)
			freebsd_scan_desc(desc, label, wanted, index,
					   default_index);
	}
}

static void freebsd_scan_mmc(bool removable, const char *wanted, int *index,
			     int *default_index)
{
	struct blk_desc *desc;
	int devnum;
	int max;

	max = blk_find_max_devnum(UCLASS_MMC);
	for (devnum = 0; devnum <= max; devnum++) {
		desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, devnum);
		if (!desc || !!desc->removable != removable)
			continue;
		freebsd_scan_desc(desc, removable ? "SD" : "eMMC", wanted,
				   index, default_index);
	}
}

static int freebsd_build_menu(void)
{
	const char *wanted = env_get("freebsd_default_boot");
	char value[16];
	int default_index = 0;
	int index = 0;

	freebsd_clear_menu();
	mmc_initialize(NULL);
	freebsd_scan_mmc(false, wanted, &index, &default_index);
	freebsd_scan_mmc(true, wanted, &index, &default_index);

	if (IS_ENABLED(CONFIG_USB_STORAGE) && !usb_init())
		freebsd_scan_uclass(UCLASS_USB, "USB", wanted, &index,
				   &default_index);
	if (IS_ENABLED(CONFIG_NVME) && !nvme_scan_namespace())
		freebsd_scan_uclass(UCLASS_NVME, "NVMe", wanted, &index,
				   &default_index);
	if (IS_ENABLED(CONFIG_SCSI) && !scsi_scan(false))
		freebsd_scan_uclass(UCLASS_SCSI, "SATA/SCSI", wanted, &index,
				   &default_index);

	snprintf(value, sizeof(value), "%d", default_index);
	env_set("bootmenu_default", value);

	snprintf(value, sizeof(value), "bootmenu_%d", index);
	env_set(value, "U-Boot CLI=exit");

	printf("Detected %d FreeBSD boot target%s\n",
	       index, index == 1 ? "" : "s");
	return 0;
}

static int do_freebsdboot(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	return freebsd_build_menu() ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	freebsdboot, 1, 0, do_freebsdboot,
	"build a FreeBSD boot menu from detected EFI loaders",
	""
);
