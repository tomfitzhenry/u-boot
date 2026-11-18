// SPDX-License-Identifier: GPL-2.0+
/*
 * Pine64 Oz64 (Sophgo SG2000), ARM64 boot via the vendor FSBL/BL31 FIP.
 */

#include <config.h>
#include <init.h>
#include <linux/kernel.h>
#include <asm/global_data.h>
#include <asm/io.h>

#include "ethernet.h"

DECLARE_GLOBAL_DATA_PTR;

#ifdef CONFIG_ARM64
#include <asm/armv8/mmu.h>

static struct mm_region oz64_mem_map[] = {
	{
		/* MMIO and the C906's view of the first 2 GiB. */
		.virt = 0x00000000ULL,
		.phys = 0x00000000ULL,
		.size = 0x80000000ULL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* 512 MiB of DRAM. */
		.virt = 0x80000000ULL,
		.phys = 0x80000000ULL,
		.size = 0x20000000ULL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = oz64_mem_map;
#endif /* CONFIG_ARM64 */

int print_cpuinfo(void)
{
	printf("CPU:   Sophgo SG2000 (ARM Cortex-A53)\n");
	return 0;
}

void reset_cpu(void)
{
	writel(1, 0x0300bef0);
}

int dram_init(void)
{
	gd->ram_size = 0x1fe00000;
	return 0;
}

int dram_init_banksize(void)
{
	gd->dram[0].start = 0x80000000;
	gd->dram[0].size = 0x1fe00000;
	return 0;
}

/*
 * U-Boot has no SG2000 pinctrl driver, so the pinctrl-0 properties in the
 * board DTS are inert.  The FSBL leaves the eMMC pins muxed to function 1
 * with pulldowns, which makes CMD1 time out with -110.  Apply the same
 * settings as Linux's emmc_cfg (function 0, pull-up, 10.8 mA at 3.3 V)
 * before the MMC controllers are probed.
 */
#define SG2000_PINCTRL_BASE	0x03001000

static void cv1800b_emmc_pinctrl_init(void)
{
	static const u16 mux_offsets[] = {
		0x048, 0x04c, 0x050, 0x054, 0x058, 0x05c, 0x060,
	};
	static const u16 conf_offsets[] = {
		0x914, 0x918, 0x91c, 0x920, 0x924, 0x928, 0x92c,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(mux_offsets); i++)
		writel(0, SG2000_PINCTRL_BASE + mux_offsets[i]);

	for (i = 0; i < ARRAY_SIZE(conf_offsets); i++)
		writel(0x44, SG2000_PINCTRL_BASE + conf_offsets[i]);
}

#define SG2000_PWM0_BUCK_MUX	0x0ec
#define SG2000_USB_VBUS_EN_MUX	0x100

/*
 * The USB-A port's VBUS switch is the SoC's USB_VBUS_EN pin (C12, function
 * 0), as in the Linux Oz64 DT.  U-Boot has no SG2000 pinctrl driver, so
 * mux it here before the DWC2 controller probes.
 *
 * The FSBL leaves PWM0_BUCK (0x0ec) muxed to GPIO (function 3).  Restore
 * function 0 (the buck PWM) before the eMMC probes; otherwise CMD1 times
 * out with "Card did not respond to voltage select" and U-Boot cannot see
 * the eMMC at all.  This is not GPIOB5: the 0x0ec pad is the buck control.
 */
static void cv1800b_usb_init(void)
{
	writel(0, SG2000_PINCTRL_BASE + SG2000_PWM0_BUCK_MUX);
	writel(0, SG2000_PINCTRL_BASE + SG2000_USB_VBUS_EN_MUX);
}

int board_init(void)
{
	if (IS_ENABLED(CONFIG_MMC_SDHCI_CV1800B))
		cv1800b_emmc_pinctrl_init();

	if (IS_ENABLED(CONFIG_USB_DWC2))
		cv1800b_usb_init();

	/*
	 * U-Boot has no internal-ePHY driver, so take it out of reset before
	 * the DesignWare MAC probes and scans the MDIO bus.
	 */
	if (IS_ENABLED(CONFIG_ETH_DESIGNWARE))
		cv1800b_ephy_init();

	return 0;
}

int board_late_init(void)
{
	return 0;
}
