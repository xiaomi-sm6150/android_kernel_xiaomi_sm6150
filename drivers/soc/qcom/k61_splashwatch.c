// SPDX-License-Identifier: GPL-2.0-only
/*
 * DEBUG: watch the hardware that keeps the bootloader splash on glass and
 * log every change with a timestamp (k61.splashwatch=1). For finding what
 * blanks the violet splash about 2 s into boot.
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/crc32.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/sched/clock.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "k61."

static bool splashwatch;
module_param_named(splashwatch, splashwatch, bool, 0444);

#define GDSC_PWR_ON	BIT(31)
#define CBCR_OFF	BIT(31)

struct k61_reg {
	const char *name;
	phys_addr_t pa;
	int gated;		/* only read while MDSS GDSC + MDP AHB clock are on */
	void __iomem *va;
	u32 last;
	bool have;
};

static struct k61_reg regs[] = {
	{ "gcc_disp_ahb_cbcr",	0x0010b008, 0 },
	{ "gcc_disp_hf_axi_cbcr",	0x0010b038, 0 },
	{ "mdss_gdscr",		0x0af03000, 0 },
	{ "dcc_mdss_ahb_cbcr",	0x0af02048, 0 },
	{ "dcc_mdp_cbcr",	0x0af02008, 0 },
	{ "dcc_pclk0_cbcr",	0x0af02004, 0 },
	{ "dcc_byte0_cbcr",	0x0af02024, 0 },
	{ "dcc_esc0_cbcr",	0x0af0202c, 0 },
	{ "dcc_vsync_cbcr",	0x0af02020, 0 },
	{ "tlmm91_rst_ctl",	0x0355b000, 0 },
	{ "tlmm91_rst_io",	0x0355b004, 0 },
	{ "smmu_cb0_sctlr",	0x15040000, 0 },
	{ "smmu_cb1_sctlr",	0x15041000, 0 },
	{ "smmu_cb2_sctlr",	0x15042000, 0 },
	{ "smmu_cb2_ttbr0_lo",	0x15042020, 0 },
	{ "smmu_cb2_fsr",	0x15042058, 0 },
	{ "smmu_cb2_far_lo",	0x15042060, 0 },
	{ "smmu_cb2_far_hi",	0x15042064, 0 },
	{ "intf1_tg_en",	0x0ae6b800, 1 },
	{ "vig0_src_size",	0x0ae05000, 1 },
	{ "vig0_src0_addr",	0x0ae05014, 1 },
	{ "vig0_ystride0",	0x0ae05024, 1 },
	{ "dma0_src_size",	0x0ae25000, 1 },
	{ "dma0_src0_addr",	0x0ae25014, 1 },
	{ "dma0_ystride0",	0x0ae25024, 1 },
	{ "dma1_src0_addr",	0x0ae27014, 1 },
	{ "dma2_src0_addr",	0x0ae29014, 1 },
	{ "dma3_src0_addr",	0x0ae2b014, 1 },
	{ "dsi0_ctrl",		0x0ae94004, 1 },
};

/* pm6150l (SID 5) WLED and LCDB, read through the PMIC regmap */
static const struct { const char *name; u16 reg; } pmic_regs[] = {
	{ "wled_fault_sts",	0xd808 },
	{ "wled_int_rt_sts",	0xd810 },
	{ "wled_mod_en",	0xd846 },
	{ "wled_sink_en",	0xd946 },
	{ "wled_str0_mod_en",	0xd950 },
	{ "wled_str0_cabc",	0xd956 },
	{ "wled_str0_bri_lsb",	0xd957 },
	{ "wled_str0_bri_msb",	0xd958 },
	{ "wled_str1_mod_en",	0xd960 },
	{ "wled_str1_cabc",	0xd966 },
	{ "wled_str1_bri_lsb",	0xd967 },
	{ "wled_str1_bri_msb",	0xd968 },
	{ "lcdb_sts1",		0xec08 },
	{ "lcdb_bst_vout",	0xec41 },
	{ "lcdb_enable_ctl1",	0xec46 },
};

static struct regmap *k61_pmic_regmap(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct regmap *map = NULL;

	np = of_find_node_by_path("/soc/qcom,spmi@c440000/qcom,pm6150l@5/qcom,wled@d800");
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return NULL;
	if (pdev->dev.parent)
		map = dev_get_regmap(pdev->dev.parent, NULL);
	put_device(&pdev->dev);
	return map;
}

/* register ranges diffed word by word while MDSS is powered */
struct k61_range {
	const char *name;
	phys_addr_t pa;
	u32 len;
	void __iomem *va;
	u32 *last;
	u8 *changes;
	bool have;
};

static struct k61_range ranges[] = {
	{ "dsi0_ctrl",	0x0ae94000, 0x400 },
	{ "dsi0_phypll", 0x0ae94400, 0x588 },
	{ "dcc_rcg",	0x0af02060, 0x130 },
};

static void k61_range_scan(struct k61_range *r)
{
	u32 n = r->len / 4, i;

	if (!r->va)
		return;
	for (i = 0; i < n; i++) {
		u32 v = readl_relaxed(r->va + 4 * i);

		if (r->have && v != r->last[i] && r->changes[i] < 4) {
			r->changes[i]++;
			pr_info("k61sw: %s+%03x %08x -> %08x%s\n", r->name, 4 * i,
				r->last[i], v, r->changes[i] == 4 ? " (muted)" : "");
		}
		r->last[i] = v;
	}
	r->have = true;
}

#define SMMU_GR0	0x15000000
#define NUM_SME		68

static int k61_splashwatch_fn(void *data)
{
	void __iomem *gr0 = ioremap(SMMU_GR0, 0x1000);
	void __iomem *fcnt = ioremap(0x0ae6b8ac, 4);
	u32 smr[NUM_SME], s2cr[NUM_SME];
	bool sme_have = false, fc_have = false, fc_moving = false;
	u32 fc_last = 0;
	/* splash framebuffer: 1080x2340 XRGB, stride 4320, at 0x9c000000 */
	void *fb = memremap(0x9c000000, 4320 * 2340, MEMREMAP_WB);
	u32 fb_crc = 0, fb_nz = 0;
	struct regmap *pmic = NULL;
	unsigned int pmic_last[ARRAY_SIZE(pmic_regs)];
	bool pmic_have[ARRAY_SIZE(pmic_regs)] = { false };
	bool fb_have = false;
	u64 end = local_clock() + 25ULL * NSEC_PER_SEC;
	int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		regs[i].va = ioremap(regs[i].pa, 4);
	for (i = 0; i < ARRAY_SIZE(ranges); i++) {
		ranges[i].va = ioremap(ranges[i].pa, ranges[i].len);
		ranges[i].last = kcalloc(ranges[i].len / 4, sizeof(u32), GFP_KERNEL);
		ranges[i].changes = kcalloc(ranges[i].len / 4, 1, GFP_KERNEL);
		if (!ranges[i].last || !ranges[i].changes)
			ranges[i].va = NULL;
	}

	pr_info("k61sw: start\n");
	while (local_clock() < end) {
		bool mdss_on = (readl_relaxed(regs[2].va) & GDSC_PWR_ON) &&
			       !(readl_relaxed(regs[3].va) & CBCR_OFF);

		for (i = 0; i < ARRAY_SIZE(regs); i++) {
			struct k61_reg *r = &regs[i];
			u32 v;

			if (!r->va || (r->gated && !mdss_on))
				continue;
			v = readl_relaxed(r->va);
			if (!r->have || v != r->last)
				pr_info("k61sw: %s %08x -> %08x\n", r->name,
					r->have ? r->last : 0, v);
			r->last = v;
			r->have = true;
		}

		if (mdss_on)
			for (i = 0; i < ARRAY_SIZE(ranges); i++)
				k61_range_scan(&ranges[i]);
		else
			for (i = 0; i < ARRAY_SIZE(ranges); i++)
				ranges[i].have = false;

		if (mdss_on && fcnt) {
			u32 v = readl_relaxed(fcnt);
			bool moving = fc_have && v != fc_last;

			if (fc_have && moving != fc_moving)
				pr_info("k61sw: intf1 frame count %s (%u)\n",
					moving ? "running" : "stopped", v);
			fc_moving = moving;
			fc_last = v;
			fc_have = true;
		} else if (fc_have) {
			pr_info("k61sw: MDSS off, frame count not readable\n");
			fc_have = false;
		}

		if (!pmic) {
			pmic = k61_pmic_regmap();
			if (pmic)
				pr_info("k61sw: pmic regmap found\n");
		}
		if (pmic) {
			for (i = 0; i < ARRAY_SIZE(pmic_regs); i++) {
				unsigned int v;

				if (regmap_read(pmic, pmic_regs[i].reg, &v))
					continue;
				if (!pmic_have[i] || v != pmic_last[i])
					pr_info("k61sw: %s %02x -> %02x\n",
						pmic_regs[i].name,
						pmic_have[i] ? pmic_last[i] : 0, v);
				pmic_last[i] = v;
				pmic_have[i] = true;
			}
		}

		if (fb) {
			u32 crc = 0, nz = 0, row;

			/* sample one 4 KiB slice every 64 rows */
			for (row = 0; row < 2340; row += 64) {
				const u32 *p = fb + row * 4320;
				int k;

				for (k = 0; k < 1024; k++)
					if (p[k] & 0xffffff)
						nz++;
				crc = crc32_le(crc, (const u8 *)p, 4096);
			}
			if (!fb_have || crc != fb_crc)
				pr_info("k61sw: fb crc %08x -> %08x, nonblack %u -> %u of %u\n",
					fb_crc, crc, fb_nz, nz, 37 * 1024);
			fb_crc = crc;
			fb_nz = nz;
			fb_have = true;
		}

		if (gr0) {
			for (i = 0; i < NUM_SME; i++) {
				u32 a = readl_relaxed(gr0 + 0x800 + 4 * i);
				u32 b = readl_relaxed(gr0 + 0xc00 + 4 * i);

				if (sme_have && (a != smr[i] || b != s2cr[i]))
					pr_info("k61sw: SME%d smr %08x->%08x s2cr %08x->%08x\n",
						i, smr[i], a, s2cr[i], b);
				else if (!sme_have && (a & BIT(31)))
					pr_info("k61sw: SME%d smr %08x s2cr %08x\n", i, a, b);
				smr[i] = a;
				s2cr[i] = b;
			}
			sme_have = true;
		}
		usleep_range(10000, 11000);
	}
	pr_info("k61sw: done\n");
	return 0;
}

static int __init k61_splashwatch_init(void)
{
	if (splashwatch)
		kthread_run(k61_splashwatch_fn, NULL, "k61splashwatch");
	return 0;
}
arch_initcall(k61_splashwatch_init);
