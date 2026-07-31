// SPDX-License-Identifier: GPL-2.0+

#include <blk.h>
#include <command.h>
#include <env.h>
#include <fs.h>
#include <malloc.h>
#include <mapmem.h>
#include <mmc.h>
#include <nvme.h>
#include <part.h>
#include <scsi.h>
#include <usb.h>
#include <linux/ctype.h>

#define FREEBSD_LOADER_PATH	"/EFI/FreeBSD/loader.efi"
#define FREEBSD_REQUEST_PATH	"/uboot-env.request"
#define FREEBSD_REQUEST_PREFIX	"freebsd_default_boot="
#define FREEBSD_MAX_ENTRIES	96

static const char *freebsd_parse_number(const char *p, unsigned int max,
					bool zero_ok)
{
	unsigned int value = 0;

	if (!isdigit(*p))
		return NULL;
	do {
		value = value * 10 + *p++ - '0';
		if (value > max)
			return NULL;
	} while (isdigit(*p));

	return value || zero_ok ? p : NULL;
}

static bool freebsd_valid_target(const char *value)
{
	static const char *const ifaces[] = {
		"mmc", "usb", "nvme", "scsi",
	};
	const char *p;
	int i;

	if (!strcmp(value, "auto"))
		return true;

	for (i = 0; i < ARRAY_SIZE(ifaces); i++) {
		p = value;
		if (strncmp(p, ifaces[i], strlen(ifaces[i])))
			continue;
		p += strlen(ifaces[i]);
		p = freebsd_parse_number(p, 255, true);
		if (!p || *p++ != ':')
			return false;
		p = freebsd_parse_number(p, MAX_SEARCH_PARTITIONS, false);
		if (!p)
			return false;
		return !*p;
	}

	return false;
}

static int freebsd_read_request(struct blk_desc *desc, int part,
				char *value, size_t value_size)
{
	char request[96];
	loff_t actread;
	loff_t size;
	char *end;

	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_size(FREEBSD_REQUEST_PATH, &size) ||
	    size <= strlen(FREEBSD_REQUEST_PREFIX) ||
	    size >= sizeof(request))
		return -ENOENT;
	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_read(FREEBSD_REQUEST_PATH, map_to_sysmem(request), 0, size,
		    &actread) ||
	    actread != size)
		return -EIO;

	request[size] = '\0';
	end = request + size;
	while (end > request && (end[-1] == '\n' || end[-1] == '\r'))
		*--end = '\0';

	if (strncmp(request, FREEBSD_REQUEST_PREFIX,
		    strlen(FREEBSD_REQUEST_PREFIX)))
		return -EINVAL;
	strlcpy(value, request + strlen(FREEBSD_REQUEST_PREFIX), value_size);

	return freebsd_valid_target(value) ? 0 : -EINVAL;
}

static int freebsd_apply_request(struct blk_desc *desc, int part)
{
	const char *current;
	char *saved = NULL;
	char value[32];
	int ret;

	ret = freebsd_read_request(desc, part, value, sizeof(value));
	if (ret)
		return ret;

	current = env_get("freebsd_default_boot");
	if (current) {
		saved = strdup(current);
		if (!saved)
			return -ENOMEM;
	}

	ret = env_set("freebsd_default_boot", value);
	if (!ret)
		ret = env_save();
	if (ret) {
		env_set("freebsd_default_boot", saved);
		free(saved);
		printf("Keeping %s on %s%d:%d: saveenv failed\n",
		       FREEBSD_REQUEST_PATH,
		       blk_get_uclass_name(desc->uclass_id), desc->devnum, part);
		return ret;
	}
	free(saved);

	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_unlink(FREEBSD_REQUEST_PATH)) {
		printf("Applied %s=%s; could not remove %s from %s%d:%d\n",
		       "freebsd_default_boot", value, FREEBSD_REQUEST_PATH,
		       blk_get_uclass_name(desc->uclass_id), desc->devnum, part);
	} else {
		printf("Applied %s=%s from %s%d:%d\n",
		       "freebsd_default_boot", value,
		       blk_get_uclass_name(desc->uclass_id), desc->devnum, part);
	}

	return 0;
}

static bool freebsd_request_on_desc(struct blk_desc *desc)
{
	struct disk_partition info;
	int part;

	for (part = 1; part <= MAX_SEARCH_PARTITIONS; part++) {
		if (!part_get_info(desc, part, &info) &&
		    !freebsd_apply_request(desc, part))
			return true;
	}

	return false;
}

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

static bool freebsd_request_uclass(enum uclass_id id)
{
	struct blk_desc *desc;
	int devnum;
	int max;

	max = blk_find_max_devnum(id);
	for (devnum = 0; devnum <= max; devnum++) {
		desc = blk_get_devnum_by_uclass_id(id, devnum);
		if (desc && freebsd_request_on_desc(desc))
			return true;
	}

	return false;
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

static bool freebsd_request_mmc(bool removable)
{
	struct blk_desc *desc;
	int devnum;
	int max;

	max = blk_find_max_devnum(UCLASS_MMC);
	for (devnum = 0; devnum <= max; devnum++) {
		desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, devnum);
		if (!desc || !!desc->removable != removable)
			continue;
		if (freebsd_request_on_desc(desc))
			return true;
	}

	return false;
}

static int freebsd_build_menu(void)
{
	const char *wanted;
	char value[16];
	int default_index = 0;
	int index = 0;
	bool usb_ready;
	bool nvme_ready;
	bool scsi_ready;

	freebsd_clear_menu();
	mmc_initialize(NULL);
	usb_ready = IS_ENABLED(CONFIG_USB_STORAGE) && !usb_init();
	nvme_ready = IS_ENABLED(CONFIG_NVME) && !nvme_scan_namespace();
	scsi_ready = IS_ENABLED(CONFIG_SCSI) && !scsi_scan(false);

	if (!freebsd_request_mmc(false) &&
	    !freebsd_request_mmc(true) &&
	    !(usb_ready && freebsd_request_uclass(UCLASS_USB)) &&
	    !(nvme_ready && freebsd_request_uclass(UCLASS_NVME)) &&
	    scsi_ready)
		freebsd_request_uclass(UCLASS_SCSI);

	wanted = env_get("freebsd_default_boot");
	freebsd_scan_mmc(false, wanted, &index, &default_index);
	freebsd_scan_mmc(true, wanted, &index, &default_index);

	if (usb_ready)
		freebsd_scan_uclass(UCLASS_USB, "USB", wanted, &index,
				   &default_index);
	if (nvme_ready)
		freebsd_scan_uclass(UCLASS_NVME, "NVMe", wanted, &index,
				   &default_index);
	if (scsi_ready)
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
