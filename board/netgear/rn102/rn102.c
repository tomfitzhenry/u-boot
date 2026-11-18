// SPDX-License-Identifier: GPL-2.0+
/*
 * U-Boot port for Netgear ReadyNAS RN102
 * Based on the Synology DS414 port by Phil Sutter <phil@nwl.cc>
 */

#include <env.h>
#include <init.h>
#include <miiphy.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/arch/cpu.h>
#include <asm/arch/soc.h>
#include <linux/bitops.h>
#include <linux/mbus.h>

#include "../drivers/ddr/marvell/axp/ddr3_hw_training.h"
#include "../arch/arm/mach-mvebu/serdes/axp/board_env_spec.h"

DECLARE_GLOBAL_DATA_PTR;

/*
 * GPP and MPP settings from the stock RN102 U-Boot source
 * (board/mv_ebu/a370/armada_370_family/boardEnv/mvBoardEnvSpec.h)
 */

#define RN102_GPP_OUT_VAL_LOW		(BIT(8)  | BIT(11) | BIT(13) | \
					 BIT(14) | BIT(15) | BIT(16) | BIT(31))
#define RN102_GPP_OUT_VAL_MID		(BIT(16) | BIT(17) | BIT(18) | \
					 BIT(19) | BIT(20) | BIT(21) | \
					 BIT(23) | BIT(24) | BIT(25) | BIT(28))
#define RN102_GPP_OUT_VAL_HIGH		(0)

#define RN102_GPP_OUT_POL_LOW		(BIT(6))
#define RN102_GPP_OUT_POL_MID		(BIT(26))
#define RN102_GPP_OUT_POL_HIGH		(0)

#define RN102_GPP_OUT_ENA_LOW		(~(BIT(8)  | BIT(11) | BIT(13) | \
					   BIT(14) | BIT(15) | BIT(16) | BIT(31)))
#define RN102_GPP_OUT_ENA_MID		(~(BIT(16) | BIT(17) | BIT(18) | \
					   BIT(19) | BIT(20) | BIT(21) | \
					   BIT(23) | BIT(24) | BIT(25) | BIT(28)))
#define RN102_GPP_OUT_ENA_HIGH		(~0)

static const u32 rn102_mpp_control[] = {
	0x00011111,
	0x00000000,
	0x22222110,
	0x02222222,
	0x11111110,
	0x01111111,
	0x00000000,
	0x10000000,
	0x00000011,
};

/*
 * DDR3 static MC configuration for RN102
 *
 * CPU 1200MHz, DDR 600MHz, 16-bit width, 512MB (single CS)
 * Derived from ddr3_A0_db_600 in the stock U-Boot source
 * (tools/marvell/bin_hdr/inc/ddr3_a370_mc_static.h)
 */
MV_DRAM_MC_INIT rn102_ddr3_600[MV_MAX_DDR3_STATIC_SIZE] = {
	{0x00001400, 0x73014924},	/* DDR SDRAM Configuration (16-bit) */
	{0x00001404, 0x3000B800},	/* Dunit Control Low */
	{0x00001408, 0x45148886},	/* DDR SDRAM Timing Low */
	{0x0000140C, 0x3AD93FB3},	/* DDR SDRAM Timing High */
	{0x00001410, 0x1E1F0011},	/* DDR SDRAM Open Pages Control */
	{0x00001424, 0x0100F37F},	/* Dunit Control High */
	{0x00001428, 0x0011AA50},	/* Dunit Control High (ODT timing low) */
	{0x0000142C, 0x014C4EE4},	/* Dunit Control High (ODT timing high) */
	{0x0000147C, 0x0000C671},	/* DDR ODT Time High */
	{0x000014a8, 0x00000100},	/* AXI Control */
	{0x00001494, 0x00010000},	/* DDR SDRAM ODT Control Low */
	{0x00001498, 0x00000000},	/* DDR SDRAM ODT Control High */
	{0x0000149C, 0x0000000F},	/* DDR Dunit ODT Control */
	{0x000014C0, 0x192424C9},	/* DRAM Address/Control Driving Strength */
	{0x000014C4, 0x0AAA2549},	/* DRAM Data/DQS Driving Strength */
	{0x00020184, 0x1FFFFFE0},	/* Close fast path window to 2G */
	{0x0002018C, 0x00000000},
	{0x00020194, 0x00000000},
	{0x0002019C, 0x00000000},
	{0x00001504, 0x1FFFFFF1},	/* CS0 Size (512MB) */
	{0x0000150C, 0x00000000},	/* CS1 Size (disabled) */
	{0x00001514, 0x00000000},	/* CS2 Size (disabled) */
	{0x0000151C, 0x00000000},	/* CS3 Size (disabled) */
	{0x00001538, 0x0000000b},	/* Read Data Sample Delays */
	{0x0000153C, 0x0000000b},	/* Read Data Ready Delay */
	{0x000015D0, 0x00000670},	/* MR0 (CL=6, WR=8) */
	{0x000015D4, 0x00000006},	/* MR1 (RTT=RZQ/4) */
	{0x000015D8, 0x00000010},	/* MR2 (CWL=6) */
	{0x000015DC, 0x00000000},	/* MR3 */
	{0x000015E0, 0x00000001},	/* DDR3 Rank Ctrl */
	{0x000015E4, 0x00203C18},	/* ZQC Configuration */
	{0x000015EC, 0xDE000025},	/* DDR PHY Configuration */
	{0x0, 0x0}			/* Terminator */
};

/*
 * Empty static training table (terminator-only).
 *
 * STATIC_TRAINING is defined for Armada 370 (see ddr3_axp_config.h), so
 * ddr3_static_training_init() walks ddr_mode->vals. RN102 relies purely on
 * the static MC register table and needs no per-register training writes,
 * so provide an empty (immediately-terminating) array rather than NULL,
 * which would otherwise be dereferenced.
 */
static MV_DRAM_TRAINING_INIT rn102_ddr3_training_empty[MV_MAX_DDR3_STATIC_SIZE] = {
	{0, 0}
};

MV_DRAM_MODES rn102_ddr_modes[MV_DDR3_MODES_NUMBER] = {
	{"rn102_1200-600", 0x6, 0x5, 0x0, A0, rn102_ddr3_600, rn102_ddr3_training_empty},
};

MV_DRAM_MODES *ddr3_get_static_ddr_mode(void)
{
	return &rn102_ddr_modes[0];
}

int board_early_init_f(void)
{
	int i;

	/* Set GPP Out value */
	reg_write(GPP_DATA_OUT_REG(0), RN102_GPP_OUT_VAL_LOW);
	reg_write(GPP_DATA_OUT_REG(1), RN102_GPP_OUT_VAL_MID);
	reg_write(GPP_DATA_OUT_REG(2), RN102_GPP_OUT_VAL_HIGH);

	/* Set GPP polarity */
	reg_write(GPP_DATA_IN_POL_REG(0), RN102_GPP_OUT_POL_LOW);
	reg_write(GPP_DATA_IN_POL_REG(1), RN102_GPP_OUT_POL_MID);
	reg_write(GPP_DATA_IN_POL_REG(2), RN102_GPP_OUT_POL_HIGH);

	/* Set GPP Out Enable */
	reg_write(GPP_DATA_OUT_EN_REG(0), RN102_GPP_OUT_ENA_LOW);
	reg_write(GPP_DATA_OUT_EN_REG(1), RN102_GPP_OUT_ENA_MID);
	reg_write(GPP_DATA_OUT_EN_REG(2), RN102_GPP_OUT_ENA_HIGH);

	/* Configure MPP pin multiplexing */
	for (i = 0; i < ARRAY_SIZE(rn102_mpp_control); i++)
		reg_write(MPP_CONTROL_REG(i), rn102_mpp_control[i]);

	return 0;
}

int board_init(void)
{
	u32 pwr_mng_ctrl_reg;

	/* Address of boot parameters */
	gd->bd->bi_boot_params = mvebu_sdram_bar(0) + 0x100;

	/*
	 * Gate unused clocks to reduce power consumption.
	 * The RN102 uses:
	 *   GE1 (RGMII1) for Ethernet -> keep
	 *   SATA0 (internal) for eSATA -> keep
	 *   PEX0, PEX1 for PCIe SATA and USB 3.0 -> keep
	 * Gate: Audio, SDIO, GE0, TDM, LCD, I2S
	 */
	pwr_mng_ctrl_reg = reg_read(POWER_MNG_CTRL_REG);
	pwr_mng_ctrl_reg &= ~(BIT(0));		/* Audio */
	pwr_mng_ctrl_reg &= ~(BIT(3));		/* GE0 */
	pwr_mng_ctrl_reg &= ~(BIT(4));		/* TDM */
	pwr_mng_ctrl_reg &= ~(BIT(16));		/* LCD */
	pwr_mng_ctrl_reg &= ~(BIT(17));		/* SDIO */
	reg_write(POWER_MNG_CTRL_REG, pwr_mng_ctrl_reg);

	return 0;
}

int board_late_init(void)
{
	/* Do late init to ensure successful enumeration of XHCI devices */
	pci_init();
	return 0;
}

int misc_init_r(void)
{
	return 0;
}
