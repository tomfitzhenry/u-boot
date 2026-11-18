/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __CONFIG_OZ64_H
#define __CONFIG_OZ64_H

/* The SG2000 FSBL hands off at 1,500,000 baud; keep the console there. */
#define CFG_SYS_BAUDRATE_TABLE { 9600, 19200, 38400, 57600, 115200, 1500000 }

/* DRAM starts at 0x80000000; gd->ram_base must not default to 0. */
#define CFG_SYS_SDRAM_BASE 0x80000000

#endif
