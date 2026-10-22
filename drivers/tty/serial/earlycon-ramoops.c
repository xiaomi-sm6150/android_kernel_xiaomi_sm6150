// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot console that logs into a ramoops zone.
 *
 * Meant for bring-up on devices without a UART: from the first printk on,
 * the kernel log is written into a ramoops zone using the persistent_ram
 * buffer layout, so after a warm reset it shows up in /sys/fs/pstore on
 * the next boot, even when that boot runs a different kernel.
 *
 *   earlycon=ramoops,<phys addr of the zone>[,<zone size>]
 *
 * The zone size defaults to 256K, the most a single early_ioremap() can
 * map. Use a zone that nothing else writes to early, e.g. the pmsg zone.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/serial_core.h>
#include <linux/sizes.h>

#include <asm/early_ioremap.h>

#define PERSISTENT_RAM_SIG	0x43474244	/* DBGC, see fs/pstore/ram_core.c */

struct ramoops_buffer {
	u32 sig;
	u32 start;
	u32 size;
	u8 data[];
};

static struct ramoops_buffer __iomem *rbuf;
static phys_addr_t rbuf_phys;
static size_t rbuf_map_size;
static u32 rbuf_size;

static void earlycon_ramoops_write(struct console *con, const char *s,
				   unsigned int n)
{
	u32 start, size;

	if (!rbuf)
		return;

	start = readl_relaxed(&rbuf->start);
	size = readl_relaxed(&rbuf->size);
	while (n--) {
		writeb_relaxed(*s++, &rbuf->data[start]);
		if (++start == rbuf_size)
			start = 0;
		if (size < rbuf_size)
			size++;
	}
	writel_relaxed(start, &rbuf->start);
	writel_relaxed(size, &rbuf->size);
	/* make sure it reached DDR before a reset can hit */
	mb();
}

static int __init earlycon_ramoops_setup(struct earlycon_device *dev,
					 const char *opt)
{
	unsigned long long size = SZ_256K;

	if (!dev->port.mapbase)
		return -ENODEV;
	if (opt && *opt && kstrtoull(opt, 0, &size))
		return -EINVAL;
	if (size <= sizeof(*rbuf) || size > SZ_256K)
		return -EINVAL;

	rbuf_phys = dev->port.mapbase;
	rbuf_map_size = size;
	rbuf = early_ioremap(rbuf_phys, rbuf_map_size);
	if (!rbuf)
		return -ENOMEM;
	rbuf_size = size - sizeof(*rbuf);

	writel_relaxed(0, &rbuf->start);
	writel_relaxed(0, &rbuf->size);
	writel_relaxed(PERSISTENT_RAM_SIG, &rbuf->sig);
	mb();

	dev->con->write = earlycon_ramoops_write;
	return 0;
}
EARLYCON_DECLARE(ramoops, earlycon_ramoops_setup);

/* Move off the early fixmap mapping once ioremap() works. */
static int __init earlycon_ramoops_remap(void)
{
	struct ramoops_buffer __iomem *old = rbuf, *new;

	if (!old)
		return 0;

	new = ioremap(rbuf_phys, rbuf_map_size);
	if (!new)
		return 0;

	console_lock();
	rbuf = new;
	console_unlock();
	early_iounmap(old, rbuf_map_size);
	return 0;
}
early_initcall(earlycon_ramoops_remap);
