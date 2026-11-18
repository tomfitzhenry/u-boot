/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2024 Tom Fitzhenry <tom@tom-fitzhenry.me.uk>
 */

#ifndef _CONFIG_NETGEAR_RN102_H
#define _CONFIG_NETGEAR_RN102_H

#define CFG_I2C_MVTWSI_BASE0		MVEBU_TWSI_BASE

/*
 * Keep device tree and initrd in lower memory so the kernel can access them
 */
#define RELOCATION_LIMITS_ENV_SETTINGS	\
	"fdt_high=0x10000000\0"		\
	"initrd_high=0x10000000\0"

/*
 * mv-common.h should be defined after CMD configs since it uses them
 * to enable certain macros
 */
#include "mv-common.h"

#ifndef CONFIG_XPL_BUILD

#define KERNEL_ADDR_R	__stringify(0x2000000)
#define FDT_ADDR_R	__stringify(0x1000000)
#define RAMDISK_ADDR_R	__stringify(0x3000000)
#define SCRIPT_ADDR_R	__stringify(0x1800000)
#define PXEFILE_ADDR_R	__stringify(0x1900000)

#define LOAD_ADDRESS_ENV_SETTINGS \
	"kernel_addr_r=" KERNEL_ADDR_R "\0" \
	"fdt_addr_r=" FDT_ADDR_R "\0" \
	"ramdisk_addr_r=" RAMDISK_ADDR_R "\0" \
	"scriptaddr=" SCRIPT_ADDR_R "\0" \
	"pxefile_addr_r=" PXEFILE_ADDR_R "\0"

#define CFG_EXTRA_ENV_SETTINGS \
	RELOCATION_LIMITS_ENV_SETTINGS \
	LOAD_ADDRESS_ENV_SETTINGS \
	"fdtfile=" CONFIG_DEFAULT_DEVICE_TREE ".dtb\0" \
	"console=ttyS0,115200\0"

#endif /* CONFIG_XPL_BUILD */

#endif /* _CONFIG_NETGEAR_RN102_H */
