/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __ENV_ROCKCHIP_RK3588_FREEBSD_H
#define __ENV_ROCKCHIP_RK3588_FREEBSD_H

#ifdef CONFIG_RK3588_FREEBSD_SHOW_LOGO
#define RK3588_FREEBSD_LOGO_SETTING	"logo_enable=1\0"
#else
#define RK3588_FREEBSD_LOGO_SETTING	"logo_enable=0\0"
#endif

#ifdef CONFIG_RK3588_FREEBSD_BOOT
#define RK3588_FREEBSD_ENV_SETTINGS \
	"bootdelay=0\0" \
	"bootmenu_title=*** FreeBSD U-Boot Boot Menu ***\0" \
	"bootmenu_delay=3\0" \
	"bootmenu_config=/bootmenu.env\0" \
	"freebsd_default_boot=auto\0" \
	"freebsd_loader=/EFI/FreeBSD/loader.efi\0" \
	"freebsd_dtb=/dtb/freebsd.dtb\0" \
	RK3588_FREEBSD_LOGO_SETTING \
	"load_bootmenu_mmc=" \
		"if mmc dev ${bootmenu_mmcdev}; then " \
			"if load mmc ${bootmenu_mmcdev}:1 ${scriptaddr} " \
					"${bootmenu_config}; then " \
				"if env import -t -r ${scriptaddr} ${filesize} " \
						"bootmenu_title bootmenu_delay; then " \
					"echo \"Loaded ${bootmenu_config} from mmc " \
						"${bootmenu_mmcdev}:1\"; " \
					"setenv bootmenu_loaded 1; " \
				"else " \
					"echo \"Invalid ${bootmenu_config} on mmc " \
						"${bootmenu_mmcdev}:1\"; " \
				"fi; " \
			"fi; " \
		"fi\0" \
	"set_boot_mmc_order=" \
		"if test \"${rk_boot_storage}\" = \"1\"; then " \
			"setenv boot_primary_mmcdev 0; " \
			"setenv boot_fallback_mmcdev 1; " \
		"else " \
			"setenv boot_primary_mmcdev 1; " \
			"setenv boot_fallback_mmcdev 0; " \
		"fi\0" \
	"load_bootmenu=" \
		"run set_boot_mmc_order; " \
		"setenv bootmenu_loaded 0; " \
		"setenv bootmenu_mmcdev ${boot_primary_mmcdev}; " \
		"run load_bootmenu_mmc; " \
		"if test \"${bootmenu_loaded}\" != \"1\"; then " \
			"setenv bootmenu_mmcdev ${boot_fallback_mmcdev}; " \
			"run load_bootmenu_mmc; " \
		"fi; " \
		"if test \"${bootmenu_loaded}\" != \"1\"; then " \
			"echo \"Using built-in FreeBSD menu settings\"; " \
		"fi; " \
		"setenv bootmenu_loaded; setenv bootmenu_mmcdev\0" \
	"load_logo_mmc=" \
		"if mmc dev ${logo_mmcdev}; then " \
			"if mmc read ${loadaddr} 0x6000 0x2000; then " \
				"setenv logo_loaded 1; " \
			"fi; " \
		"fi\0" \
	"show_logo=" \
		"setenv logo_loaded 0; " \
		"if test \"${rk_boot_storage}\" = \"9\"; then " \
			"if sf probe && sf read ${loadaddr} 0xc00000 0x400000; then " \
				"setenv logo_loaded 1; " \
			"fi; " \
		"elif test \"${rk_boot_storage}\" = \"1\"; then " \
			"setenv logo_mmcdev 0; run load_logo_mmc; " \
		"else " \
			"setenv logo_mmcdev 1; run load_logo_mmc; " \
			"if test \"${logo_loaded}\" != \"1\"; then " \
				"setenv logo_mmcdev 0; run load_logo_mmc; " \
			"fi; " \
		"fi; " \
		"if test \"${logo_loaded}\" = \"1\"; then " \
			"if bmp display ${loadaddr} m m; then sleep 3; fi; " \
		"fi; " \
		"setenv logo_loaded; setenv logo_mmcdev\0" \
	"boot_freebsd_target=" \
		"echo \"Trying FreeBSD from ${freebsd_iface} " \
			"${freebsd_devpart}\"; " \
		"if load ${freebsd_iface} ${freebsd_devpart} ${fdt_addr_r} " \
				"${freebsd_dtb}; then " \
			"fdt addr ${fdt_addr_r}; " \
			"if load ${freebsd_iface} ${freebsd_devpart} " \
					"${kernel_addr_r} ${freebsd_loader}; then " \
				"bootefi ${kernel_addr_r} ${fdt_addr_r}; " \
			"fi; " \
		"fi; echo \"FreeBSD boot failed\"\0" \
	"fallback_menu=" \
		"run load_bootmenu; " \
		"freebsdboot; " \
		"echo \"RK3588 FreeBSD U-Boot 2026.07\"; " \
		"echo \"Default target: ${freebsd_default_boot}\"; " \
		"bootmenu ${bootmenu_delay}\0" \
	"bootcmd=" \
		"if test \"${logo_enable}\" = \"1\"; then run show_logo; fi; " \
		"setenv stdout serial,vidconsole; " \
		"setenv stderr serial,vidconsole; " \
		"run fallback_menu\0"
#else
#define RK3588_FREEBSD_ENV_SETTINGS
#endif

#endif
