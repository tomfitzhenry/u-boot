// SPDX-License-Identifier: GPL-2.0+
/*
 * Internal ePHY bring-up for the Sophgo SG2000 (CV1800B-compatible) GMAC.
 *
 * The MAC's MDIO bus only reaches the SoC's internal ePHY after its analog
 * and digital blocks are taken out of reset and its ID registers are set;
 * Linux has a dedicated driver for this, U-Boot does not.  These register
 * writes are the same ones the Milk-V Duo board code performs for the
 * legacy network stack (board/sophgo/milkv_duo/ethernet.c).
 */

#include <linux/io.h>
#include <linux/bitops.h>
#include <linux/mii.h>

#include "ethernet.h"

#define REG_EPHY_TOP_WRAP	((u32 *)0x03009800)
#define REG_EPHY_BASE		((u32 *)0x03009000)

#define REG_EPHY_CTL		REG_EPHY_TOP_WRAP
#define REG_EPHY_APB_RW_SEL	(REG_EPHY_TOP_WRAP + 1)

/* Page 0 registers */
#define REG_PHY_ID1		(REG_EPHY_BASE + MII_PHYSID1)
#define REG_PHY_ID2		(REG_EPHY_BASE + MII_PHYSID2)
#define REG_PHY_PAGE_SEL	(REG_EPHY_BASE + 0x1f)

/* Page 5 register */
#define REG_PD_EN_CTL		(REG_EPHY_BASE + 0x10)

#define REG_EPHY_SHUTDOWN	BIT(0)
#define REG_EPHY_ANA_RST_N	BIT(1)
#define REG_EPHY_DIG_RST_N	BIT(2)
#define REG_EPHY_MAIN_RST_N	BIT(3)

#define REG_EN_ETH_TXRT		BIT(0)
#define REG_EN_ETH_CLK100M	BIT(1)
#define REG_EN_ETH_CLK125M	BIT(2)
#define REG_EN_ETH_PLL_LCKDET	BIT(3)
#define REG_EN_ETH_RXADC	BIT(4)
#define REG_EN_ETH_RXPGA	BIT(5)
#define REG_EN_ETH_RXRT		BIT(6)
#define REG_PD_ETH_PLL		BIT(8)
#define REG_PD_ETH_TXDAC	BIT(9)
#define REG_PD_ETH_TXDACBST	BIT(10)
#define REG_PD_ETH_TXECHO	BIT(11)
#define REG_PD_ETH_TXDRV_NMOS	BIT(12)
#define REG_PD_ETH_TXLDO	BIT(13)

void cv1800b_ephy_init(void)
{
	u32 reg;

	/* Enable direct memory access for PHY registers. */
	writel(1, REG_EPHY_APB_RW_SEL);

	reg = readl(REG_EPHY_CTL);
	reg &= ~REG_EPHY_SHUTDOWN;
	reg |= REG_EPHY_ANA_RST_N | REG_EPHY_DIG_RST_N | REG_EPHY_MAIN_RST_N;
	writel(reg, REG_EPHY_CTL);

	/* Page 5: unpower the PHY blocks and enable the RX/TX chains. */
	writel(5 << 8, REG_PHY_PAGE_SEL);
	reg = readl(REG_PD_EN_CTL);
	reg &= ~(REG_PD_ETH_TXLDO | REG_PD_ETH_TXDRV_NMOS |
		 REG_PD_ETH_TXDAC | REG_PD_ETH_PLL);
	reg |= REG_EN_ETH_TXRT | REG_EN_ETH_CLK100M | REG_EN_ETH_CLK125M |
	       REG_EN_ETH_PLL_LCKDET | REG_EN_ETH_RXADC | REG_EN_ETH_RXPGA |
	       REG_EN_ETH_RXRT;
	writel(reg, REG_PD_EN_CTL);

	/* Page 0: give the unidentified internal PHY a generic PHY ID. */
	writel(0 << 8, REG_PHY_PAGE_SEL);
	writel(1 >> 16, REG_PHY_ID1);
	writel(1 & 0xffff, REG_PHY_ID2);

	/* Back to MDIO access. */
	writel(0, REG_EPHY_APB_RW_SEL);
}
