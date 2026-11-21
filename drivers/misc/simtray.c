// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi SIM tray status: exposes the tray detect GPIO as the "status"
 * attribute of the simtray platform device (1 = tray inserted on sweet),
 * read by the vendor RIL.
 *
 * Port of the 4.14 driver by Tao Jun <taojun@xiaomi.com>.
 */
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

struct simtray_data {
	int status_gpio;
};

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct simtray_data *data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gpio_get_value(data->status_gpio));
}
static DEVICE_ATTR_RO(status);

static struct attribute *simtray_attrs[] = {
	&dev_attr_status.attr,
	NULL
};
ATTRIBUTE_GROUPS(simtray);

static int simtray_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct simtray_data *data;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->status_gpio = of_get_named_gpio(dev->of_node, "status-gpio", 0);
	if (!gpio_is_valid(data->status_gpio))
		return data->status_gpio == -EPROBE_DEFER ? -EPROBE_DEFER : -EINVAL;

	/* 4.14 read the pin without claiming it; claim it as an input */
	ret = devm_gpio_request_one(dev, data->status_gpio, GPIOF_IN,
				    "simtray-status");
	if (ret)
		return ret;

	platform_set_drvdata(pdev, data);
	return 0;
}

static const struct of_device_id simtray_of_match[] = {
	{ .compatible = "xiaomi,simtray-status", },
	{},
};
MODULE_DEVICE_TABLE(of, simtray_of_match);

static struct platform_driver simtray_status_driver = {
	.driver = {
		.name = "simtray-status",
		.of_match_table = simtray_of_match,
		.dev_groups = simtray_groups,
	},
	.probe = simtray_probe,
};
module_platform_driver(simtray_status_driver);

MODULE_AUTHOR("Tao Jun <taojun@xiaomi.com>");
MODULE_DESCRIPTION("Xiaomi SIM tray status");
MODULE_LICENSE("GPL");
