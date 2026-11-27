/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef __QTI_TYPEC_CLASS_H
#define __QTI_TYPEC_CLASS_H

#include <linux/usb/typec.h>

struct typec_role_class {
	struct typec_port		*typec_port;
	struct typec_capability		typec_caps;
	struct typec_partner		*typec_partner;
	struct typec_partner_desc	typec_partner_desc;
};

#if IS_ENABLED(CONFIG_QTI_TYPEC_CLASS)
int qti_typec_partner_register(struct typec_role_class *chip, int mode);
void qti_typec_partner_unregister(struct typec_role_class *chip);
struct typec_role_class *qti_typec_class_init(struct device *dev);
void qti_typec_class_deinit(struct typec_role_class *chip);
#else
static inline int qti_typec_partner_register(struct typec_role_class *chip,
					      int mode)
{
	return -ENODEV;
}

static inline void qti_typec_partner_unregister(struct typec_role_class *chip)
{
}

static inline struct typec_role_class *qti_typec_class_init(struct device *dev)
{
	return NULL;
}

static inline void qti_typec_class_deinit(struct typec_role_class *chip)
{
}
#endif /* CONFIG_QTI_TYPEC_CLASS */
#endif /* __QTI_TYPEC_CLASS_H */
