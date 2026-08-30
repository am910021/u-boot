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
#define FREEBSD_REQUEST_SIZE	512
#define FREEBSD_MAX_ENTRIES	96
#define FREEBSD_TARGETS_SIZE	(FREEBSD_MAX_ENTRIES * 16)

enum freebsd_request_key {
	FREEBSD_REQUEST_DEFAULT,
	FREEBSD_REQUEST_TITLE,
	FREEBSD_REQUEST_DELAY,
	FREEBSD_REQUEST_LOGO_DELAY,
	FREEBSD_REQUEST_KEYS,
};

static const char *const freebsd_request_names[FREEBSD_REQUEST_KEYS] = {
	[FREEBSD_REQUEST_DEFAULT] = "freebsd_default_boot",
	[FREEBSD_REQUEST_TITLE] = "bootmenu_title",
	[FREEBSD_REQUEST_DELAY] = "bootmenu_delay",
	[FREEBSD_REQUEST_LOGO_DELAY] = "logo_delay",
};

struct freebsd_request {
	char value[FREEBSD_REQUEST_KEYS][96];
	bool present[FREEBSD_REQUEST_KEYS];
};

static const char *freebsd_parse_number(const char *p, unsigned int max,
					bool zero_ok)
{
	unsigned int value = 0;
	unsigned int digit;

	if (!isdigit(*p))
		return NULL;
	do {
		digit = *p++ - '0';
		if (value > (max - digit) / 10)
			return NULL;
		value = value * 10 + digit;
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

static bool freebsd_valid_delay(const char *value)
{
	const char *end = freebsd_parse_number(value, 99, true);

	return end && !*end;
}

static bool freebsd_valid_title(const char *value)
{
	const char *p;
	size_t len = strlen(value);

	if (!len || len > 80)
		return false;
	for (p = value; *p; p++) {
		if (!isprint((unsigned char)*p))
			return false;
	}

	return true;
}

static int freebsd_request_set(struct freebsd_request *request,
			       char *name, const char *value)
{
	int key;

	for (key = 0; key < FREEBSD_REQUEST_KEYS; key++) {
		if (!strcmp(name, freebsd_request_names[key]))
			break;
	}
	if (key == FREEBSD_REQUEST_KEYS || request->present[key] ||
	    strlen(value) >= sizeof(request->value[key]))
		return -EINVAL;
	if ((key == FREEBSD_REQUEST_DEFAULT && !freebsd_valid_target(value)) ||
	    (key == FREEBSD_REQUEST_TITLE && !freebsd_valid_title(value)) ||
	    ((key == FREEBSD_REQUEST_DELAY ||
	      key == FREEBSD_REQUEST_LOGO_DELAY) && !freebsd_valid_delay(value)))
		return -EINVAL;

	strlcpy(request->value[key], value, sizeof(request->value[key]));
	request->present[key] = true;
	return 0;
}

static int freebsd_read_request(struct blk_desc *desc, int part,
				struct freebsd_request *parsed)
{
	char request[FREEBSD_REQUEST_SIZE];
	loff_t actread;
	loff_t size;
	char *cursor;
	char *equal;
	char *line;
	bool any = false;

	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_size(FREEBSD_REQUEST_PATH, &size) ||
	    size <= 0 ||
	    size >= sizeof(request))
		return -ENOENT;
	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_read(FREEBSD_REQUEST_PATH, map_to_sysmem(request), 0, size,
		    &actread) ||
	    actread != size)
		return -EIO;
	if (memchr(request, '\0', size))
		return -EINVAL;

	request[size] = '\0';
	memset(parsed, 0, sizeof(*parsed));
	cursor = request;
	while ((line = strsep(&cursor, "\n")) != NULL) {
		size_t len = strlen(line);

		if (len && line[len - 1] == '\r')
			line[--len] = '\0';
		if (!len)
			continue;
		equal = strchr(line, '=');
		if (!equal || equal == line)
			return -EINVAL;
		*equal++ = '\0';
		if (freebsd_request_set(parsed, line, equal))
			return -EINVAL;
		any = true;
	}

	return any ? 0 : -EINVAL;
}

static int freebsd_apply_request(struct blk_desc *desc, int part)
{
	struct freebsd_request request;
	char *saved[FREEBSD_REQUEST_KEYS] = {};
	bool changed[FREEBSD_REQUEST_KEYS] = {};
	const char *current;
	int key;
	int ret;

	ret = freebsd_read_request(desc, part, &request);
	if (ret)
		return ret;

	for (key = 0; key < FREEBSD_REQUEST_KEYS; key++) {
		if (!request.present[key])
			continue;
		current = env_get(freebsd_request_names[key]);
		if (current && !strcmp(current, request.value[key]))
			continue;
		if (current) {
			saved[key] = strdup(current);
			if (!saved[key]) {
				ret = -ENOMEM;
				goto out;
			}
		}
		changed[key] = true;
	}

	for (key = 0; key < FREEBSD_REQUEST_KEYS; key++) {
		if (changed[key] && env_set(freebsd_request_names[key],
					    request.value[key])) {
			ret = -ENOMEM;
			goto restore;
		}
	}
	ret = 0;
	for (key = 0; key < FREEBSD_REQUEST_KEYS; key++) {
		if (changed[key]) {
			ret = env_save();
			break;
		}
	}
	if (ret)
		goto restore;

	printf("Accepted %s from %s%d:%d\n", FREEBSD_REQUEST_PATH,
	       blk_get_uclass_name(desc->uclass_id), desc->devnum, part);
	goto out;

restore:
	for (key = 0; key < FREEBSD_REQUEST_KEYS; key++) {
		if (changed[key])
			env_set(freebsd_request_names[key], saved[key]);
	}
	printf("Keeping %s on %s%d:%d: environment update failed\n",
	       FREEBSD_REQUEST_PATH,
	       blk_get_uclass_name(desc->uclass_id), desc->devnum, part);

out:
	for (key = 0; key < FREEBSD_REQUEST_KEYS; key++)
		free(saved[key]);
	return ret;
}

static bool freebsd_request_on_desc(struct blk_desc *desc)
{
	struct disk_partition info;
	int ret;
	int part;

	for (part = 1; part <= MAX_SEARCH_PARTITIONS; part++) {
		if (part_get_info(desc, part, &info))
			continue;
		ret = freebsd_apply_request(desc, part);
		if (!ret)
			return true;
		if (ret == -EINVAL)
			printf("Ignoring invalid %s on %s%d:%d\n",
			       FREEBSD_REQUEST_PATH,
			       blk_get_uclass_name(desc->uclass_id),
			       desc->devnum, part);
	}

	return false;
}

static void freebsd_remove_request(struct blk_desc *desc, int part)
{
	if (fs_set_blk_dev_with_part(desc, part) ||
	    !fs_exists(FREEBSD_REQUEST_PATH))
		return;
	if (fs_unlink(FREEBSD_REQUEST_PATH))
		printf("Could not remove %s from %s%d:%d\n",
		       FREEBSD_REQUEST_PATH,
		       blk_get_uclass_name(desc->uclass_id), desc->devnum, part);
}

static void freebsd_remove_request_uclass(enum uclass_id id)
{
	struct disk_partition info;
	struct blk_desc *desc;
	int devnum;
	int part;
	int max;

	max = blk_find_max_devnum(id);
	for (devnum = 0; devnum <= max; devnum++) {
		desc = blk_get_devnum_by_uclass_id(id, devnum);
		if (!desc)
			continue;
		for (part = 1; part <= MAX_SEARCH_PARTITIONS; part++) {
			if (!part_get_info(desc, part, &info))
				freebsd_remove_request(desc, part);
		}
	}
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
			     char *targets, size_t targets_size,
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
		 "echo \"RK3588-BOOT-TARGET "
		 "${freebsd_iface}${freebsd_devpart}\"; "
		 "run boot_freebsd_target", ifname, devpart);
	snprintf(value, sizeof(value), "FreeBSD - %s (%s)=%s",
		 label, target, command);
	if (strlen(targets) + strlen(target) + (targets[0] ? 1 : 0) >=
	    targets_size)
		return -ENOSPC;

	if (env_set(name, value))
		return -ENOMEM;
	if (targets[0])
		strlcat(targets, ",", targets_size);
	strlcat(targets, target, targets_size);
	if (wanted && !strcmp(wanted, target))
		*default_index = *index;

	(*index)++;
	return 0;
}

static void freebsd_scan_desc(struct blk_desc *desc, const char *label,
			      const char *wanted, char *targets,
			      size_t targets_size, int *index,
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
				      wanted, targets, targets_size, index,
				      default_index))
			return;
	}
}

static void freebsd_scan_uclass(enum uclass_id id, const char *label,
				const char *wanted, char *targets,
				size_t targets_size, int *index,
				int *default_index)
{
	struct blk_desc *desc;
	int devnum;
	int max;

	max = blk_find_max_devnum(id);
	for (devnum = 0; devnum <= max; devnum++) {
		desc = blk_get_devnum_by_uclass_id(id, devnum);
		if (desc)
			freebsd_scan_desc(desc, label, wanted, targets,
					  targets_size, index, default_index);
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

static void freebsd_scan_mmc(bool removable, const char *wanted, char *targets,
			     size_t targets_size, int *index,
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
				   targets, targets_size, index, default_index);
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
	static const char *const runtime_defaults[] = {
		"boot_freebsd_target",
	};
	const char *wanted;
	char targets[FREEBSD_TARGETS_SIZE] = {};
	char value[16];
	int default_index = 0;
	int index = 0;
	bool usb_ready;
	bool nvme_ready;
	bool scsi_ready;
	bool request_applied;

	/* Do not let a saved environment pin an old firmware implementation. */
	env_set_default_vars(ARRAY_SIZE(runtime_defaults),
			     (char * const *)runtime_defaults, 0);
	freebsd_clear_menu();
	mmc_initialize(NULL);
	usb_ready = IS_ENABLED(CONFIG_USB_STORAGE) && !usb_init() &&
		usb_stor_scan(1) >= 0;
	nvme_ready = IS_ENABLED(CONFIG_NVME) && !nvme_scan_namespace();
	scsi_ready = IS_ENABLED(CONFIG_SCSI) && !scsi_scan(false);

	request_applied = freebsd_request_mmc(false) ||
		freebsd_request_mmc(true) ||
		(usb_ready && freebsd_request_uclass(UCLASS_USB)) ||
		(nvme_ready && freebsd_request_uclass(UCLASS_NVME)) ||
		(scsi_ready && freebsd_request_uclass(UCLASS_SCSI));
	if (request_applied) {
		freebsd_remove_request_uclass(UCLASS_MMC);
		if (usb_ready)
			freebsd_remove_request_uclass(UCLASS_USB);
		if (nvme_ready)
			freebsd_remove_request_uclass(UCLASS_NVME);
		if (scsi_ready)
			freebsd_remove_request_uclass(UCLASS_SCSI);
	}

	wanted = env_get("freebsd_default_boot");
	freebsd_scan_mmc(false, wanted, targets, sizeof(targets), &index,
			 &default_index);
	freebsd_scan_mmc(true, wanted, targets, sizeof(targets), &index,
			 &default_index);

	if (usb_ready)
		freebsd_scan_uclass(UCLASS_USB, "USB", wanted, targets,
				    sizeof(targets), &index, &default_index);
	if (nvme_ready)
		freebsd_scan_uclass(UCLASS_NVME, "NVMe", wanted, targets,
				    sizeof(targets), &index, &default_index);
	if (scsi_ready)
		freebsd_scan_uclass(UCLASS_SCSI, "SATA/SCSI", wanted, targets,
				    sizeof(targets), &index, &default_index);
	if (env_set("freebsd_boot_targets", targets))
		return -ENOMEM;

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
