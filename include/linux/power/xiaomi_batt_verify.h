/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Battery authentication result from the Xiaomi ds28e16 driver
 * (drivers/power/supply/xiaomi), for the charger and fuel gauge.
 */
#ifndef __XIAOMI_BATT_VERIFY_H__
#define __XIAOMI_BATT_VERIFY_H__

#include <linux/errno.h>
#include <linux/notifier.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_XIAOMI_BATT_VERIFY_DS28E16)
/*
 * 1: battery authenticated, 0: authentication failed,
 * -EAGAIN: still running, -ENODEV: no authentication IC/driver.
 */
int xm_batt_verify_authentic(void);
/* page 0 of the IC (16 bytes, cell vendor id in byte 0); 0 or -errno */
int xm_batt_verify_page0(u8 *buf);
/*
 * Called once the result is final (authentic or not). A late registrant
 * must check xm_batt_verify_authentic() itself.
 */
int xm_batt_verify_register_notifier(struct notifier_block *nb);
int xm_batt_verify_unregister_notifier(struct notifier_block *nb);
#else
static inline int xm_batt_verify_authentic(void) { return -ENODEV; }
static inline int xm_batt_verify_page0(u8 *buf) { return -ENODEV; }
static inline int xm_batt_verify_register_notifier(struct notifier_block *nb)
{ return 0; }
static inline int xm_batt_verify_unregister_notifier(struct notifier_block *nb)
{ return 0; }
#endif

#endif
