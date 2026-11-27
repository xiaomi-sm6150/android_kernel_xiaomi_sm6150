// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "qti_typec_class.h"

int qti_typec_partner_register(struct typec_role_class *chip, int mode)
{
	if (!chip || !chip->typec_port)
		return -ENODEV;

	if (!chip->typec_partner) {
		chip->typec_partner = typec_register_partner(chip->typec_port,
						&chip->typec_partner_desc);
		if (IS_ERR_OR_NULL(chip->typec_partner))
			return PTR_ERR(chip->typec_partner);
	}

	if (mode == TYPEC_DEVICE) {
		typec_set_data_role(chip->typec_port, TYPEC_DEVICE);
		typec_set_pwr_role(chip->typec_port, TYPEC_SINK);
	} else {
		typec_set_data_role(chip->typec_port, TYPEC_HOST);
		typec_set_pwr_role(chip->typec_port, TYPEC_SOURCE);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(qti_typec_partner_register);

void qti_typec_partner_unregister(struct typec_role_class *chip)
{
	if (!chip || !chip->typec_port)
		return;

	if (chip->typec_partner) {
		typec_unregister_partner(chip->typec_partner);
		chip->typec_partner = NULL;
	}
}
EXPORT_SYMBOL_GPL(qti_typec_partner_unregister);

struct typec_role_class *qti_typec_class_init(struct device *dev)
{
	struct typec_role_class *chip;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return ERR_PTR(-ENOMEM);

	chip->typec_caps.type = TYPEC_PORT_DRP;
	chip->typec_caps.data = TYPEC_PORT_DRD;
	chip->typec_caps.revision = 0x0130;
	chip->typec_partner_desc.usb_pd = false;
	chip->typec_partner_desc.accessory = TYPEC_ACCESSORY_NONE;

	chip->typec_port = typec_register_port(dev, &chip->typec_caps);
	if (IS_ERR_OR_NULL(chip->typec_port))
		return ERR_PTR(-ENODEV);

	return chip;
}
EXPORT_SYMBOL_GPL(qti_typec_class_init);

void qti_typec_class_deinit(struct typec_role_class *chip)
{
	if (!chip || !chip->typec_port)
		return;

	qti_typec_partner_unregister(chip);
	typec_unregister_port(chip->typec_port);
	chip->typec_port = NULL;
}
EXPORT_SYMBOL_GPL(qti_typec_class_deinit);

MODULE_DESCRIPTION("QTI typec class driver");
MODULE_LICENSE("GPL");
