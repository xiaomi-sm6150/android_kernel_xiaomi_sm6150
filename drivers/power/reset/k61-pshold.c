// SPDX-License-Identifier: GPL-2.0-only
/*
 * DEBUG (violet bring-up): restart by pulling PS_HOLD directly.
 *
 * PSCI SYSTEM_RESET goes through TZ, which does a PMIC hard reset and so
 * wipes DDR. The 4.14 kernel that kexecs us has already set the PMIC
 * PS_HOLD reset type to warm, and 4.14 itself resets by dropping PS_HOLD,
 * so do the same here at a higher priority than PSCI. With a warm reset,
 * the ramoops log of this kernel survives into the next 4.14 boot.
 *
 * Bring-up only; drop once the pm6150 PON driver is ported.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/delay.h>

#define K61_PSHOLD_BASE	0x0c264000

static void __iomem *k61_pshold;

static int k61_pshold_restart(struct notifier_block *nb, unsigned long mode,
			      void *cmd)
{
	pr_emerg("k61: restart by PS_HOLD\n");
	writel(0, k61_pshold);
	mdelay(10000);
	return NOTIFY_DONE;
}

static struct notifier_block k61_pshold_nb = {
	.notifier_call = k61_pshold_restart,
	.priority = 255,
};

static int __init k61_pshold_init(void)
{
	k61_pshold = ioremap(K61_PSHOLD_BASE, 4);
	if (!k61_pshold)
		return -ENOMEM;
	register_restart_handler(&k61_pshold_nb);
	pr_info("k61: PS_HOLD restart handler registered\n");
	return 0;
}
early_initcall(k61_pshold_init);
