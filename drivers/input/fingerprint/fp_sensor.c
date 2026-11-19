// SPDX-License-Identifier: GPL-2.0-only
/*
 * Which fingerprint sensor the bootloader reports: violet units carry either
 * an FPC1020 (androidboot.fpsensor=fpc) or a Goodix GF3208 (=gdx). The 4.14
 * kernel parsed this in init/main.c; 1 = FPC, 2 = Goodix.
 */
#include <linux/init.h>
#include <linux/string.h>

int fpsensor = 2;

static int __init fpsensor_setup(char *str)
{
	fpsensor = strncmp(str, "fpc", 3) ? 2 : 1;
	return 1;
}
__setup("androidboot.fpsensor=", fpsensor_setup);
