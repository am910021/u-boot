// SPDX-License-Identifier: GPL-2.0+

#include <blk.h>
#include <command.h>
#include <cli.h>
#include <dm.h>
#include <env.h>
#include <fs.h>
#include <init.h>
#include <malloc.h>
#include <mapmem.h>
#include <mmc.h>
#include <nvme.h>
#include <part.h>
#include <scsi.h>
#include <usb.h>
#include <wdt.h>
#include <asm/cache.h>
#include <linux/ctype.h>
#if IS_ENABLED(CONFIG_RK3588_FREEBSD_SPI_UPDATE)
#include <hexdump.h>
#include <spi_flash.h>
#include <u-boot/sha256.h>
#include <version.h>
#include <linux/sizes.h>
#endif

#define FREEBSD_LOADER_PATH	"/EFI/FreeBSD/loader.efi"
#define FREEBSD_MENU_NAME_PATH	"/uboot-menu-name"
#define FREEBSD_MENU_NAME_SIZE	32
#define FREEBSD_ENTRY_PATH	"/uboot-boot-entry.conf"
#define FREEBSD_ENTRY_SIZE	1024
#define FREEBSD_ENTRY_BOOT_PATH_SIZE	128
#define FREEBSD_REQUEST_PATH	"/uboot-env.request"
#define FREEBSD_REQUEST_SIZE	512
#define FREEBSD_MAX_ENTRIES	96
#define FREEBSD_TARGETS_SIZE	(FREEBSD_MAX_ENTRIES * 16)

#if IS_ENABLED(CONFIG_RK3588_FREEBSD_SPI_UPDATE)
#define FREEBSD_SPI_REQUEST_PATH	"/uboot-spi-update.request"
#define FREEBSD_SPI_IMAGE_PATH		"/firmware-update.bin"
#define FREEBSD_SPI_REQUEST_SIZE	512
#define FREEBSD_SPI_COMPAT_PREFIX	"RK3588-FW-COMPAT-V1:"
#define FREEBSD_SPI_VERSION_PREFIX	"RK3588-FW-VERSION-V1:"
#define FREEBSD_FW_TARGET_PREFIX		"RK3588-FW-TARGET-V1:"
#define FREEBSD_FW_TARGET_SECTOR_SIZE	512
#define FREEBSD_SPI_16M_UPDATE_SIZE	(16 * SZ_1M - SZ_512K)
#define FREEBSD_SPI_32M_UPDATE_SIZE	(32 * SZ_1M - SZ_512K)
#define FREEBSD_BOOT_HEADER_OFFSET	SZ_32K
#define FREEBSD_MMC_FIT_OFFSET		(8 * SZ_1M)

#if CONFIG_RK3588_FREEBSD_SPI_LAYOUT_MIB != 16 && \
    CONFIG_RK3588_FREEBSD_SPI_LAYOUT_MIB != 32
#error RK3588_FREEBSD_SPI_LAYOUT_MIB must be 16 or 32
#endif

static const char freebsd_spi_compat_marker[] =
	FREEBSD_SPI_COMPAT_PREFIX CONFIG_RK3588_FREEBSD_SPI_COMPAT;
static const char freebsd_spi_version_marker[] =
	FREEBSD_SPI_VERSION_PREFIX U_BOOT_VERSION;

struct freebsd_spi_update {
	struct blk_desc *desc;
	int part;
	void *image;
	size_t image_size;
	u8 digest[SHA256_SUM_LEN];
};

static int freebsd_remove_path(struct blk_desc *desc, int part,
			       const char *path);

static int freebsd_find_image_marker(const void *image, size_t image_size,
				     const char *prefix, size_t prefix_size,
				     size_t max_size,
				     const char **marker)
{
	const u8 *bytes = image;
	const char *found = NULL;
	size_t offset;

	for (offset = 0; offset + prefix_size < image_size; offset++) {
		const char *end;
		const char *p;
		size_t available;

		if (memcmp(bytes + offset, prefix, prefix_size))
			continue;
		if (found)
			return -EEXIST;
		available = min(max_size, image_size - offset);
		end = memchr(bytes + offset, '\0', available);
		if (!end || end == (const char *)bytes + offset + prefix_size)
			return -EINVAL;
		for (p = (const char *)bytes + offset; p < end; p++) {
			if (!isprint((unsigned char)*p))
				return -EINVAL;
		}
		found = (const char *)bytes + offset;
	}
	if (!found)
		return -ENOENT;
	*marker = found;
	return 0;
}

static int freebsd_check_spi_capacity(u32 layout_mib, size_t image_size)
{
	struct udevice *dev;
	struct spi_flash *flash;
	u32 expected = layout_mib * SZ_1M;
	int ret;

	ret = spi_flash_probe_bus_cs(CONFIG_SF_DEFAULT_BUS,
				     CONFIG_SF_DEFAULT_CS, &dev);
	if (ret)
		return ret;
	flash = dev_get_uclass_priv(dev);
	if (!flash)
		return -ENODEV;
	printf("SPI flash capacity: %u MiB; firmware layout: %u MiB\n",
	       flash->size / SZ_1M, layout_mib);
	return flash->size >= expected && image_size <= flash->size ? 0 :
		-EINVAL;
}

static bool freebsd_spi_16m_to_32m(const char *candidate, size_t image_size)
{
	static const char old_suffix[] = ":SPI:16M";
	static const char new_suffix[] = ":SPI:32M";
	size_t current_len = strlen(freebsd_spi_compat_marker);
	size_t suffix_len = sizeof(old_suffix) - 1;

	if (CONFIG_RK3588_FREEBSD_SPI_LAYOUT_MIB != 16 ||
	    image_size != FREEBSD_SPI_32M_UPDATE_SIZE ||
	    current_len < suffix_len ||
	    strcmp(freebsd_spi_compat_marker + current_len - suffix_len,
		   old_suffix) ||
	    strncmp(candidate, freebsd_spi_compat_marker,
		    current_len - suffix_len))
		return false;
	return !strcmp(candidate + current_len - suffix_len, new_suffix);
}

static const char *freebsd_firmware_storage(void)
{
	ulong storage = env_get_ulong("rk_boot_storage", 10, 0);

	if (storage == 1 || storage == 2)
		return "MMC";
	if (storage == 9)
		return "SPI";
	return NULL;
}

static int freebsd_check_firmware_target(const void *image, size_t image_size,
					 const char *storage, bool migrate)
{
	const char *compat = CONFIG_RK3588_FREEBSD_SPI_COMPAT;
	const char *board_end = strstr(compat, ":SPI:");
	const char *marker;
	char expected[96];
	size_t available;
	size_t board_len;

	if (!storage || !board_end || image_size < FREEBSD_FW_TARGET_SECTOR_SIZE)
		return -EINVAL;
	marker = image + image_size - FREEBSD_FW_TARGET_SECTOR_SIZE;
	available = FREEBSD_FW_TARGET_SECTOR_SIZE;
	if (memcmp(marker, FREEBSD_FW_TARGET_PREFIX,
		   sizeof(FREEBSD_FW_TARGET_PREFIX) - 1) ||
	    !memchr(marker, '\0', available))
		return -EINVAL;
	board_len = board_end - compat;
	if (snprintf(expected, sizeof(expected), "%s%.*s:%s:%uM",
		     FREEBSD_FW_TARGET_PREFIX, (int)board_len, compat, storage,
		     migrate ? 32 : CONFIG_RK3588_FREEBSD_SPI_LAYOUT_MIB) >=
	    sizeof(expected))
		return -E2BIG;
	return strcmp(marker, expected) ? -EINVAL : 0;
}

static bool freebsd_boot_header_valid(const u8 *header, const u8 *fit)
{
	static const u8 fit_magic[] = { 0xd0, 0x0d, 0xfe, 0xed };

	return !memcmp(header, "RKNS", 4) &&
		!memcmp(fit, fit_magic, sizeof(fit_magic));
}

static int freebsd_mmc_firmware_target(int devnum, struct blk_desc **desc)
{
	struct disk_partition info;
	struct blk_desc *disk;

	disk = blk_get_devnum_by_uclass_id(UCLASS_MMC, devnum);
	if (!disk || disk->blksz != 512 || part_get_info(disk, 1, &info) ||
	    strcmp(info.name, "rk3588_firmware") || info.start > 64 ||
	    info.start + info.size <
		CONFIG_RK3588_FREEBSD_SPI_LAYOUT_MIB * SZ_1M / 512)
		return -EINVAL;
	*desc = disk;
	return 0;
}

static bool freebsd_target_bootable(bool spi, int devnum, bool disabled)
{
	struct udevice *dev;
	struct spi_flash *flash;
	struct blk_desc *disk;
	u8 header[512], fit[512], marker[512];
	u32 fit_offset = spi ? CONFIG_SYS_SPI_U_BOOT_OFFS :
		FREEBSD_MMC_FIT_OFFSET;
	u32 marker_offset = CONFIG_ENV_OFFSET - 512;

	if (spi) {
		if (spi_flash_probe_bus_cs(CONFIG_SF_DEFAULT_BUS,
					   CONFIG_SF_DEFAULT_CS, &dev))
			return false;
		flash = dev_get_uclass_priv(dev);
		if (!flash || flash->size <
		    CONFIG_RK3588_FREEBSD_SPI_LAYOUT_MIB * SZ_1M ||
		    spi_flash_read(flash, FREEBSD_BOOT_HEADER_OFFSET, 512,
				   header) ||
		    spi_flash_read(flash, fit_offset, 512, fit) ||
		    spi_flash_read(flash, marker_offset, 512, marker))
			return false;
	} else {
		if (freebsd_mmc_firmware_target(devnum, &disk) ||
		    blk_dread(disk, FREEBSD_BOOT_HEADER_OFFSET / 512, 1,
			      header) != 1 ||
		    blk_dread(disk, fit_offset / 512, 1, fit) != 1 ||
		    blk_dread(disk, marker_offset / 512, 1, marker) != 1)
			return false;
	}
	return (disabled ? !memcmp(header, "\0\0\0\0", 4) :
		!memcmp(header, "RKNS", 4)) &&
		!memcmp(fit, "\xd0\x0d\xfe\xed", 4) &&
		!freebsd_check_firmware_target(marker, sizeof(marker),
					       spi ? "SPI" : "MMC", false);
}

static bool freebsd_other_bootable(bool target_spi, int target_mmc)
{
	int devnum;

	if (!target_spi && freebsd_target_bootable(true, 0, false))
		return true;
	for (devnum = 0; devnum < 4; devnum++) {
		if ((!target_spi && devnum == target_mmc) ||
		    !freebsd_target_bootable(false, devnum, false))
			continue;
		return true;
	}
	return false;
}

static int freebsd_update_mmc(struct freebsd_spi_update *update, void *verify)
{
	struct disk_partition info;
	struct blk_desc *target;
	lbaint_t first = 64;
	lbaint_t blocks = update->image_size / 512 - first;
	int devnum = mmc_get_env_dev();

	target = blk_get_devnum_by_uclass_id(UCLASS_MMC, devnum);
	if (!target || target->blksz != 512 || part_get_info(target, 1, &info) ||
	    strcmp(info.name, "rk3588_firmware") || info.start > first ||
	    info.start + info.size <
		CONFIG_RK3588_FREEBSD_SPI_LAYOUT_MIB * SZ_1M / 512) {
		printf("MMC update rejected: invalid rk3588_firmware partition\n");
		return -EINVAL;
	}
	if (freebsd_remove_path(update->desc, update->part,
				 FREEBSD_SPI_REQUEST_PATH)) {
		printf("MMC update cancelled: request is not one-shot\n");
		return -ECANCELED;
	}
	printf("Updating MMC firmware on mmc%d; do not remove power\n", devnum);
	if (blk_dwrite(target, first, blocks,
		       (u8 *)update->image + first * 512) != blocks ||
	    blk_dread(target, first, blocks,
		      (u8 *)verify + first * 512) != blocks ||
	    memcmp((u8 *)update->image + first * 512,
		   (u8 *)verify + first * 512, blocks * 512)) {
		printf("MMC UPDATE FAILED: do not reset; use Maskrom recovery\n");
		return -EIO;
	}
	memcpy(verify, update->image, first * 512);
	return 0;
}
#endif

enum freebsd_request_key {
	FREEBSD_REQUEST_DEFAULT,
	FREEBSD_REQUEST_TITLE,
	FREEBSD_REQUEST_DELAY,
	FREEBSD_REQUEST_LOGO_DELAY,
	FREEBSD_REQUEST_WATCHDOG_ENABLE,
	FREEBSD_REQUEST_WATCHDOG_TIMEOUT,
	FREEBSD_REQUEST_KEYS,
};

static const char *const freebsd_request_names[FREEBSD_REQUEST_KEYS] = {
	[FREEBSD_REQUEST_DEFAULT] = "freebsd_default_boot",
	[FREEBSD_REQUEST_TITLE] = "bootmenu_title",
	[FREEBSD_REQUEST_DELAY] = "bootmenu_delay",
	[FREEBSD_REQUEST_LOGO_DELAY] = "logo_delay",
	[FREEBSD_REQUEST_WATCHDOG_ENABLE] = "freebsd_watchdog_enable",
	[FREEBSD_REQUEST_WATCHDOG_TIMEOUT] = "freebsd_watchdog_timeout",
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

static bool freebsd_valid_watchdog_timeout(const char *value)
{
	return !strcmp(value, "60");
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
	      key == FREEBSD_REQUEST_LOGO_DELAY) && !freebsd_valid_delay(value)) ||
	    (key == FREEBSD_REQUEST_WATCHDOG_ENABLE &&
	     strcmp(value, "0") && strcmp(value, "1")) ||
	    (key == FREEBSD_REQUEST_WATCHDOG_TIMEOUT &&
	     !freebsd_valid_watchdog_timeout(value)))
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

static int freebsd_remove_path(struct blk_desc *desc, int part,
			       const char *path)
{
	if (fs_set_blk_dev_with_part(desc, part) ||
	    !fs_exists(path))
		return 0;
	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_unlink(path)) {
		printf("Could not remove %s from %s%d:%d\n", path,
		       blk_get_uclass_name(desc->uclass_id), desc->devnum, part);
		return -EIO;
	}

	return 0;
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
				freebsd_remove_path(desc, part,
						    FREEBSD_REQUEST_PATH);
		}
	}
}

#if IS_ENABLED(CONFIG_RK3588_FREEBSD_SPI_UPDATE)
static int freebsd_read_spi_request(struct blk_desc *desc, int part,
				    struct freebsd_spi_update *update)
{
	char request[FREEBSD_SPI_REQUEST_SIZE];
	bool have_version = false;
	bool have_size = false;
	bool have_digest = false;
	loff_t actread;
	loff_t image_size;
	loff_t size;
	char *cursor;
	char *equal;
	char *end;
	char *line;
	ulong value;
	u8 digest[SHA256_SUM_LEN];
	int ret = -EINVAL;

	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_size(FREEBSD_SPI_REQUEST_PATH, &size) || size <= 0 ||
	    size >= sizeof(request))
		return -ENOENT;
	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_read(FREEBSD_SPI_REQUEST_PATH, map_to_sysmem(request), 0, size,
		    &actread) || actread != size)
		return -EIO;
	if (memchr(request, '\0', size))
		return -EINVAL;

	request[size] = '\0';
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
		if (!strcmp(line, "version")) {
			if (have_version || strcmp(equal, "1"))
				return -EINVAL;
			have_version = true;
		} else if (!strcmp(line, "size")) {
			if (have_size)
				return -EINVAL;
			value = simple_strtoul(equal, &end, 10);
			if (!*equal || *end ||
			    (value != CONFIG_ENV_OFFSET &&
			     (CONFIG_RK3588_FREEBSD_SPI_LAYOUT_MIB != 16 ||
			      value != FREEBSD_SPI_32M_UPDATE_SIZE)))
				return -EINVAL;
			update->image_size = value;
			have_size = true;
		} else if (!strcmp(line, "sha256")) {
			if (have_digest || strlen(equal) != SHA256_SUM_LEN * 2 ||
			    hex2bin(update->digest, equal, SHA256_SUM_LEN))
				return -EINVAL;
			have_digest = true;
		} else {
			return -EINVAL;
		}
	}
	if (!have_version || !have_size || !have_digest)
		return -EINVAL;
	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_size(FREEBSD_SPI_IMAGE_PATH, &image_size) ||
	    image_size != update->image_size)
		return -EINVAL;

	update->image = memalign(ARCH_DMA_MINALIGN, image_size);
	if (!update->image)
		return -ENOMEM;
	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_read(FREEBSD_SPI_IMAGE_PATH, map_to_sysmem(update->image), 0,
		    image_size, &actread) || actread != image_size) {
		ret = -EIO;
		goto fail;
	}
	sha256_csum_wd(update->image, image_size, digest, CHUNKSZ_SHA256);
	if (memcmp(update->digest, digest, sizeof(digest)))
		goto fail;

	update->desc = desc;
	update->part = part;
	return 0;

fail:
	free(update->image);
	update->image = NULL;
	return ret;
}

static bool freebsd_find_spi_update_desc(struct blk_desc *desc,
					 struct freebsd_spi_update *update)
{
	struct disk_partition info;
	int part;
	int ret;

	for (part = 1; part <= MAX_SEARCH_PARTITIONS; part++) {
		if (part_get_info(desc, part, &info))
			continue;
		ret = freebsd_read_spi_request(desc, part, update);
		if (!ret)
			return true;
		if (ret != -ENOENT)
			printf("Ignoring invalid %s on %s%d:%d (%d)\n",
			       FREEBSD_SPI_REQUEST_PATH,
			       blk_get_uclass_name(desc->uclass_id), desc->devnum,
			       part, ret);
	}

	return false;
}

static bool freebsd_find_spi_update_uclass(enum uclass_id id,
					   struct freebsd_spi_update *update)
{
	struct blk_desc *desc;
	int devnum;
	int max;

	max = blk_find_max_devnum(id);
	for (devnum = 0; devnum <= max; devnum++) {
		desc = blk_get_devnum_by_uclass_id(id, devnum);
		if (desc && freebsd_find_spi_update_desc(desc, update))
			return true;
	}

	return false;
}

static int freebsd_migrate_spi_16m_to_32m(struct freebsd_spi_update *update,
					  void *verify, ulong verify_addr)
{
	const ulong old_end = 16 * SZ_1M;
	const ulong extension_size = update->image_size - old_end;
	const ulong new_env_offset = FREEBSD_SPI_32M_UPDATE_SIZE;
	const ulong env_bytes = 2 * CONFIG_ENV_SIZE;
	void *env_copy;
	ulong env_addr;
	ulong image_addr = map_to_sysmem(update->image);

	env_copy = memalign(ARCH_DMA_MINALIGN, env_bytes);
	if (!env_copy)
		return -ENOMEM;
	env_addr = map_to_sysmem(env_copy);

	printf("Migrating SPI firmware layout from 16 MiB to 32 MiB\n");
	if (run_commandf("sf update %lx %lx %lx", image_addr + old_end,
			 old_end, extension_size) ||
	    run_commandf("sf read %lx %lx %lx", verify_addr + old_end,
			 old_end, extension_size) ||
	    memcmp((u8 *)update->image + old_end, (u8 *)verify + old_end,
		   extension_size))
		goto fail;

	if (run_commandf("sf read %lx %x %lx", env_addr, CONFIG_ENV_OFFSET,
			 env_bytes) ||
	    run_commandf("sf update %lx %lx %lx", env_addr, new_env_offset,
			 env_bytes) ||
	    run_commandf("sf read %lx %lx %lx", verify_addr, new_env_offset,
			 env_bytes) ||
	    memcmp(env_copy, verify, env_bytes))
		goto fail;

	if (freebsd_remove_path(update->desc, update->part,
				 FREEBSD_SPI_REQUEST_PATH)) {
		printf("SPI migration cancelled: request is not one-shot\n");
		free(env_copy);
		return -ECANCELED;
	}

	if (run_commandf("sf update %lx 0 %lx", image_addr, old_end) ||
	    run_commandf("sf read %lx 0 %zx", verify_addr,
			 update->image_size))
		goto fail_after_request;
	free(env_copy);
	return 0;

fail:
	printf("SPI migration stopped before replacing the 16 MiB firmware\n");
	free(env_copy);
	return -EAGAIN;
fail_after_request:
	printf("SPI MIGRATION FAILED: do not reset; use Maskrom recovery\n");
	free(env_copy);
	return -EIO;
}

static int freebsd_apply_spi_update(bool usb_ready, bool nvme_ready,
				    bool scsi_ready)
{
	struct freebsd_spi_update update = {};
	const char *candidate_compat;
	const char *candidate_version;
	const char *storage;
	void *verify = NULL;
	u8 digest[SHA256_SUM_LEN];
	ulong image_addr;
	ulong verify_addr;
	bool found;
	bool migrate;
	u32 candidate_layout_mib;
	int ret = 0;

	found = freebsd_find_spi_update_uclass(UCLASS_MMC, &update) ||
		(usb_ready && freebsd_find_spi_update_uclass(UCLASS_USB,
							      &update)) ||
		(nvme_ready && freebsd_find_spi_update_uclass(UCLASS_NVME,
							       &update)) ||
		(scsi_ready && freebsd_find_spi_update_uclass(UCLASS_SCSI,
							       &update));
	if (!found)
		return 0;
	storage = freebsd_firmware_storage();

	ret = freebsd_find_image_marker(update.image, update.image_size,
					freebsd_spi_compat_marker,
					sizeof(FREEBSD_SPI_COMPAT_PREFIX) - 1,
					sizeof(freebsd_spi_compat_marker),
					&candidate_compat);
	migrate = !ret && freebsd_spi_16m_to_32m(candidate_compat,
						 update.image_size);
	if (ret || (strcmp(candidate_compat, freebsd_spi_compat_marker) &&
		    !migrate) ||
	    (!strcmp(candidate_compat, freebsd_spi_compat_marker) &&
	     update.image_size != CONFIG_ENV_OFFSET)) {
		if (ret)
			printf("SPI update rejected: invalid compatibility marker (%d)\n",
			       ret);
		else
			printf("SPI update rejected: firmware identity mismatch\n");
		ret = 0;
		goto out;
	}
	if (freebsd_check_firmware_target(update.image, update.image_size,
					 storage, migrate)) {
		printf("Firmware update rejected: image target does not match boot storage\n");
		goto out;
	}
	ret = freebsd_find_image_marker(update.image, update.image_size,
					freebsd_spi_version_marker,
					sizeof(FREEBSD_SPI_VERSION_PREFIX) - 1,
					160,
					&candidate_version);
	if (ret) {
		printf("SPI update rejected: invalid firmware version marker (%d)\n",
		       ret);
		ret = 0;
		goto out;
	}
	printf("Firmware compatibility: %s\n", candidate_compat +
	       sizeof(FREEBSD_SPI_COMPAT_PREFIX) - 1);
	printf("Current U-Boot: %s\n", freebsd_spi_version_marker +
	       sizeof(FREEBSD_SPI_VERSION_PREFIX) - 1);
	printf("Candidate U-Boot: %s\n", candidate_version +
	       sizeof(FREEBSD_SPI_VERSION_PREFIX) - 1);

	printf("Verified %s from %s%d:%d\n", FREEBSD_SPI_IMAGE_PATH,
	       blk_get_uclass_name(update.desc->uclass_id), update.desc->devnum,
	       update.part);
	verify_addr = env_get_hex("ramdisk_addr_r", 0);
	if (!verify_addr) {
		ret = -ENOMEM;
		goto out;
	}
	verify = map_sysmem(verify_addr, update.image_size);
	if (!strcmp(storage, "MMC")) {
		ret = freebsd_update_mmc(&update, verify);
		if (ret) {
			if (ret == -EINVAL || ret == -ECANCELED)
				ret = 0;
			goto out_verify;
		}
		goto verify_update;
	}
	if (run_command("sf probe", 0)) {
		ret = -EIO;
		goto out_verify;
	}
	candidate_layout_mib = migrate ? 32 :
		CONFIG_RK3588_FREEBSD_SPI_LAYOUT_MIB;
	ret = freebsd_check_spi_capacity(candidate_layout_mib,
					 update.image_size);
	if (ret) {
		printf("SPI update rejected: flash capacity mismatch (%d)\n", ret);
		ret = 0;
		goto out_verify;
	}
	if (migrate) {
		ret = freebsd_migrate_spi_16m_to_32m(&update, verify,
						     verify_addr);
		if (ret) {
			if (ret == -EAGAIN || ret == -ECANCELED)
				ret = 0;
			goto out_verify;
		}
		goto verify_update;
	}
	if (freebsd_remove_path(update.desc, update.part,
				 FREEBSD_SPI_REQUEST_PATH)) {
		printf("SPI update cancelled: request is not one-shot\n");
		goto out_verify;
	}

	image_addr = map_to_sysmem(update.image);
	printf("Updating SPI firmware; do not remove power\n");
	if (run_commandf("sf update %lx 0 %zx", image_addr,
			 update.image_size) ||
	    run_commandf("sf read %lx 0 %zx", verify_addr,
			 update.image_size)) {
		ret = -EIO;
		goto update_failed;
	}
verify_update:
	sha256_csum_wd(verify, update.image_size, digest, CHUNKSZ_SHA256);
	if (memcmp(update.digest, digest, sizeof(digest))) {
		ret = -EIO;
		goto update_failed;
	}

	printf("SPI firmware read-back verification passed; resetting\n");
	unmap_sysmem(verify);
	free(update.image);
	do_reset(NULL, 0, 0, NULL);
	return -EIO;

update_failed:
	printf("SPI UPDATE FAILED: do not reset; use the U-Boot CLI or Maskrom recovery\n");
out_verify:
	unmap_sysmem(verify);
out:
	free(update.image);
	return ret;
}

static int do_rkspi(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	const char *candidate_compat;
	const char *candidate_version;
	char confirmation[CONFIG_SYS_CBSIZE + 1] = { 0 };
	loff_t size, actread;
	void *image = NULL, *verify = NULL;
	ulong verify_addr;
	ulong image_addr;
	size_t offset;
	bool full_image;
	u8 digest[SHA256_SUM_LEN], readback[SHA256_SUM_LEN];
	int ret = CMD_RET_FAILURE;

	if (argc != 5 || strcmp(argv[1], "install"))
		return CMD_RET_USAGE;
	if (fs_set_blk_dev(argv[2], argv[3], FS_TYPE_ANY) ||
	    fs_size(argv[4], &size) ||
	    (size != CONFIG_ENV_OFFSET &&
	     size != CONFIG_ENV_OFFSET + SZ_512K)) {
		printf("SPI install rejected: expected a %u-byte preserve-env "
		       "or %u-byte full SPI image\n", CONFIG_ENV_OFFSET,
		       CONFIG_ENV_OFFSET + SZ_512K);
		return CMD_RET_FAILURE;
	}
	full_image = size != CONFIG_ENV_OFFSET;
	if (run_command("sf probe", 0) ||
	    freebsd_check_spi_capacity(CONFIG_RK3588_FREEBSD_SPI_LAYOUT_MIB,
					size)) {
		puts("SPI install rejected: flash capacity/layout mismatch\n");
		return CMD_RET_FAILURE;
	}
	image = memalign(ARCH_DMA_MINALIGN, size);
	verify_addr = env_get_hex("ramdisk_addr_r", 0);
	if (!image || !verify_addr) {
		puts("SPI install rejected: insufficient memory\n");
		goto out;
	}
	verify = map_sysmem(verify_addr, size);
	if (fs_set_blk_dev(argv[2], argv[3], FS_TYPE_ANY) ||
	    fs_read(argv[4], map_to_sysmem(image), 0, size, &actread) ||
	    actread != size) {
		puts("SPI install rejected: cannot read image\n");
		goto out;
	}
	for (offset = CONFIG_ENV_OFFSET; full_image && offset < size;
	     offset++) {
		if (((u8 *)image)[offset] != 0xff) {
			puts("SPI install rejected: environment reserve is not blank\n");
			goto out;
		}
	}
	if (freebsd_find_image_marker(image, size, freebsd_spi_compat_marker,
				       sizeof(FREEBSD_SPI_COMPAT_PREFIX) - 1,
				       sizeof(freebsd_spi_compat_marker),
				       &candidate_compat) ||
	    strcmp(candidate_compat, freebsd_spi_compat_marker) ||
	    freebsd_check_firmware_target(image, CONFIG_ENV_OFFSET, "SPI",
					  false) ||
	    freebsd_find_image_marker(image, size, freebsd_spi_version_marker,
				       sizeof(FREEBSD_SPI_VERSION_PREFIX) - 1,
				       160, &candidate_version)) {
		puts("SPI install rejected: board, layout, or image marker mismatch\n");
		goto out;
	}
	if (!freebsd_boot_header_valid((u8 *)image +
					FREEBSD_BOOT_HEADER_OFFSET,
					(u8 *)image +
					CONFIG_SYS_SPI_U_BOOT_OFFS)) {
		puts("SPI install rejected: invalid boot header or FIT\n");
		goto out;
	}
	sha256_csum_wd(image, size, digest, CHUNKSZ_SHA256);
	printf("Candidate: %s\n", candidate_version +
	       sizeof(FREEBSD_SPI_VERSION_PREFIX) - 1);
	printf("Install %lld bytes to SPI NOR, %s its environment. "
	       "Future boots will prefer SPI.\n", (long long)size,
	       full_image ? "resetting" : "preserving");
	if (cli_readline_into_buffer("Type INSTALL SPI to continue: ",
				     confirmation, 0) < 0 ||
	    strcmp(confirmation, "INSTALL SPI")) {
		puts("SPI install cancelled\n");
		goto out;
	}
	image_addr = map_to_sysmem(image);
	puts("Writing SPI firmware; do not remove power\n");
	if (run_commandf("sf update %lx 0 %zx", image_addr,
			 (size_t)size) ||
	    run_commandf("sf read %lx 0 %zx", verify_addr,
			 (size_t)size)) {
		puts("SPI install failed: do not reset; use Maskrom recovery\n");
		goto out;
	}
	sha256_csum_wd(verify, size, readback, CHUNKSZ_SHA256);
	if (memcmp(digest, readback, sizeof(digest))) {
		puts("SPI read-back mismatch: do not reset; use Maskrom recovery\n");
		goto out;
	}
	puts("SPI install verified; reboot manually when ready\n");
	ret = CMD_RET_SUCCESS;
out:
	if (verify)
		unmap_sysmem(verify);
	free(image);
	return ret;
}

U_BOOT_CMD(
	rkspi, 5, 0, do_rkspi,
	"manually install board-matched SPI firmware",
	"install <interface> <dev:partition> <SPI image path>"
);

static int freebsd_disable_boot_target(bool spi, int devnum)
{
	struct udevice *dev;
	struct spi_flash *flash;
	struct blk_desc *disk;
	char confirmation[CONFIG_SYS_CBSIZE + 1] = { 0 };
	u8 *buffer, *readback;
	size_t length = spi ? SZ_4K : 512;
	int ret = CMD_RET_FAILURE;

	if (!freebsd_target_bootable(spi, devnum, false)) {
		puts("Boot disable rejected: target is not a matching bootable image\n");
		return ret;
	}
	if (!freebsd_other_bootable(spi, devnum)) {
		puts("Boot disable rejected: no matching SPI/eMMC/SD fallback found\n");
		return ret;
	}
	buffer = memalign(ARCH_DMA_MINALIGN, length);
	readback = memalign(ARCH_DMA_MINALIGN, length);
	if (!buffer || !readback) {
		free(readback);
		free(buffer);
		return ret;
	}
	printf("Disable %s U-Boot at 0x%x by invalidating only RKNS; "
	       "another matching boot image was found, but fallback is not "
	       "guaranteed.\n", spi ? "SPI" : "MMC",
	       FREEBSD_BOOT_HEADER_OFFSET);
	if (cli_readline_into_buffer("Type DISABLE BOOT to continue: ",
				     confirmation, 0) < 0 ||
	    strcmp(confirmation, "DISABLE BOOT")) {
		puts("Boot disable cancelled\n");
		goto out;
	}
	if (spi) {
		if (spi_flash_probe_bus_cs(CONFIG_SF_DEFAULT_BUS,
					   CONFIG_SF_DEFAULT_CS, &dev))
			goto out;
		flash = dev_get_uclass_priv(dev);
		if (!flash || flash->sector_size != SZ_4K ||
		    spi_flash_read(flash, FREEBSD_BOOT_HEADER_OFFSET, length,
				   buffer))
			goto out;
		memset(buffer, 0, 4);
		if (spi_flash_write(flash, FREEBSD_BOOT_HEADER_OFFSET, 4,
				    buffer) ||
		    spi_flash_read(flash, FREEBSD_BOOT_HEADER_OFFSET, length,
				   readback))
			goto out;
	} else {
		if (freebsd_mmc_firmware_target(devnum, &disk))
			goto out;
		if (blk_dread(disk, FREEBSD_BOOT_HEADER_OFFSET / 512, 1,
			      buffer) != 1)
			goto out;
		memset(buffer, 0, 4);
		if (blk_dwrite(disk, FREEBSD_BOOT_HEADER_OFFSET / 512, 1,
			       buffer) != 1 ||
		    blk_dread(disk, FREEBSD_BOOT_HEADER_OFFSET / 512, 1,
			      readback) != 1)
			goto out;
	}
	if (memcmp(buffer, readback, length))
		goto out;
	puts("Boot header disabled and read back; reboot manually\n");
	ret = CMD_RET_SUCCESS;
out:
	if (ret)
		puts("Boot disable failed; do not reset until recovery is ready\n");
	free(readback);
	free(buffer);
	return ret;
}

static int freebsd_enable_boot_target(bool spi, int devnum)
{
	struct udevice *dev;
	struct spi_flash *flash;
	struct blk_desc *disk;
	char confirmation[CONFIG_SYS_CBSIZE + 1] = { 0 };
	const char *storage = freebsd_firmware_storage();
	bool source_spi;
	u8 *buffer = NULL, *readback = NULL;
	size_t length = spi ? SZ_4K : 512;
	int ret = CMD_RET_FAILURE;

	if (!storage) {
		puts("Boot enable rejected: current boot medium is unknown\n");
		return ret;
	}
	source_spi = !strcmp(storage, "SPI");
	if ((spi == source_spi &&
	     (spi || devnum == mmc_get_env_dev())) ||
	    !freebsd_target_bootable(source_spi,
				      source_spi ? 0 : mmc_get_env_dev(), false) ||
	    !freebsd_target_bootable(spi, devnum, true)) {
		puts("Boot enable rejected: source or disabled target is not valid for this board\n");
		return ret;
	}
	buffer = memalign(ARCH_DMA_MINALIGN, length);
	readback = memalign(ARCH_DMA_MINALIGN, length);
	if (!buffer || !readback)
		goto out;
	if (spi) {
		if (spi_flash_probe_bus_cs(CONFIG_SF_DEFAULT_BUS,
					   CONFIG_SF_DEFAULT_CS, &dev))
			goto out;
		flash = dev_get_uclass_priv(dev);
		if (!flash || flash->sector_size != SZ_4K ||
		    spi_flash_read(flash, FREEBSD_BOOT_HEADER_OFFSET,
				   length, buffer))
			goto out;
	} else {
		if (freebsd_mmc_firmware_target(devnum, &disk) ||
		    blk_dread(disk, FREEBSD_BOOT_HEADER_OFFSET / 512, 1,
			      buffer) != 1)
			goto out;
	}
	memcpy(buffer, "RKNS", 4);
	printf("Restore only RKNS on %s using U-Boot's built-in signature; "
	       "remaining target firmware is unchanged.\n",
	       spi ? "SPI" : "MMC");
	if (cli_readline_into_buffer("Type ENABLE BOOT to continue: ",
				     confirmation, 0) < 0 ||
	    strcmp(confirmation, "ENABLE BOOT")) {
		puts("Boot enable cancelled\n");
		goto out;
	}
	if (spi) {
		if (spi_flash_erase(flash, FREEBSD_BOOT_HEADER_OFFSET,
				    length) ||
		    spi_flash_write(flash, FREEBSD_BOOT_HEADER_OFFSET,
				    length, buffer) ||
		    spi_flash_read(flash, FREEBSD_BOOT_HEADER_OFFSET,
				   length, readback))
			goto out;
	} else if (blk_dwrite(disk, FREEBSD_BOOT_HEADER_OFFSET / 512,
			      1, buffer) != 1 ||
		   blk_dread(disk, FREEBSD_BOOT_HEADER_OFFSET / 512,
			     1, readback) != 1) {
		goto out;
	}
	if (memcmp(buffer, readback, length))
		goto out;
	puts("Boot header enabled and read back; reboot manually\n");
	ret = CMD_RET_SUCCESS;
out:
	if (ret)
		puts("Boot enable failed; do not reset until recovery is ready\n");
	free(readback);
	free(buffer);
	return ret;
}

static int do_rkboot(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	char *end;
	int devnum = 0;
	bool spi;

	if (argc < 3 || (strcmp(argv[1], "disable") &&
			 strcmp(argv[1], "enable")))
		return CMD_RET_USAGE;
	spi = !strcmp(argv[2], "spi");
	if (!spi) {
		if (strcmp(argv[2], "mmc") || argc < 4)
			return CMD_RET_USAGE;
		devnum = simple_strtoul(argv[3], &end, 10);
		if (!argv[3][0] || *end || devnum < 0 || devnum > 3)
			return CMD_RET_USAGE;
	}
	if (!strcmp(argv[1], "disable")) {
		if (argc != (spi ? 3 : 4))
			return CMD_RET_USAGE;
		return freebsd_disable_boot_target(spi, devnum);
	}
	if (argc != (spi ? 3 : 4))
		return CMD_RET_USAGE;
	return freebsd_enable_boot_target(spi, devnum);
}

U_BOOT_CMD(
	rkboot, 4, 0, do_rkboot,
	"enable or disable matching SPI/eMMC/SD U-Boot firmware",
	"disable spi\n"
	"rkboot disable mmc <number>\n"
	"rkboot enable spi\n"
	"rkboot enable mmc <number>"
);
#endif

static void freebsd_clear_menu(void)
{
	char name[16];
	int i;

	for (i = 0; i < 99; i++) {
		snprintf(name, sizeof(name), "bootmenu_%d", i);
		env_set(name, NULL);
	}
}

static void freebsd_configure_watchdog(void)
{
	const char *enabled = env_get("freebsd_watchdog_enable");
	const char *timeout = env_get("freebsd_watchdog_timeout");
#if CONFIG_IS_ENABLED(WDT)
	struct udevice *dev;
	ulong seconds;
	int ret;
#endif

	if (!enabled) {
		env_set("freebsd_watchdog_enable", "0");
		enabled = "0";
	}
	if (!timeout) {
		env_set("freebsd_watchdog_timeout", "60");
		timeout = "60";
	}
	env_set("freebsd_watchdog_active", "0");

#if CONFIG_IS_ENABLED(WDT)
	if (uclass_get_device(UCLASS_WDT, 0, &dev)) {
		if (!strcmp(enabled, "1"))
			puts("U-Boot watchdog requested but no device was found\n");
		return;
	}
	if (!enabled || strcmp(enabled, "1")) {
		wdt_stop(dev);
		puts("U-Boot watchdog: disabled\n");
		return;
	}
	if (!timeout || !freebsd_valid_watchdog_timeout(timeout)) {
		puts("U-Boot watchdog: invalid timeout; watchdog remains disabled\n");
		wdt_stop(dev);
		return;
	}
	seconds = simple_strtoul(timeout, NULL, 10);
	ret = wdt_start(dev, seconds * 1000, 0);
	if (ret) {
		printf("U-Boot watchdog: start failed (%d)\n", ret);
		return;
	}
	env_set("freebsd_watchdog_active", "1");
#else
	if (!strcmp(enabled, "1"))
		puts("U-Boot watchdog requested but unavailable\n");
#endif
}

enum freebsd_entry_type {
	FREEBSD_ENTRY_EFI,
	FREEBSD_ENTRY_EXTLINUX,
};

struct freebsd_entry {
	char name[FREEBSD_MENU_NAME_SIZE + 1];
	char path[FREEBSD_ENTRY_BOOT_PATH_SIZE];
	enum freebsd_entry_type type;
	int part;
};

static bool freebsd_valid_entry_name(const char *name)
{
	const char *p;
	size_t len = strlen(name);

	if (!len || len > FREEBSD_MENU_NAME_SIZE)
		return false;
	for (p = name; *p; p++) {
		if (!isprint((unsigned char)*p) || *p == '=')
			return false;
	}
	return true;
}

static bool freebsd_valid_entry_path(const char *path)
{
	const char *p;

	if (*path != '/' || strlen(path) >= FREEBSD_ENTRY_BOOT_PATH_SIZE)
		return false;
	for (p = path; *p; p++) {
		if (!isalnum((unsigned char)*p) && !strchr("_./:+-", *p))
			return false;
	}
	return true;
}

static int freebsd_parse_entry(char *data, struct freebsd_entry *entry)
{
	bool have_format = false, have_name = false, have_type = false;
	bool have_path = false, have_part = false;
	char *cursor = data;
	char *line;

	memset(entry, 0, sizeof(*entry));
	while ((line = strsep(&cursor, "\n")) != NULL) {
		const char *end;
		char *value;
		size_t len = strlen(line);

		if (len && line[len - 1] == '\r')
			line[--len] = '\0';
		if (!len || line[0] == '#')
			continue;
		value = strchr(line, '=');
		if (!value || value == line || !value[1])
			return -EINVAL;
		*value++ = '\0';
		if (!strcmp(line, "format")) {
			if (have_format || strcmp(value, "1"))
				return -EINVAL;
			have_format = true;
		} else if (!strcmp(line, "name")) {
			if (have_name || !freebsd_valid_entry_name(value))
				return -EINVAL;
			strlcpy(entry->name, value, sizeof(entry->name));
			have_name = true;
		} else if (!strcmp(line, "type")) {
			if (have_type)
				return -EINVAL;
			if (!strcmp(value, "efi"))
				entry->type = FREEBSD_ENTRY_EFI;
			else if (!strcmp(value, "extlinux"))
				entry->type = FREEBSD_ENTRY_EXTLINUX;
			else
				return -EINVAL;
			have_type = true;
		} else if (!strcmp(line, "path")) {
			if (have_path || !freebsd_valid_entry_path(value))
				return -EINVAL;
			strlcpy(entry->path, value, sizeof(entry->path));
			have_path = true;
		} else if (!strcmp(line, "partition")) {
			if (have_part)
				return -EINVAL;
			if (!strcmp(value, "self")) {
				entry->part = 0;
			} else {
				end = freebsd_parse_number(value, 128, false);
				if (!end || *end)
					return -EINVAL;
				entry->part = dectoul(value, NULL);
			}
			have_part = true;
		} else {
			return -EINVAL;
		}
	}

	if (!have_format || !have_name || !have_type || !have_path ||
	    (entry->type == FREEBSD_ENTRY_EFI && entry->part))
		return -EINVAL;
	return 0;
}

static int freebsd_read_entry(struct blk_desc *desc, int part,
			      struct freebsd_entry *entry)
{
	char data[FREEBSD_ENTRY_SIZE + 1];
	loff_t actread;
	loff_t size;

	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_size(FREEBSD_ENTRY_PATH, &size) || size <= 0 ||
	    size > FREEBSD_ENTRY_SIZE)
		return -EINVAL;
	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_read(FREEBSD_ENTRY_PATH, map_to_sysmem(data), 0, size,
		    &actread) || actread != size)
		return -EIO;
	data[size] = '\0';
	return freebsd_parse_entry(data, entry);
}

static int freebsd_entry_selftest(void)
{
	static const char valid[] =
		"format=1\nname=Armbian\ntype=extlinux\n"
		"path=/boot/extlinux/extlinux.conf\npartition=2\n";
	static const char duplicate[] =
		"format=1\nname=FreeBSD\nname=Again\ntype=efi\n"
		"path=/EFI/FreeBSD/loader.efi\n";
	static const char unsafe[] =
		"format=1\nname=Bad\ntype=efi\npath=/loader.efi;reset\n";
	struct freebsd_entry entry;
	char data[FREEBSD_ENTRY_SIZE + 1];

	strcpy(data, valid);
	if (freebsd_parse_entry(data, &entry) ||
	    strcmp(entry.name, "Armbian") ||
	    entry.type != FREEBSD_ENTRY_EXTLINUX || entry.part != 2)
		return CMD_RET_FAILURE;
	strlcpy(data, duplicate, sizeof(data));
	if (!freebsd_parse_entry(data, &entry))
		return CMD_RET_FAILURE;
	strlcpy(data, unsafe, sizeof(data));
	if (!freebsd_parse_entry(data, &entry))
		return CMD_RET_FAILURE;
	puts("RK3588 boot-entry self-test passed\n");
	return CMD_RET_SUCCESS;
}

static int freebsd_add_entry(const char *label, const char *ifname,
			     const char *menu_name, int devnum, int part,
			     bool config_entry,
			     const char *wanted,
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
	if (config_entry) {
		snprintf(command, sizeof(command), "freebsdboot boot %s %s",
			 ifname, devpart);
	} else {
		snprintf(command, sizeof(command),
			 "setenv boot_entry_mode legacy-efi; "
			 "setenv freebsd_iface %s; setenv freebsd_devpart %s; "
			 "echo RK3588-BOOT-TARGET %s%s MODE legacy-efi; "
			 "run boot_freebsd_target",
			 ifname, devpart, ifname, devpart);
	}
	snprintf(value, sizeof(value), "%-32.32s - %s (%s)=%s",
		 menu_name, label, target, command);
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

static void freebsd_read_menu_name(struct blk_desc *desc, int part,
				   char *name)
{
	loff_t actread;
	loff_t size;
	int i;

	strcpy(name, "Unknown");
	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_size(FREEBSD_MENU_NAME_PATH, &size) || size <= 0)
		return;
	size = min_t(loff_t, size, FREEBSD_MENU_NAME_SIZE);
	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_read(FREEBSD_MENU_NAME_PATH, map_to_sysmem(name), 0, size,
		    &actread) || actread != size)
		goto invalid;

	name[size] = '\0';
	for (i = 0; i < size && name[i] != '\n' && name[i] != '\r'; i++) {
		if (!isprint((unsigned char)name[i]) || name[i] == '=')
			goto invalid;
	}
	while (i > 0 && name[i - 1] == ' ')
		i--;
	if (!i)
		goto invalid;
	name[i] = '\0';
	return;

invalid:
	strcpy(name, "Unknown");
}

static void freebsd_scan_desc(struct blk_desc *desc, const char *label,
			      const char *wanted, char *targets,
			      size_t targets_size, int *index,
			      int *default_index)
{
	struct disk_partition info;
	const char *ifname = blk_get_uclass_name(desc->uclass_id);
	char menu_name[FREEBSD_MENU_NAME_SIZE + 1];
	char legacy_name[FREEBSD_MENU_NAME_SIZE + 1];
	struct freebsd_entry entry;
	int part;

	for (part = 1; part <= MAX_SEARCH_PARTITIONS; part++) {
		if (part_get_info(desc, part, &info))
			continue;
		if (!(info.bootable & PART_EFI_SYSTEM_PARTITION))
			continue;
		if (fs_set_blk_dev_with_part(desc, part))
			continue;
		if (fs_exists(FREEBSD_ENTRY_PATH)) {
			int boot_part;

			if (!freebsd_read_entry(desc, part, &entry)) {
				boot_part = entry.part ? entry.part : part;
				if (fs_set_blk_dev_with_part(desc, boot_part) ||
				    !fs_exists(entry.path)) {
					printf("Unavailable entry path %s on %s%d:%d; "
					       "checking legacy EFI\n", entry.path,
					       ifname, desc->devnum, boot_part);
					goto legacy;
				}
				if (freebsd_add_entry(label, ifname, entry.name,
						      desc->devnum, part, true,
						      wanted, targets, targets_size,
						      index, default_index))
					return;
				continue;
			}
			printf("Invalid %s on %s%d:%d; checking legacy EFI\n",
			       FREEBSD_ENTRY_PATH, ifname, desc->devnum, part);
		}
legacy:
		if (fs_set_blk_dev_with_part(desc, part) ||
		    !fs_exists(FREEBSD_LOADER_PATH))
			continue;
		freebsd_read_menu_name(desc, part, menu_name);
		snprintf(legacy_name, sizeof(legacy_name), "%.19s [legacy EFI]",
			 menu_name);
		if (freebsd_add_entry(label, ifname, legacy_name, desc->devnum,
				      part,
				      false,
				      wanted, targets, targets_size, index,
				      default_index))
			return;
	}
}

static int freebsd_boot_entry(const char *ifname, const char *devpart)
{
	struct disk_partition info;
	struct freebsd_entry entry;
	struct blk_desc *desc;
	char bootpart[24];
	int part;

	part = blk_get_device_part_str(ifname, devpart, &desc, &info, 0);
	if (part <= 0 || freebsd_read_entry(desc, part, &entry)) {
		printf("Boot entry on %s %s is no longer valid\n", ifname,
		       devpart);
		return CMD_RET_FAILURE;
	}
	if (!entry.part)
		entry.part = part;
	snprintf(bootpart, sizeof(bootpart), "%d:%d", desc->devnum,
		 entry.part);
	if (fs_set_blk_dev_with_part(desc, entry.part) ||
	    !fs_exists(entry.path)) {
		printf("Boot entry path is unavailable: %s %s %s\n", ifname,
		       bootpart, entry.path);
		return CMD_RET_FAILURE;
	}

	if (env_set("boot_entry_iface", ifname) ||
	    env_set("boot_entry_devpart", bootpart) ||
	    env_set("boot_entry_path", entry.path) ||
	    env_set("boot_entry_type",
		    entry.type == FREEBSD_ENTRY_EFI ? "efi" : "extlinux") ||
	    env_set("boot_entry_mode", "config"))
		return CMD_RET_FAILURE;
	printf("RK3588-BOOT-TARGET %s%s MODE config TYPE %s\n", ifname,
	       devpart, env_get("boot_entry_type"));
	return run_command("run boot_entry_target", 0) ?
		CMD_RET_FAILURE : CMD_RET_SUCCESS;
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

static bool freebsd_mmc_is_sd(struct blk_desc *desc)
{
	struct mmc *mmc = find_mmc_device(desc->devnum);

	return mmc ? IS_SD(mmc) : !!desc->removable;
}

static void freebsd_scan_mmc(bool sd, const char *wanted, char *targets,
			     size_t targets_size, int *index,
			     int *default_index)
{
	struct blk_desc *desc;
	int devnum;
	int max;

	max = blk_find_max_devnum(UCLASS_MMC);
	for (devnum = 0; devnum <= max; devnum++) {
		desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, devnum);
		if (!desc || freebsd_mmc_is_sd(desc) != sd)
			continue;
		freebsd_scan_desc(desc, sd ? "SD" : "eMMC", wanted,
				   targets, targets_size, index, default_index);
	}
}

static bool freebsd_request_mmc(bool sd)
{
	struct blk_desc *desc;
	int devnum;
	int max;

	max = blk_find_max_devnum(UCLASS_MMC);
	for (devnum = 0; devnum <= max; devnum++) {
		desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, devnum);
		if (!desc || freebsd_mmc_is_sd(desc) != sd)
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
		"boot_entry_target",
		"freebsd_loader",
	};
	const char *wanted;
	char targets[FREEBSD_TARGETS_SIZE] = {};
	char value[16];
#if IS_ENABLED(CONFIG_RK3588_FREEBSD_SPI_UPDATE)
	char firmware_size[24];
#endif
	int default_index = -1;
	int index = 0;
	bool usb_ready;
	bool nvme_ready;
	bool scsi_ready;
	bool request_applied;

	/* Do not let a saved environment pin an old firmware implementation. */
	env_set_default_vars(ARRAY_SIZE(runtime_defaults),
			     (char * const *)runtime_defaults, 0);
#if IS_ENABLED(CONFIG_RK3588_FREEBSD_SPI_UPDATE)
	env_set("freebsd_firmware_compat",
		CONFIG_RK3588_FREEBSD_SPI_COMPAT);
	snprintf(firmware_size, sizeof(firmware_size), "%u", CONFIG_ENV_OFFSET);
	env_set("freebsd_firmware_size", firmware_size);
	env_set("freebsd_firmware_storage", freebsd_firmware_storage());
#else
	env_set("freebsd_firmware_compat", NULL);
	env_set("freebsd_firmware_size", NULL);
	env_set("freebsd_firmware_storage", NULL);
#endif
	freebsd_configure_watchdog();
	freebsd_clear_menu();
	mmc_initialize(NULL);
	usb_ready = IS_ENABLED(CONFIG_USB_STORAGE) && !usb_init() &&
		usb_stor_scan(1) >= 0;
	if (IS_ENABLED(CONFIG_PCI))
		pci_init();
	nvme_ready = IS_ENABLED(CONFIG_NVME) && !nvme_scan_namespace();
	scsi_ready = IS_ENABLED(CONFIG_SCSI) && !scsi_scan(false);

#if IS_ENABLED(CONFIG_RK3588_FREEBSD_SPI_UPDATE)
	if (freebsd_apply_spi_update(usb_ready, nvme_ready, scsi_ready))
		return -EIO;
#endif

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
		freebsd_configure_watchdog();
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

	snprintf(value, sizeof(value), "bootmenu_%d", index);
	if (env_set(value, "U-Boot CLI=exit"))
		return -ENOMEM;

	if (default_index < 0) {
		if (wanted && strcmp(wanted, "auto")) {
			printf("Configured FreeBSD target %s is unavailable; "
			       "stopping at U-Boot CLI\n", wanted);
			default_index = index;
		} else {
			default_index = 0;
		}
	}
	snprintf(value, sizeof(value), "%d", default_index);
	if (env_set("bootmenu_default", value))
		return -ENOMEM;

	printf("Detected %d RK3588 boot target%s\n",
	       index, index == 1 ? "" : "s");
	return 0;
}

static int do_freebsdboot(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	if (argc == 4 && !strcmp(argv[1], "boot"))
		return freebsd_boot_entry(argv[2], argv[3]);
	if (argc == 2 && !strcmp(argv[1], "selftest"))
		return freebsd_entry_selftest();
	if (argc != 1)
		return CMD_RET_USAGE;
	return freebsd_build_menu() ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	freebsdboot, 4, 0, do_freebsdboot,
	"build or execute an RK3588 boot menu entry",
	"\n"
	"freebsdboot boot <interface> <dev:partition>\n"
	"freebsdboot selftest"
);
