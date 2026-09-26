/*
 * ohci-brcm.c - OHCI host controller driver for Broadcom STB.
 *
 * Copyright (C) 2009 - 2017 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#include "ohci.h"

#define BRCM_DRIVER_DESC "OHCI Broadcom STB driver"

static const char hcd_name[] = "ohci-brcm";

#define hcd_to_ohci_priv(h) ((struct brcm_priv *)hcd_to_ohci(h)->priv)

struct brcm_priv {
	struct clk *clk;
	struct phy *phy;
#ifdef CONFIG_TIGER_DISABLE_WIFI_ON_POE
	bool allow_inhibit;
#endif
};

#ifdef CONFIG_TIGER_DISABLE_WIFI_ON_POE

/* BrightSign: special case for Tiger using PoE */
static unsigned int inhibit_internal_port = 0;
module_param(inhibit_internal_port, uint, S_IRUGO);

/* For OHCI, we only need these hooks when inhibiting */

static int (*org_hub_control)(struct usb_hcd *hcd,
                        u16 typeReq, u16 wValue, u16 wIndex,
                        char *buf, u16 wLength);
static int (*org_ohci_start)(struct usb_hcd *hcd);

static int ohci_brcm_start(struct usb_hcd *hcd)
{
        /* BrightSign: Disable WiFi port when using PoE. BS#29159 */
	/* This is only used on Tiger */
	struct brcm_priv *priv = hcd_to_ohci_priv(hcd);
        if (inhibit_internal_port && (priv->allow_inhibit)) {
		struct ohci_hcd *ohci = hcd_to_ohci(hcd);
		printk("ohci-brcm: preventing startup on internal port\n");
	        ohci->rh_state = OHCI_RH_HALTED;
		ohci_writel(ohci, (u32) ~0, &ohci->regs->intrdisable);

		/* Software reset, after which the controller goes into SUSPEND */
		ohci_writel(ohci, OHCI_HCR, &ohci->regs->cmdstatus);
		ohci_readl(ohci, &ohci->regs->cmdstatus);       /* flush the writes */

		return 0;
	}

	return (*org_ohci_start)(hcd);
}

static int ohci_brcm_hub_control(
        struct usb_hcd  *hcd,
        u16             typeReq,
        u16             wValue,
        u16             wIndex,
        char            *buf,
        u16             wLength)
{
        /* BrightSign: Disable WiFi port when using PoE. BS#29159 */
	struct brcm_priv *priv = hcd_to_ohci_priv(hcd);
        if (inhibit_internal_port && (priv->allow_inhibit)) {
                /* Force the power off every time an operation is attempted */
                /* printk("ohci-brcm: %hx %hx %hx\n", typeReq, wValue, wIndex); */
		struct ohci_hcd *ohci = hcd_to_ohci(hcd);
		ohci_writel(ohci, 0x00000201, &ohci->regs->roothub.portstatus[0x0]);
        }

        return (*org_hub_control)(hcd, typeReq, wValue, wIndex, buf, wLength);
}

#endif

#ifdef CONFIG_TIGER_DISABLE_WIFI_ON_POE
static int ohci_brcm_tiger_reset(struct usb_hcd *hcd)
{
	struct ohci_hcd *ohci = hcd_to_ohci(hcd);

	/* BrightSign: Special option for Tiger only to conditionally disable
	 * the first USB port on the second controller.  Needed for WiFi
	 * power disable when using PoE.  BS#29159
	 */
	{
		struct brcm_priv *priv = hcd_to_ohci_priv(hcd);
		if (inhibit_internal_port && priv->allow_inhibit) {
			int rc;
			printk("ohci-brcm: power off internal USB port\n");
			rc = ohci_setup(hcd);   /* Setup can reset power state */

			ohci_writel(ohci, 0x02000101, &ohci->regs->roothub.a);
			ohci_writel(ohci, 0x00020000, &ohci->regs->roothub.b);
			ohci_writel(ohci, 0x00000201, &ohci->regs->roothub.portstatus[0x0]);
			return rc;
		}
        }
	return ohci_setup(hcd);
}
#endif // CONFIG_TIGER_DISABLE_WIFI_ON_POE

static struct hc_driver __read_mostly ohci_brcm_hc_driver;

static const struct ohci_driver_overrides brcm_overrides __initconst = {
	.product_desc =	"Broadcom STB OHCI controller",
	.extra_priv_size = sizeof(struct brcm_priv),
#ifdef CONFIG_TIGER_DISABLE_WIFI_ON_POE
	.reset = ohci_brcm_tiger_reset,
#endif
};

static int ohci_brcm_probe(struct platform_device *pdev)
{
	struct usb_hcd *hcd;
	struct resource *res_mem;
	struct brcm_priv *priv;
	int irq;
	int err;

	if (usb_disabled())
		return -ENODEV;

	err = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		return err;

#ifdef CONFIG_TIGER_DISABLE_WIFI_ON_POE
	{
		/* BrightSign: Intercept hub control so we can optionally inhibit port */
		if (org_hub_control == NULL) {
			org_hub_control = ohci_brcm_hc_driver.hub_control;
			ohci_brcm_hc_driver.hub_control = ohci_brcm_hub_control;
		}
		if (org_ohci_start == NULL) {
			org_ohci_start = ohci_brcm_hc_driver.start;
			ohci_brcm_hc_driver.start = ohci_brcm_start;
		}
	}
#endif

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "platform_get_irq error.\n");
		return -ENODEV;
	}

	/* initialize hcd */
	hcd = usb_create_hcd(&ohci_brcm_hc_driver,
			&pdev->dev, dev_name(&pdev->dev));
	if (!hcd) {
		dev_err(&pdev->dev, "Failed to create hcd\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, hcd);
	priv = hcd_to_ohci_priv(hcd);

	priv->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->clk)) {
		if (PTR_ERR(priv->clk) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_err(&pdev->dev, "Clock not found in Device Tree\n");
		priv->clk = NULL;
	}
	err = clk_prepare_enable(priv->clk);
	if (err)
		goto err_hcd;

#ifdef CONFIG_TIGER_DISABLE_WIFI_ON_POE
	priv->allow_inhibit = of_property_read_bool(pdev->dev.of_node,
						    "allow-internal-inhibit");
#endif
	priv->phy = devm_of_phy_get_by_index(&pdev->dev, pdev->dev.of_node, 0);
	if (IS_ERR(priv->phy)) {
		err = PTR_ERR(priv->phy);
		if (err == -EPROBE_DEFER)
			dev_dbg(&pdev->dev, "DEFER, waiting for PHY\n");
		else
			dev_err(&pdev->dev, "USB Phy not found.\n");
		goto err_clk;
	}
	err = phy_init(priv->phy);
	if (err)
		goto err_clk;

	res_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	hcd->regs = devm_ioremap_resource(&pdev->dev, res_mem);
	if (IS_ERR(hcd->regs)) {
		err = PTR_ERR(hcd->regs);
		goto err_phy;
	}
	hcd->rsrc_start = res_mem->start;
	hcd->rsrc_len = resource_size(res_mem);

	err = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (err)
		goto err_phy;

	device_wakeup_enable(hcd->self.controller);

	platform_set_drvdata(pdev, hcd);

	return err;

err_phy:
	phy_exit(priv->phy);
err_clk:
	clk_disable_unprepare(priv->clk);
err_hcd:
	usb_put_hcd(hcd);

	return err;

}

static int ohci_brcm_remove(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);
	struct brcm_priv *priv = hcd_to_ohci_priv(hcd);

	usb_remove_hcd(hcd);
	phy_exit(priv->phy);
	clk_disable_unprepare(priv->clk);
	usb_put_hcd(hcd);
	return 0;
}

#ifdef CONFIG_PM_SLEEP

static int ohci_brcm_suspend(struct device *dev)
{
	int ret;
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct brcm_priv *priv = hcd_to_ohci_priv(hcd);
	bool do_wakeup = device_may_wakeup(dev);

	ret = ohci_suspend(hcd, do_wakeup);
	clk_disable_unprepare(priv->clk);
	return ret;
}

static int ohci_brcm_resume(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct brcm_priv *priv = hcd_to_ohci_priv(hcd);
	int err;

	err = clk_prepare_enable(priv->clk);
	if (err)
		return err;
	ohci_resume(hcd, false);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(ohci_brcm_pm_ops, ohci_brcm_suspend,
		ohci_brcm_resume);

#ifdef CONFIG_OF
static const struct of_device_id brcm_ohci_of_match[] = {
	{ .compatible = "brcm,ohci-brcm-v2", },
	{}
};

MODULE_DEVICE_TABLE(of, brcm_ohci_of_match);
#endif /* CONFIG_OF */

static struct platform_driver ohci_brcm_driver = {
	.probe		= ohci_brcm_probe,
	.remove		= ohci_brcm_remove,
	.shutdown	= usb_hcd_platform_shutdown,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "ohci-brcm",
		.pm	= &ohci_brcm_pm_ops,
		.of_match_table = of_match_ptr(brcm_ohci_of_match),
	}
};

static int __init ohci_brcm_init(void)
{
	if (usb_disabled())
		return -ENODEV;

	pr_info("%s: " BRCM_DRIVER_DESC "\n", hcd_name);

	ohci_init_driver(&ohci_brcm_hc_driver, &brcm_overrides);
	return platform_driver_register(&ohci_brcm_driver);
}
module_init(ohci_brcm_init);

static void __exit ohci_brcm_cleanup(void)
{
	platform_driver_unregister(&ohci_brcm_driver);
}
module_exit(ohci_brcm_cleanup);

MODULE_ALIAS("platform:ohci-brcm");
MODULE_DESCRIPTION(BRCM_DRIVER_DESC);
MODULE_AUTHOR("Al Cooper");
MODULE_LICENSE("GPL");
