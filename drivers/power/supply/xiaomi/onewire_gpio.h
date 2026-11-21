/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ONEWIRE_GPIO_H__
#define __ONEWIRE_GPIO_H__

#include <linux/types.h>

bool xm_ow_ready(void);
unsigned char xm_ow_reset(void);
unsigned char xm_ow_read_byte(void);
void xm_ow_write_byte(char val);

#endif
