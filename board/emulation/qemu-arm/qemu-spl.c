// SPDX-License-Identifier: GPL-2.0+
/*
 * SPL for the QEMU aarch64 'virt' machine.
 *
 * The SPL stands in for the early stage a split-layout board keeps in SPI
 * NOR.  Like Rockchip's SPI-resident SPL it reads its next-stage payload
 * from the eMMC boot partition (MMCSD_MODE_EMMCBOOT, byte 0) rather than
 * from the medium it was itself loaded from, exercising the same generic
 * U-Boot loader path the carried rockchip patch selects.  The SPL is
 * booted by QEMU with -bios (it runs from the machine's low memory alias),
 * and reads its payload from an emulated eMMC attached behind a PCI SDHCI.
 *
 * Only built when CONFIG_SPL_BUILD; the U-Boot proper board code lives in
 * qemu-arm.c.
 */
#include <dm.h>
#include <fdtdec.h>
#include <hang.h>
#include <init.h>
#include <log.h>
#include <mapmem.h>
#include <mmc.h>
#include <spl.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

u32 spl_boot_device(void)
{
	return BOOT_DEVICE_MMC1;
}

u32 spl_mmc_boot_mode(struct mmc *mmc, const u32 boot_device)
{
	/*
	 * As on Rockchip's SPI-resident SPL, the next stage lives on the
	 * eMMC boot partition, not on the medium we were loaded from.
	 */
	return MMCSD_MODE_EMMCBOOT;
}

unsigned long arch_spl_mmc_get_uboot_raw_sector(struct mmc *mmc,
						unsigned long raw_sect)
{
	/* The payload FIT sits at byte 0 of the eMMC boot partition. */
	return 0;
}

void reset_cpu(void)
{
	/* Nothing to reset to; just stop. */
	while (1) { }
}

struct legacy_img_hdr *spl_get_load_buffer(ssize_t offset, size_t size)
{
	/*
	 * The weak default scratches at CONFIG_TEXT_BASE (0x0 here, the
	 * low-memory alias the SPL itself runs from); point it at spare
	 * DRAM instead.
	 */
	return map_sysmem(0x4E000000 + offset, 0);
}

int dram_init(void)
{
	if (fdtdec_setup_mem_size_base() != 0)
		return -EINVAL;

	return 0;
}

int board_fdt_blob_setup(void **fdtp)
{
	/* QEMU loads a generated DTB for us at the start of RAM. */
	*fdtp = (void *)CFG_SYS_SDRAM_BASE;

	return 0;
}

void board_init_f(ulong dummy)
{
	int ret;

	ret = spl_early_init();
	if (ret) {
		printf("spl_early_init() failed: %d\n", ret);
		hang();
	}

	if (dram_init()) {
		printf("dram_init() failed\n");
		hang();
	}
	gd->ram_top = gd->ram_base + gd->ram_size;

	preloader_console_init();

	/* The SPL's own board_init_r() enumerates PCI before booting. */
}
