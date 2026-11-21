// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016  xiaomi Inc.
 * Copyright (C) 2021 XiaoMi, Inc.
 *
 * Bit-banged 1-Wire master on one TLMM pin, for the ds28e16 battery
 * authentication IC. The pin is driven through its TLMM config/in_out
 * registers directly (mi,onewire-gpio-cfg-addr = <cfg_reg in_out_offset>)
 * because gpiolib calls are too slow for the 1 us 1-Wire slots.
 */
#define pr_fmt(fmt)	"[Onewire] %s: " fmt, __func__

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include "onewire_gpio.h"

#define DRV_STRENGTH_4MA		(0x1 << 6)
#define GPIO_OUTPUT			(0x1 << 9)
#define GPIO_INPUT			(0x0 << 9)
#define GPIO_PULL_UP			0x3
#define OUTPUT_HIGH			(0x1 << 1)
#define OUTPUT_LOW			0x1

struct onewire_gpio_data {
	void __iomem *cfg_reg;
	void __iomem *in_out_reg;
	raw_spinlock_t lock;
};

static struct onewire_gpio_data *g_ow;

static inline void ow_config_out(void)
{
	writel_relaxed(DRV_STRENGTH_4MA | GPIO_OUTPUT | GPIO_PULL_UP, g_ow->cfg_reg);
}

static inline void ow_config_in(void)
{
	writel_relaxed(DRV_STRENGTH_4MA | GPIO_INPUT | GPIO_PULL_UP, g_ow->cfg_reg);
}

static inline void ow_out_high(void)
{
	writel_relaxed(OUTPUT_HIGH, g_ow->in_out_reg);
}

static inline void ow_out_low(void)
{
	writel_relaxed(OUTPUT_LOW, g_ow->in_out_reg);
}

bool xm_ow_ready(void)
{
	return g_ow != NULL;
}
EXPORT_SYMBOL_GPL(xm_ow_ready);

/* returns 0 when a device answered the reset with a presence pulse */
unsigned char xm_ow_reset(void)
{
	unsigned char presence;
	unsigned long flags;

	raw_spin_lock_irqsave(&g_ow->lock, flags);
	ow_config_out();
	ow_out_low();
	udelay(50);
	ow_out_high();
	ow_config_in();
	udelay(7);
	presence = (unsigned char)readl_relaxed(g_ow->in_out_reg) & 0x01;
	udelay(50);
	raw_spin_unlock_irqrestore(&g_ow->lock, flags);

	return presence;
}
EXPORT_SYMBOL_GPL(xm_ow_reset);

static unsigned char ow_read_bit(void)
{
	unsigned int v;

	ow_config_out();
	ow_out_low();
	udelay(1);
	ow_config_in();
	ndelay(100);
	v = readl_relaxed(g_ow->in_out_reg);
	udelay(5);
	ow_out_high();
	ow_config_out();
	udelay(6);

	return (unsigned char)v & 0x01;
}

static void ow_write_bit(char bitval)
{
	ow_out_low();
	udelay(1);
	if (bitval != 0)
		ow_out_high();
	udelay(10);
	ow_out_high();
	udelay(6);
}

unsigned char xm_ow_read_byte(void)
{
	unsigned char i, value = 0;
	unsigned long flags;

	raw_spin_lock_irqsave(&g_ow->lock, flags);
	for (i = 0; i < 8; i++)
		if (ow_read_bit())
			value |= 0x01 << i;
	raw_spin_unlock_irqrestore(&g_ow->lock, flags);

	return value;
}
EXPORT_SYMBOL_GPL(xm_ow_read_byte);

void xm_ow_write_byte(char val)
{
	unsigned char i;
	unsigned long flags;

	raw_spin_lock_irqsave(&g_ow->lock, flags);
	ow_config_out();
	for (i = 0; i < 8; i++)
		ow_write_bit((val >> i) & 0x01);
	raw_spin_unlock_irqrestore(&g_ow->lock, flags);
}
EXPORT_SYMBOL_GPL(xm_ow_write_byte);

static int onewire_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct onewire_gpio_data *ow;
	struct pinctrl *pinctrl;
	struct pinctrl_state *active;
	u32 reg[2];
	int gpio, rc;

	ow = devm_kzalloc(dev, sizeof(*ow), GFP_KERNEL);
	if (!ow)
		return -ENOMEM;

	rc = of_property_read_u32_array(dev->of_node, "mi,onewire-gpio-cfg-addr",
					reg, 2);
	if (rc) {
		dev_err(dev, "mi,onewire-gpio-cfg-addr missing: %d\n", rc);
		return rc;
	}

	pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pinctrl))
		return dev_err_probe(dev, PTR_ERR(pinctrl), "no pinctrl\n");
	active = pinctrl_lookup_state(pinctrl, "onewire_active");
	if (IS_ERR(active))
		return dev_err_probe(dev, PTR_ERR(active), "no onewire_active state\n");
	rc = pinctrl_select_state(pinctrl, active);
	if (rc)
		return dev_err_probe(dev, rc, "can't select onewire_active\n");

	gpio = of_get_named_gpio(dev->of_node, "xiaomi,ow_gpio", 0);
	if (!gpio_is_valid(gpio))
		return dev_err_probe(dev, gpio < 0 ? gpio : -EINVAL,
				     "xiaomi,ow_gpio invalid\n");
	rc = devm_gpio_request_one(dev, gpio, GPIOF_OUT_INIT_HIGH, "onewire gpio");
	if (rc)
		return dev_err_probe(dev, rc, "can't request gpio %d\n", gpio);

	/* TLMM is already mapped by the pinctrl driver: map, don't request */
	ow->cfg_reg = devm_ioremap(dev, reg[0], 0x4);
	ow->in_out_reg = devm_ioremap(dev, reg[0] + reg[1], 0x4);
	if (!ow->cfg_reg || !ow->in_out_reg)
		return -ENOMEM;

	raw_spin_lock_init(&ow->lock);
	platform_set_drvdata(pdev, ow);
	g_ow = ow;

	dev_info(dev, "gpio %d, cfg 0x%x, in_out 0x%x\n", gpio, reg[0],
		 reg[0] + reg[1]);
	return 0;
}

static int onewire_gpio_remove(struct platform_device *pdev)
{
	g_ow = NULL;
	return 0;
}

static const struct of_device_id onewire_gpio_dt_match[] = {
	{ .compatible = "xiaomi,onewire_gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, onewire_gpio_dt_match);

static struct platform_driver onewire_gpio_driver = {
	.driver = {
		.name = "onewire_gpio",
		.of_match_table = onewire_gpio_dt_match,
		/* the ds28e16 user holds no device link: don't unbind under it */
		.suppress_bind_attrs = true,
	},
	.probe = onewire_gpio_probe,
	.remove = onewire_gpio_remove,
};
module_platform_driver(onewire_gpio_driver);

MODULE_AUTHOR("xiaomi Inc.");
MODULE_DESCRIPTION("onewire driver");
MODULE_LICENSE("GPL");
