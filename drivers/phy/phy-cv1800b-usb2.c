// SPDX-License-Identifier: GPL-2.0+
/*
 * Sophgo CV1800B/SG2000 USB 2.0 PHY.
 *
 * The PHY is controlled through a register in the SoC top syscon.  U-Boot
 * only needs host mode: select the host ID override and power VBUS.  The
 * vendor FSBL leaves the USB clocks and reset released, so no clock/reset
 * consumer is needed here.
 */

#include <dm.h>
#include <dm/device.h>
#include <generic-phy.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <regmap.h>
#include <syscon.h>

#define REG_USB_PHY_CTRL	0x048

#define PHY_VBUS_POWER_EN	BIT(0)
#define PHY_VBUS_POWER		BIT(1)
#define PHY_ID_OVERWRITE_EN	BIT(6)
#define PHY_ID_OVERWRITE_MODE	BIT(7)

struct cv1800b_usb2_phy_priv {
	struct regmap *regmap;
};

static int cv1800b_usb2_phy_power_on(struct phy *phy)
{
	struct cv1800b_usb2_phy_priv *priv = dev_get_priv(phy->dev);

	return regmap_update_bits(priv->regmap, REG_USB_PHY_CTRL,
				  PHY_VBUS_POWER_EN | PHY_VBUS_POWER |
				  PHY_ID_OVERWRITE_EN | PHY_ID_OVERWRITE_MODE,
				  PHY_VBUS_POWER_EN | PHY_VBUS_POWER |
				  PHY_ID_OVERWRITE_EN);
}

static int cv1800b_usb2_phy_power_off(struct phy *phy)
{
	struct cv1800b_usb2_phy_priv *priv = dev_get_priv(phy->dev);

	return regmap_update_bits(priv->regmap, REG_USB_PHY_CTRL,
				  PHY_VBUS_POWER, 0);
}

static int cv1800b_usb2_phy_probe(struct udevice *dev)
{
	struct cv1800b_usb2_phy_priv *priv = dev_get_priv(dev);

	priv->regmap =
		syscon_node_to_regmap(dev_ofnode(dev_get_parent(dev)));
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	return 0;
}

static const struct phy_ops cv1800b_usb2_phy_ops = {
	.power_on	= cv1800b_usb2_phy_power_on,
	.power_off	= cv1800b_usb2_phy_power_off,
};

static const struct udevice_id cv1800b_usb2_phy_ids[] = {
	{ .compatible = "sophgo,cv1800b-usb2-phy" },
	{ }
};

U_BOOT_DRIVER(cv1800b_usb2_phy) = {
	.name		= "cv1800b-usb2-phy",
	.id		= UCLASS_PHY,
	.of_match	= cv1800b_usb2_phy_ids,
	.probe		= cv1800b_usb2_phy_probe,
	.ops		= &cv1800b_usb2_phy_ops,
	.priv_auto	= sizeof(struct cv1800b_usb2_phy_priv),
};
