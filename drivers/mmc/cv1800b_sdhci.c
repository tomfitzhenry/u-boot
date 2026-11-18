// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2024, Kongyang Liu <seashell11234455@gmail.com>
 */

#include <dm.h>
#include <mmc.h>
#include <sdhci.h>
#include <linux/delay.h>

#define SDHCI_PHY_TX_RX_DLY  0x240
#define MMC_MAX_CLOCK        375000000
#define TUNE_MAX_PHCODE      128

#define PHY_TX_SRC_INVERT  BIT(8)

/*
 * CV1800B vendor register area 1.  The DWC MSHC "P_VENDOR_SPECIFIC_AREA1"
 * pointer is 0x200 on this SoC, so Linux sdhci-of-dwcmshc.c's
 * CV18XX_SDHCI_* offsets are used here as absolute offsets.
 *
 * sdhci_setup_cfg()/sdhci_init() reset the standard SDHCI registers but do
 * not restore these vendor ones, while Linux's cv18xx_sdhci_reset() programs
 * them on every reset: eMMC function enable + 1T latency, the PHY TX bypass,
 * and the RX/TX clock source inversion.  Without them the eMMC receive
 * sampling window is only good enough for the low boot clock.
 */
#define CV1800B_MSHC_CTRL		0x200
#define  CV1800B_MSHC_CTRL_EMMC_FUNC_EN	BIT(0)
#define  CV1800B_MSHC_CTRL_LATANCY_1T	BIT(1)
#define CV1800B_PHY_TX_RX_DLY		0x240
#define  CV1800B_PHY_TX_SRC_INVERT_TX	BIT(8)
#define  CV1800B_PHY_RX_SRC_INVERT_RX	BIT(24)
#define CV1800B_PHY_CONFIG		0x24c
#define  CV1800B_PHY_CONFIG_TX_BPS	BIT(0)

struct cv1800b_sdhci_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

#if CONFIG_IS_ENABLED(MMC_SUPPORTS_TUNING)
static void cv1800b_set_tap_delay(struct sdhci_host *host, u16 tap)
{
	sdhci_writel(host, PHY_TX_SRC_INVERT | tap << 16, SDHCI_PHY_TX_RX_DLY);
}

static void cv1800b_sdhci_reset(struct sdhci_host *host, u8 mask)
{
	sdhci_writeb(host, mask, SDHCI_SOFTWARE_RESET);
	while (sdhci_readb(host, SDHCI_SOFTWARE_RESET) & mask)
		udelay(10);
}

static int cv1800b_execute_tuning(struct mmc *mmc, u8 opcode)
{
	struct sdhci_host *host = dev_get_priv(mmc->dev);

	u16 tap;

	int current_size = 0;
	int max_size = 0;
	int max_window = 0;

	for (tap = 0; tap < TUNE_MAX_PHCODE; tap++) {
		cv1800b_set_tap_delay(host, tap);

		if (mmc_send_tuning(host->mmc, opcode)) {
			current_size = 0;
		} else {
			current_size++;
			if (current_size > max_size) {
				max_size = current_size;
				max_window = tap;
			}
		}
	}

	cv1800b_sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);

	cv1800b_set_tap_delay(host, max_window - max_size / 2);

	return 0;
}
#endif

/*
 * Program the CV1800B vendor PHY/function registers exactly as Linux's
 * sdhci-of-dwcmshc.c does in cv18xx_sdhci_reset(), after the SDHCI core reset
 * has run.  On the Oz64 this turns a link that read reliably only at the
 * 13 MHz boot clock into one that reads reliably at 26/52 MHz too.
 */
static void cv1800b_sdhci_vendor_init(struct sdhci_host *host)
{
	u32 val;

	val = sdhci_readl(host, CV1800B_MSHC_CTRL);
	val |= CV1800B_MSHC_CTRL_EMMC_FUNC_EN | CV1800B_MSHC_CTRL_LATANCY_1T;
	sdhci_writel(host, val, CV1800B_MSHC_CTRL);

	val = sdhci_readl(host, CV1800B_PHY_CONFIG);
	val |= CV1800B_PHY_CONFIG_TX_BPS;
	sdhci_writel(host, val, CV1800B_PHY_CONFIG);

	val = CV1800B_PHY_TX_SRC_INVERT_TX | CV1800B_PHY_RX_SRC_INVERT_RX;
	sdhci_writel(host, val, CV1800B_PHY_TX_RX_DLY);
}

const struct sdhci_ops cv1800b_sdhci_sd_ops = {
#if CONFIG_IS_ENABLED(MMC_SUPPORTS_TUNING)
	.platform_execute_tuning = cv1800b_execute_tuning,
#endif
};

static int cv1800b_sdhci_bind(struct udevice *dev)
{
	struct cv1800b_sdhci_plat *plat = dev_get_plat(dev);

	return sdhci_bind(dev, &plat->mmc, &plat->cfg);
}

static int cv1800b_sdhci_probe(struct udevice *dev)
{
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct cv1800b_sdhci_plat *plat = dev_get_plat(dev);
	struct sdhci_host *host = dev_get_priv(dev);
	u32 f_max;
	int ret;

	host->name = dev->name;
	host->ioaddr = dev_read_addr_ptr(dev);

	upriv->mmc = &plat->mmc;
	host->mmc = &plat->mmc;
	host->mmc->priv = host;
	host->mmc->dev = dev;
	host->ops = &cv1800b_sdhci_sd_ops;
	host->max_clk = MMC_MAX_CLOCK;

	if (dev_read_bool(dev, "no-1-8-v"))
		host->quirks |= SDHCI_QUIRK_NO_1_8_V;

	ret = mmc_of_parse(dev, &plat->cfg);
	if (ret)
		return ret;

	/*
	 * sdhci_setup_cfg() otherwise overwrites f_max with host->max_clk and
	 * unconditionally advertises 4-bit, so honour the DT max-frequency and
	 * the DT bus width (the jukebox board needs 26 MHz to avoid 52 MHz x4
	 * read errors).
	 */
	f_max = plat->cfg.f_max;
	ret = sdhci_setup_cfg(&plat->cfg, host, f_max, 200000);
	if (ret)
		return ret;

	if (dev_read_u32_default(dev, "bus-width", 1) == 1)
		plat->cfg.host_caps &= ~(MMC_MODE_4BIT | MMC_MODE_8BIT);

	ret = sdhci_probe(dev);
	if (ret)
		return ret;

	/*
	 * sdhci_probe() -> sdhci_init() -> SDHCI_RESET_ALL has just run; apply
	 * the vendor PHY/function setup now, before the first command.
	 */
	cv1800b_sdhci_vendor_init(host);

	return 0;
}

static const struct udevice_id cv1800b_sdhci_match[] = {
	{ .compatible = "sophgo,cv1800b-dwcmshc" },
	{ }
};

U_BOOT_DRIVER(cv1800b_sdhci) = {
	.name = "sdhci-cv1800b",
	.id = UCLASS_MMC,
	.of_match = cv1800b_sdhci_match,
	.bind = cv1800b_sdhci_bind,
	.probe = cv1800b_sdhci_probe,
	.priv_auto = sizeof(struct sdhci_host),
	.plat_auto = sizeof(struct cv1800b_sdhci_plat),
	.ops = &sdhci_ops,
};
