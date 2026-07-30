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
	"bootmenu_0=FreeBSD=run boot_freebsd\0" \
	"bootmenu_1=U-Boot CLI=exit\0" \
	"bootmenu_config=/bootmenu.env\0" \
	RK3588_FREEBSD_LOGO_SETTING \
	"reset_bootmenu=" \
		"setenv bootmenu_title \"*** FreeBSD U-Boot Boot Menu ***\"; " \
		"setenv bootmenu_delay 3; " \
		"setenv bootmenu_0 \"FreeBSD=run boot_freebsd\"; " \
		"setenv bootmenu_1 \"U-Boot CLI=exit\"; " \
		"setenv bootmenu_2; setenv bootmenu_3; setenv bootmenu_4; " \
		"setenv bootmenu_5; setenv bootmenu_6; setenv bootmenu_7; " \
		"setenv bootmenu_8; setenv bootmenu_9\0" \
	"load_bootmenu_mmc=" \
		"if mmc dev ${bootmenu_mmcdev}; then " \
			"if load mmc ${bootmenu_mmcdev}:1 ${scriptaddr} " \
					"${bootmenu_config}; then " \
				"if env import -t -r ${scriptaddr} ${filesize} " \
						"bootmenu_title bootmenu_delay " \
						"bootmenu_0 bootmenu_1 bootmenu_2 " \
						"bootmenu_3 bootmenu_4 bootmenu_5 " \
						"bootmenu_6 bootmenu_7 bootmenu_8 " \
						"bootmenu_9; then " \
					"echo \"Loaded ${bootmenu_config} from mmc " \
						"${bootmenu_mmcdev}:1\"; " \
					"setenv bootmenu_loaded 1; " \
				"else " \
					"echo \"Invalid ${bootmenu_config} on mmc " \
						"${bootmenu_mmcdev}:1\"; " \
					"run reset_bootmenu; " \
				"fi; " \
			"fi; " \
		"fi\0" \
	"load_bootmenu=" \
		"setenv bootmenu_loaded 0; run reset_bootmenu; " \
		"setenv bootmenu_mmcdev 1; run load_bootmenu_mmc; " \
		"if test \"${bootmenu_loaded}\" != \"1\"; then " \
			"setenv bootmenu_mmcdev 0; run load_bootmenu_mmc; " \
		"fi; " \
		"if test \"${bootmenu_loaded}\" != \"1\"; then " \
			"echo \"Using built-in FreeBSD boot menu\"; " \
			"run reset_bootmenu; " \
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
	"boot_freebsd_from_mmc=" \
		"if mmc dev ${boot_mmcdev}; then " \
			"echo \"Trying FreeBSD from mmc ${boot_mmcdev}:1\"; " \
			"if load mmc ${boot_mmcdev}:1 ${fdt_addr_r} /dtb/freebsd.dtb; then " \
				"fdt addr ${fdt_addr_r}; " \
				"if load mmc ${boot_mmcdev}:1 ${kernel_addr_r} " \
						"/EFI/FreeBSD/loader.efi; then " \
					"bootefi ${kernel_addr_r} ${fdt_addr_r}; " \
				"elif load mmc ${boot_mmcdev}:1 ${kernel_addr_r} " \
						"/EFI/BOOT/BOOTAA64.EFI; then " \
					"bootefi ${kernel_addr_r} ${fdt_addr_r}; " \
				"fi; " \
			"fi; " \
		"fi\0" \
	"boot_freebsd=" \
		"setenv boot_mmcdev 1; run boot_freebsd_from_mmc; " \
		"setenv boot_mmcdev 0; run boot_freebsd_from_mmc; " \
		"setenv boot_mmcdev; echo \"FreeBSD boot failed\"\0" \
	"fallback_menu=" \
		"run load_bootmenu; " \
		"echo \"RK3588 FreeBSD U-Boot 2026.07\"; " \
		"echo \"Default after timeout: FreeBSD\"; " \
		"bootmenu ${bootmenu_delay}; run boot_freebsd; bootflow scan -lb\0" \
	"bootcmd=" \
		"if test \"${logo_enable}\" = \"1\"; then run show_logo; fi; " \
		"setenv stdout serial,vidconsole; " \
		"setenv stderr serial,vidconsole; " \
		"run fallback_menu\0"
#else
#define RK3588_FREEBSD_ENV_SETTINGS
#endif

#endif
