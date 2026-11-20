// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 *
 * 10nm DSI PLL (DSI PHY v3.0, sdmmagpie) for the display-drivers dsi_pll
 * framework. The register values and the enable/disable sequence come from
 * the 4.14 drivers/clk/qcom/mdss/mdss-dsi-pll-10nm.c, which drove it as a
 * separate clock controller. The 4.14 clock tree
 *
 *   vco -> pll_out_div (1/2/4/8) -> bitclk_src (CFG0[3:0]) -> byte /8
 *                                -> pclk mux (CFG1[1:0]) -> pclk_src (CFG0[7:4])
 *
 * is folded into the two outputs that 6.1 registers, and the dividers are
 * chosen in configure() from the byte clock rate, bpp and lane count, as the
 * 6.1 5nm PLL (same mux design) does. With DSC, dsi_phy passes the
 * destination format bpp, like on the 5nm targets.
 *
 * Single DSI only: no master/slave split, no dynamic refresh, D-PHY only.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include "dsi_pll.h"

#define DSI_PLL_10NM_MAX		2

#define VCO_REF_CLK_RATE		19200000UL
#define VCO_MIN_RATE			1000000000UL
#define VCO_MAX_RATE			3500000000UL

#define MHZ_1100			1100000000UL

/* PLL registers (pll_base) */
#define PLL_ANALOG_CONTROLS_ONE			0x000
#define PLL_ANALOG_CONTROLS_TWO			0x004
#define PLL_INT_LOOP_SETTINGS			0x008
#define PLL_INT_LOOP_SETTINGS_TWO		0x00c
#define PLL_ANALOG_CONTROLS_THREE		0x010
#define PLL_ANALOG_CONTROLS_FOUR		0x014
#define PLL_INT_LOOP_CONTROLS			0x018
#define PLL_DSM_DIVIDER				0x01c
#define PLL_FEEDBACK_DIVIDER			0x020
#define PLL_SYSTEM_MUXES			0x024
#define PLL_FREQ_UPDATE_CONTROL_OVERRIDES	0x028
#define PLL_CMODE				0x02c
#define PLL_CALIBRATION_SETTINGS		0x030
#define PLL_BAND_SEL_CAL_TIMER_LOW		0x034
#define PLL_BAND_SEL_CAL_TIMER_HIGH		0x038
#define PLL_BAND_SEL_CAL_SETTINGS		0x03c
#define PLL_BAND_SEL_MIN			0x040
#define PLL_BAND_SEL_MAX			0x044
#define PLL_BAND_SEL_PFILT			0x048
#define PLL_BAND_SEL_IFILT			0x04c
#define PLL_BAND_SEL_CAL_SETTINGS_TWO		0x050
#define PLL_BAND_SEL_CAL_SETTINGS_THREE		0x054
#define PLL_BAND_SEL_CAL_SETTINGS_FOUR		0x058
#define PLL_BAND_SEL_ICODE_HIGH			0x05c
#define PLL_BAND_SEL_ICODE_LOW			0x060
#define PLL_FREQ_DETECT_SETTINGS_ONE		0x064
#define PLL_PFILT				0x07c
#define PLL_IFILT				0x080
#define PLL_GAIN				0x084
#define PLL_ICODE_LOW				0x088
#define PLL_ICODE_HIGH				0x08c
#define PLL_LOCKDET				0x090
#define PLL_OUTDIV				0x094
#define PLL_FASTLOCK_CONTROL			0x098
#define PLL_PASS_OUT_OVERRIDE_ONE		0x09c
#define PLL_PASS_OUT_OVERRIDE_TWO		0x0a0
#define PLL_CORE_OVERRIDE			0x0a4
#define PLL_CORE_INPUT_OVERRIDE			0x0a8
#define PLL_RATE_CHANGE				0x0ac
#define PLL_PLL_DIGITAL_TIMERS			0x0b0
#define PLL_PLL_DIGITAL_TIMERS_TWO		0x0b4
#define PLL_DEC_FRAC_MUXES			0x0c8
#define PLL_DECIMAL_DIV_START_1			0x0cc
#define PLL_FRAC_DIV_START_LOW_1		0x0d0
#define PLL_FRAC_DIV_START_MID_1		0x0d4
#define PLL_FRAC_DIV_START_HIGH_1		0x0d8
#define PLL_MASH_CONTROL			0x0ec
#define PLL_SSC_MUX_CONTROL			0x108
#define PLL_SSC_STEPSIZE_LOW_1			0x10c
#define PLL_SSC_STEPSIZE_HIGH_1			0x110
#define PLL_SSC_DIV_PER_LOW_1			0x114
#define PLL_SSC_DIV_PER_HIGH_1			0x118
#define PLL_SSC_DIV_ADJPER_LOW_1		0x11c
#define PLL_SSC_DIV_ADJPER_HIGH_1		0x120
#define PLL_SSC_CONTROL				0x13c
#define PLL_PLL_OUTDIV_RATE			0x140
#define PLL_PLL_LOCKDET_RATE_1			0x144
#define PLL_PLL_PROP_GAIN_RATE_1		0x14c
#define PLL_PLL_BAND_SET_RATE_1			0x154
#define PLL_PLL_INT_GAIN_IFILT_BAND_1		0x15c
#define PLL_PLL_FL_INT_GAIN_PFILT_BAND_1	0x164
#define PLL_FASTLOCK_EN_BAND			0x16c
#define PLL_FREQ_TUNE_ACCUM_INIT_MUX		0x17c
#define PLL_PLL_LOCK_OVERRIDE			0x180
#define PLL_PLL_LOCK_DELAY			0x184
#define PLL_PLL_LOCK_MIN_DELAY			0x188
#define PLL_CLOCK_INVERTERS			0x18c
#define PLL_SPARE_AND_JPC_OVERRIDES		0x190
#define PLL_BIAS_CONTROL_1			0x194
#define PLL_BIAS_CONTROL_2			0x198
#define PLL_ALOG_OBSV_BUS_CTRL_1		0x19c
#define PLL_COMMON_STATUS_ONE			0x1a0

/* PHY common registers (phy_base) */
#define PHY_CMN_CLK_CFG0	0x010
#define PHY_CMN_CLK_CFG1	0x014
#define PHY_CMN_GLBL_CTRL	0x018
#define PHY_CMN_RBUF_CTRL	0x01c
#define PHY_CMN_CTRL_0		0x024
#define PHY_CMN_PLL_CNTRL	0x038

#define SSC_CENTER		BIT(0)
#define SSC_EN			BIT(1)

struct dsi_pll_10nm_regs {
	u32 decimal_div_start;
	u32 frac_div_start_low;
	u32 frac_div_start_mid;
	u32 frac_div_start_high;
	u32 pll_clock_inverters;
	u32 ssc_stepsize_low;
	u32 ssc_stepsize_high;
	u32 ssc_div_per_low;
	u32 ssc_div_per_high;
	u32 ssc_adjper_low;
	u32 ssc_adjper_high;
	u32 ssc_control;
};

struct dsi_pll_10nm_config {
	u32 ref_freq;
	u32 frac_bits;
	bool enable_ssc;
	bool ssc_center;
	u32 ssc_freq;
	u32 ssc_offset;
	u32 ssc_adj_per;
};

struct dsi_pll_10nm {
	struct dsi_pll_resource *rsc;
	struct dsi_pll_10nm_config cfg;
	struct dsi_pll_10nm_regs regs;
};

static struct dsi_pll_10nm plls[DSI_PLL_10NM_MAX];

/* ---- dividers ---------------------------------------------------------- */

static void dsi_pll_10nm_set_pll_post_div(struct dsi_pll_resource *rsc,
		u32 div)
{
	/* PLL_PLL_OUTDIV_RATE holds log2 of the divider (1, 2, 4, 8) */
	DSI_PLL_REG_W(rsc->pll_base, PLL_PLL_OUTDIV_RATE, ilog2(div));
}

static u32 dsi_pll_10nm_get_pll_post_div(struct dsi_pll_resource *rsc)
{
	return 1 << (DSI_PLL_REG_R(rsc->pll_base, PLL_PLL_OUTDIV_RATE) & 0x3);
}

static void dsi_pll_10nm_update_cfg(struct dsi_pll_resource *rsc, u32 reg,
		u32 mask, u32 val)
{
	u32 data = DSI_PLL_REG_R(rsc->phy_base, reg);

	data &= ~mask;
	data |= val & mask;
	DSI_PLL_REG_W(rsc->phy_base, reg, data);
}

/*
 * vco = bit clock * pll_post_div * phy_post_div, kept in 1.0 - 3.5 GHz:
 * the smallest dividers that reach 1 GHz, the PHY divider first (what the
 * 4.14 clock framework picked through bitclk_src -> pll_out_div).
 */
static int dsi_pll_10nm_set_byteclk_div(struct dsi_pll_resource *rsc,
		bool commit)
{
	u64 bitclk = rsc->byteclk_rate * 8;
	u32 pll_div, phy_div;
	u64 vco;

	if (!bitclk) {
		DSI_PLL_ERR(rsc, "no byte clock rate\n");
		return -EINVAL;
	}

	for (pll_div = 1; pll_div <= 8; pll_div <<= 1) {
		for (phy_div = 1; phy_div <= 15; phy_div++) {
			vco = bitclk * pll_div * phy_div;
			if (vco >= VCO_MIN_RATE)
				goto found;
		}
	}
	DSI_PLL_ERR(rsc, "bit clock %llu too low for the VCO\n", bitclk);
	return -EINVAL;

found:
	if (vco > VCO_MAX_RATE) {
		DSI_PLL_ERR(rsc, "bit clock %llu needs vco %llu, out of range\n",
				bitclk, vco);
		return -EINVAL;
	}

	if (commit) {
		dsi_pll_10nm_set_pll_post_div(rsc, pll_div);
		dsi_pll_10nm_update_cfg(rsc, PHY_CMN_CLK_CFG0, 0x0f, phy_div);
	}
	rsc->vco_rate = vco;

	DSI_PLL_DBG(rsc, "bit clk %llu: pll_post_div %u, phy_post_div %u, vco %llu\n",
			bitclk, pll_div, phy_div, vco);
	return 0;
}

/* the 6.1 5nm D-PHY pixel divider: bpp * m / n / lanes, halved for sel 1 */
static u32 dsi_pll_10nm_calc_pclk_div(struct dsi_pll_resource *rsc,
		u32 dsiclk_sel)
{
	u32 m_val, n_val, pclk_div;

	if (rsc->bpp == 30 && rsc->lanes == 4) {
		m_val = 2;	/* RGB101010 */
		n_val = 3;
	} else if (rsc->bpp == 18 && rsc->lanes == 2) {
		m_val = 2;	/* RGB666 packed */
		n_val = 9;
	} else if (rsc->bpp == 18 && rsc->lanes == 4) {
		m_val = 4;
		n_val = 9;
	} else if (rsc->bpp == 16 && rsc->lanes == 3) {
		m_val = 3;	/* RGB565 */
		n_val = 8;
	} else {
		m_val = 1;
		n_val = 1;
	}

	pclk_div = mult_frac(rsc->bpp, m_val, n_val);
	if (dsiclk_sel == 1)
		pclk_div /= 2;
	pclk_div /= rsc->lanes;

	return clamp_t(u32, pclk_div, 1, 15);
}

static int dsi_pll_10nm_set_pclk_div(struct dsi_pll_resource *rsc,
		bool commit)
{
	u32 pll_div = dsi_pll_10nm_get_pll_post_div(rsc);
	u32 phy_div = DSI_PLL_REG_R(rsc->phy_base, PHY_CMN_CLK_CFG0) & 0xf;
	u32 dsiclk_sel, pclk_div;
	u64 src;

	if (!rsc->lanes || !rsc->bpp || !phy_div) {
		DSI_PLL_ERR(rsc, "bpp %d lanes %d phy_div %u\n",
				rsc->bpp, rsc->lanes, phy_div);
		return -EINVAL;
	}

	/* 0: bit clock, 1: bit clock / 2 (5nm D-PHY choice) */
	if ((rsc->bpp == 30 && (rsc->lanes == 2 || rsc->lanes == 4)) ||
			(rsc->bpp == 3 && rsc->lanes >= 3))
		dsiclk_sel = 0;
	else
		dsiclk_sel = 1;

	src = div_u64(rsc->vco_rate, pll_div * phy_div);
	if (dsiclk_sel == 1)
		src = div_u64(src, 2);

	pclk_div = dsi_pll_10nm_calc_pclk_div(rsc, dsiclk_sel);
	rsc->pclk_rate = div_u64(src, pclk_div);

	if (commit) {
		dsi_pll_10nm_update_cfg(rsc, PHY_CMN_CLK_CFG1, 0x03, dsiclk_sel);
		dsi_pll_10nm_update_cfg(rsc, PHY_CMN_CLK_CFG0, 0xf0,
				pclk_div << 4);
	}

	DSI_PLL_DBG(rsc, "pclk %llu: dsiclk_sel %u, pclk_div %u\n",
			rsc->pclk_rate, dsiclk_sel, pclk_div);
	return 0;
}

/* ---- VCO (4.14 mdss-dsi-pll-10nm.c) ------------------------------------- */

static void dsi_pll_10nm_setup_config(struct dsi_pll_10nm *pll)
{
	struct dsi_pll_resource *rsc = pll->rsc;
	struct dsi_pll_10nm_config *cfg = &pll->cfg;

	cfg->ref_freq = VCO_REF_CLK_RATE;
	cfg->frac_bits = 18;
	cfg->ssc_freq = 31500;
	cfg->ssc_offset = 5000;
	cfg->ssc_adj_per = 2;
	cfg->enable_ssc = rsc->ssc_en;
	cfg->ssc_center = rsc->ssc_center;
	if (cfg->enable_ssc) {
		if (rsc->ssc_freq)
			cfg->ssc_freq = rsc->ssc_freq;
		if (rsc->ssc_ppm)
			cfg->ssc_offset = rsc->ssc_ppm;
	}
}

static void dsi_pll_10nm_calc_dec_frac(struct dsi_pll_10nm *pll)
{
	struct dsi_pll_resource *rsc = pll->rsc;
	struct dsi_pll_10nm_regs *regs = &pll->regs;
	u64 pll_freq = rsc->vco_current_rate;
	u64 multiplier = 1 << pll->cfg.frac_bits;
	u64 dec_multiple, dec;
	u32 frac;

	/* the prescaler stays on: reference doubled */
	dec_multiple = div_u64(pll_freq * multiplier, VCO_REF_CLK_RATE * 2);
	div_u64_rem(dec_multiple, multiplier, &frac);
	dec = div_u64(dec_multiple, multiplier);

	regs->pll_clock_inverters = pll_freq < MHZ_1100 ? 8 : 0;

	regs->decimal_div_start = dec;
	regs->frac_div_start_low = frac & 0xff;
	regs->frac_div_start_mid = (frac & 0xff00) >> 8;
	regs->frac_div_start_high = (frac & 0x30000) >> 16;
}

static void dsi_pll_10nm_calc_ssc(struct dsi_pll_10nm *pll)
{
	struct dsi_pll_10nm_config *cfg = &pll->cfg;
	struct dsi_pll_10nm_regs *regs = &pll->regs;
	u32 ssc_per, ssc_mod;
	u64 step, frac;

	if (!cfg->enable_ssc)
		return;

	ssc_per = DIV_ROUND_CLOSEST(cfg->ref_freq, cfg->ssc_freq) / 2 - 1;
	ssc_mod = (ssc_per + 1) % (cfg->ssc_adj_per + 1);
	ssc_per -= ssc_mod;

	frac = regs->frac_div_start_low | (regs->frac_div_start_mid << 8) |
		(regs->frac_div_start_high << 16);
	step = regs->decimal_div_start;
	step *= 1 << cfg->frac_bits;
	step += frac;
	step *= cfg->ssc_offset;
	step *= cfg->ssc_adj_per + 1;
	step = div_u64(step, ssc_per + 1);
	step = DIV_ROUND_CLOSEST_ULL(step, 1000000);

	regs->ssc_div_per_low = ssc_per & 0xff;
	regs->ssc_div_per_high = (ssc_per & 0xff00) >> 8;
	regs->ssc_stepsize_low = (u32)(step & 0xff);
	regs->ssc_stepsize_high = (u32)((step & 0xff00) >> 8);
	regs->ssc_adjper_low = cfg->ssc_adj_per & 0xff;
	regs->ssc_adjper_high = (cfg->ssc_adj_per & 0xff00) >> 8;
	regs->ssc_control = cfg->ssc_center ? SSC_CENTER : 0;
}

static void dsi_pll_10nm_init_val(struct dsi_pll_resource *rsc)
{
	void __iomem *base = rsc->pll_base;

	DSI_PLL_REG_W(base, PLL_CORE_INPUT_OVERRIDE, 0x10);
	DSI_PLL_REG_W(base, PLL_INT_LOOP_SETTINGS, 0x3f);
	DSI_PLL_REG_W(base, PLL_INT_LOOP_SETTINGS_TWO, 0x0);
	DSI_PLL_REG_W(base, PLL_ANALOG_CONTROLS_FOUR, 0x0);
	DSI_PLL_REG_W(base, PLL_INT_LOOP_CONTROLS, 0x80);
	DSI_PLL_REG_W(base, PLL_FREQ_UPDATE_CONTROL_OVERRIDES, 0x0);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_CAL_TIMER_LOW, 0x0);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_CAL_TIMER_HIGH, 0x02);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_CAL_SETTINGS, 0x82);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_MIN, 0x00);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_MAX, 0xff);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_PFILT, 0x00);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_IFILT, 0x00);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_CAL_SETTINGS_TWO, 0x25);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_CAL_SETTINGS_FOUR, 0x4f);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_ICODE_HIGH, 0x0a);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_ICODE_LOW, 0x0);
	DSI_PLL_REG_W(base, PLL_GAIN, 0x42);
	DSI_PLL_REG_W(base, PLL_ICODE_LOW, 0x00);
	DSI_PLL_REG_W(base, PLL_ICODE_HIGH, 0x00);
	DSI_PLL_REG_W(base, PLL_LOCKDET, 0x30);
	DSI_PLL_REG_W(base, PLL_FASTLOCK_CONTROL, 0x04);
	DSI_PLL_REG_W(base, PLL_PASS_OUT_OVERRIDE_ONE, 0x00);
	DSI_PLL_REG_W(base, PLL_PASS_OUT_OVERRIDE_TWO, 0x00);
	DSI_PLL_REG_W(base, PLL_RATE_CHANGE, 0x01);
	DSI_PLL_REG_W(base, PLL_PLL_DIGITAL_TIMERS, 0x08);
	DSI_PLL_REG_W(base, PLL_DEC_FRAC_MUXES, 0x00);
	DSI_PLL_REG_W(base, PLL_MASH_CONTROL, 0x03);
	DSI_PLL_REG_W(base, PLL_SSC_MUX_CONTROL, 0x0);
	DSI_PLL_REG_W(base, PLL_SSC_CONTROL, 0x0);
	DSI_PLL_REG_W(base, PLL_FASTLOCK_EN_BAND, 0x03);
	DSI_PLL_REG_W(base, PLL_FREQ_TUNE_ACCUM_INIT_MUX, 0x0);
	DSI_PLL_REG_W(base, PLL_PLL_LOCK_MIN_DELAY, 0x19);
	DSI_PLL_REG_W(base, PLL_SPARE_AND_JPC_OVERRIDES, 0x0);
	DSI_PLL_REG_W(base, PLL_BIAS_CONTROL_1, 0x40);
	DSI_PLL_REG_W(base, PLL_BIAS_CONTROL_2, 0x20);
	DSI_PLL_REG_W(base, PLL_ALOG_OBSV_BUS_CTRL_1, 0x0);
}

static void dsi_pll_10nm_commit(struct dsi_pll_10nm *pll)
{
	void __iomem *base = pll->rsc->pll_base;
	struct dsi_pll_10nm_regs *regs = &pll->regs;

	DSI_PLL_REG_W(base, PLL_CORE_INPUT_OVERRIDE, 0x12);
	DSI_PLL_REG_W(base, PLL_DECIMAL_DIV_START_1, regs->decimal_div_start);
	DSI_PLL_REG_W(base, PLL_FRAC_DIV_START_LOW_1, regs->frac_div_start_low);
	DSI_PLL_REG_W(base, PLL_FRAC_DIV_START_MID_1, regs->frac_div_start_mid);
	DSI_PLL_REG_W(base, PLL_FRAC_DIV_START_HIGH_1,
			regs->frac_div_start_high);
	DSI_PLL_REG_W(base, PLL_PLL_LOCKDET_RATE_1, 0x40);
	DSI_PLL_REG_W(base, PLL_PLL_LOCK_DELAY, 0x06);
	DSI_PLL_REG_W(base, PLL_CMODE, 0x10);	/* D-PHY */
	DSI_PLL_REG_W(base, PLL_CLOCK_INVERTERS, regs->pll_clock_inverters);
}

static void dsi_pll_10nm_config_hzindep_reg(struct dsi_pll_10nm *pll)
{
	void __iomem *base = pll->rsc->pll_base;

	DSI_PLL_REG_W(base, PLL_ANALOG_CONTROLS_ONE, 0x80);
	DSI_PLL_REG_W(base, PLL_ANALOG_CONTROLS_TWO, 0x03);
	DSI_PLL_REG_W(base, PLL_ANALOG_CONTROLS_THREE, 0x00);
	DSI_PLL_REG_W(base, PLL_DSM_DIVIDER, 0x00);
	DSI_PLL_REG_W(base, PLL_FEEDBACK_DIVIDER, 0x4e);
	DSI_PLL_REG_W(base, PLL_CALIBRATION_SETTINGS, 0x40);
	DSI_PLL_REG_W(base, PLL_BAND_SEL_CAL_SETTINGS_THREE, 0xba);
	DSI_PLL_REG_W(base, PLL_FREQ_DETECT_SETTINGS_ONE, 0x0c);
	DSI_PLL_REG_W(base, PLL_OUTDIV, 0x00);
	DSI_PLL_REG_W(base, PLL_CORE_OVERRIDE, 0x00);
	DSI_PLL_REG_W(base, PLL_PLL_DIGITAL_TIMERS_TWO, 0x08);
	/* 4.14 computes a rate-dependent gain but programs 0x08 */
	DSI_PLL_REG_W(base, PLL_PLL_PROP_GAIN_RATE_1, 0x08);
	DSI_PLL_REG_W(base, PLL_PLL_BAND_SET_RATE_1, 0xc0);
	DSI_PLL_REG_W(base, PLL_PLL_INT_GAIN_IFILT_BAND_1, 0xfa);
	DSI_PLL_REG_W(base, PLL_PLL_FL_INT_GAIN_PFILT_BAND_1, 0x4c);
	DSI_PLL_REG_W(base, PLL_PLL_LOCK_OVERRIDE, 0x80);
	DSI_PLL_REG_W(base, PLL_PFILT, 0x29);
	DSI_PLL_REG_W(base, PLL_IFILT, 0x3f);
}

static void dsi_pll_10nm_ssc_commit(struct dsi_pll_10nm *pll)
{
	void __iomem *base = pll->rsc->pll_base;
	struct dsi_pll_10nm_regs *regs = &pll->regs;

	if (!pll->cfg.enable_ssc)
		return;

	DSI_PLL_REG_W(base, PLL_SSC_STEPSIZE_LOW_1, regs->ssc_stepsize_low);
	DSI_PLL_REG_W(base, PLL_SSC_STEPSIZE_HIGH_1, regs->ssc_stepsize_high);
	DSI_PLL_REG_W(base, PLL_SSC_DIV_PER_LOW_1, regs->ssc_div_per_low);
	DSI_PLL_REG_W(base, PLL_SSC_DIV_PER_HIGH_1, regs->ssc_div_per_high);
	DSI_PLL_REG_W(base, PLL_SSC_DIV_ADJPER_LOW_1, regs->ssc_adjper_low);
	DSI_PLL_REG_W(base, PLL_SSC_DIV_ADJPER_HIGH_1, regs->ssc_adjper_high);
	DSI_PLL_REG_W(base, PLL_SSC_CONTROL, SSC_EN | regs->ssc_control);
}

static void dsi_pll_10nm_enable_pll_bias(struct dsi_pll_resource *rsc)
{
	u32 data = DSI_PLL_REG_R(rsc->phy_base, PHY_CMN_CTRL_0);

	DSI_PLL_REG_W(rsc->phy_base, PHY_CMN_CTRL_0, data | BIT(5));
	DSI_PLL_REG_W(rsc->pll_base, PLL_SYSTEM_MUXES, 0xc0);
	ndelay(250);
}

static void dsi_pll_10nm_disable_pll_bias(struct dsi_pll_resource *rsc)
{
	u32 data = DSI_PLL_REG_R(rsc->phy_base, PHY_CMN_CTRL_0);

	DSI_PLL_REG_W(rsc->pll_base, PLL_SYSTEM_MUXES, 0);
	DSI_PLL_REG_W(rsc->phy_base, PHY_CMN_CTRL_0, data & ~BIT(5));
	ndelay(250);
}

static int dsi_pll_10nm_lock_status(struct dsi_pll_resource *rsc)
{
	u32 status;
	int rc;

	rc = readl_poll_timeout_atomic(rsc->pll_base + PLL_COMMON_STATUS_ONE,
			status, (status & BIT(0)), 100, 5000);
	if (rc)
		DSI_PLL_ERR(rsc, "lock failed, status=0x%08x\n", status);

	return rc;
}

int dsi_pll_10nm_configure(void *pll_rsc, bool commit)
{
	struct dsi_pll_resource *rsc = pll_rsc;
	struct dsi_pll_10nm *pll;
	int rc;

	if (!rsc || !rsc->priv)
		return -EINVAL;
	pll = rsc->priv;

	if (!commit) {
		DSI_PLL_WARN(rsc, "dynamic clock switch is not supported\n");
		return -EOPNOTSUPP;
	}

	if (rsc->type != DSI_PHY_TYPE_DPHY) {
		DSI_PLL_ERR(rsc, "only D-PHY is supported\n");
		return -EINVAL;
	}

	/* PLL power before touching the PLL registers */
	dsi_pll_10nm_enable_pll_bias(rsc);
	dsi_pll_10nm_init_val(rsc);

	rc = dsi_pll_10nm_set_byteclk_div(rsc, true);
	if (rc)
		return rc;
	rc = dsi_pll_10nm_set_pclk_div(rsc, true);
	if (rc)
		return rc;

	rsc->vco_current_rate = rsc->vco_rate;
	rsc->vco_ref_clk_rate = VCO_REF_CLK_RATE;

	/* the 4.14 vco_10nm_set_rate() order */
	dsi_pll_10nm_setup_config(pll);
	dsi_pll_10nm_calc_dec_frac(pll);
	dsi_pll_10nm_calc_ssc(pll);
	dsi_pll_10nm_commit(pll);
	dsi_pll_10nm_config_hzindep_reg(pll);
	dsi_pll_10nm_ssc_commit(pll);
	wmb();	/* make sure the PLL is configured */

	return 0;
}

int dsi_pll_10nm_toggle(void *pll_rsc, bool prepare)
{
	struct dsi_pll_resource *rsc = pll_rsc;
	u32 data;
	int rc;

	if (!rsc)
		return -EINVAL;

	if (!prepare) {
		/* gate the output before stopping the PLL (4.14) */
		data = DSI_PLL_REG_R(rsc->phy_base, PHY_CMN_CLK_CFG1);
		DSI_PLL_REG_W(rsc->phy_base, PHY_CMN_CLK_CFG1, data & ~BIT(5));
		DSI_PLL_REG_W(rsc->phy_base, PHY_CMN_PLL_CNTRL, 0);
		DSI_PLL_REG_W(rsc->phy_base, PHY_CMN_RBUF_CTRL, 0);
		dsi_pll_10nm_disable_pll_bias(rsc);
		wmb();	/* make sure the PLL is stopped */
		return 0;
	}

	DSI_PLL_REG_W(rsc->phy_base, PHY_CMN_PLL_CNTRL, 0x01);
	wmb();	/* start the PLL before polling for lock */

	rc = dsi_pll_10nm_lock_status(rsc);
	if (rc)
		return rc;

	data = DSI_PLL_REG_R(rsc->phy_base, PHY_CMN_CLK_CFG1);
	DSI_PLL_REG_W(rsc->phy_base, PHY_CMN_CLK_CFG1, data | BIT(5));
	DSI_PLL_REG_W(rsc->phy_base, PHY_CMN_RBUF_CTRL, 0x01);
	wmb();	/* make sure the output is enabled */

	return 0;
}

/* ---- clocks ------------------------------------------------------------ */

static u64 dsi_pll_10nm_vco_recalc(struct dsi_pll_resource *rsc)
{
	u64 ref = VCO_REF_CLK_RATE * 2, rate;
	u32 dec, frac;

	dec = DSI_PLL_REG_R(rsc->pll_base, PLL_DECIMAL_DIV_START_1) & 0xff;
	frac = DSI_PLL_REG_R(rsc->pll_base, PLL_FRAC_DIV_START_LOW_1) & 0xff;
	frac |= (DSI_PLL_REG_R(rsc->pll_base, PLL_FRAC_DIV_START_MID_1) & 0xff)
			<< 8;
	frac |= (DSI_PLL_REG_R(rsc->pll_base, PLL_FRAC_DIV_START_HIGH_1) & 0x3)
			<< 16;

	rate = dec * ref + div_u64(ref * frac, 1 << 18);
	return div_u64(rate, dsi_pll_10nm_get_pll_post_div(rsc));
}

static unsigned long dsi_pll_10nm_byteclk_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct dsi_pll_resource *rsc = to_pll_clk_hw(hw)->priv;
	u32 phy_div;

	if (rsc->byteclk_rate)
		return rsc->byteclk_rate;

	/* the bootloader's setup, at registration */
	phy_div = DSI_PLL_REG_R(rsc->phy_base, PHY_CMN_CLK_CFG0) & 0xf;
	if (!phy_div)
		return 0;
	return div_u64(dsi_pll_10nm_vco_recalc(rsc), phy_div * 8);
}

static unsigned long dsi_pll_10nm_pclk_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct dsi_pll_resource *rsc = to_pll_clk_hw(hw)->priv;
	u32 cfg0, phy_div, pclk_div, sel;
	u64 rate;

	if (rsc->pclk_rate)
		return rsc->pclk_rate;

	cfg0 = DSI_PLL_REG_R(rsc->phy_base, PHY_CMN_CLK_CFG0);
	sel = DSI_PLL_REG_R(rsc->phy_base, PHY_CMN_CLK_CFG1) & 0x3;
	phy_div = cfg0 & 0xf;
	pclk_div = (cfg0 >> 4) & 0xf;
	if (!phy_div || !pclk_div)
		return 0;

	rate = div_u64(dsi_pll_10nm_vco_recalc(rsc), phy_div);
	if (sel == 1)
		rate = div_u64(rate, 2);
	return div_u64(rate, pclk_div);
}

static long dsi_pll_10nm_byteclk_round_rate(struct clk_hw *hw,
		unsigned long rate, unsigned long *parent_rate)
{
	struct dsi_pll_resource *rsc = to_pll_clk_hw(hw)->priv;

	return rsc->byteclk_rate;
}

static long dsi_pll_10nm_pclk_round_rate(struct clk_hw *hw,
		unsigned long rate, unsigned long *parent_rate)
{
	struct dsi_pll_resource *rsc = to_pll_clk_hw(hw)->priv;

	return rsc->pclk_rate;
}

static int dsi_pll_10nm_set_rate_stub(struct clk_hw *hw, unsigned long rate,
		unsigned long parent_rate)
{
	return 0;
}

static int dsi_pll_10nm_prepare_stub(struct clk_hw *hw)
{
	return 0;
}

static void dsi_pll_10nm_unprepare_stub(struct clk_hw *hw)
{
}

static const struct clk_ops dsi_pll_10nm_byteclk_ops = {
	.recalc_rate = dsi_pll_10nm_byteclk_recalc_rate,
	.set_rate = dsi_pll_10nm_set_rate_stub,
	.round_rate = dsi_pll_10nm_byteclk_round_rate,
	.prepare = dsi_pll_10nm_prepare_stub,
	.unprepare = dsi_pll_10nm_unprepare_stub,
};

static const struct clk_ops dsi_pll_10nm_pclk_ops = {
	.recalc_rate = dsi_pll_10nm_pclk_recalc_rate,
	.set_rate = dsi_pll_10nm_set_rate_stub,
	.round_rate = dsi_pll_10nm_pclk_round_rate,
	.prepare = dsi_pll_10nm_prepare_stub,
	.unprepare = dsi_pll_10nm_unprepare_stub,
};

/* Same output names as the 5nm PLL: dispcc-sdmmagpie looks them up by name. */
static struct dsi_pll_clk dsi_pll_10nm_clks[DSI_PLL_10NM_MAX][2] = {
	{
		{ .hw.init = &(struct clk_init_data){
			.name = "dsi0_phy_pll_out_byteclk",
			.ops = &dsi_pll_10nm_byteclk_ops, }, },
		{ .hw.init = &(struct clk_init_data){
			.name = "dsi0_phy_pll_out_dsiclk",
			.ops = &dsi_pll_10nm_pclk_ops, }, },
	},
	{
		{ .hw.init = &(struct clk_init_data){
			.name = "dsi1_phy_pll_out_byteclk",
			.ops = &dsi_pll_10nm_byteclk_ops, }, },
		{ .hw.init = &(struct clk_init_data){
			.name = "dsi1_phy_pll_out_dsiclk",
			.ops = &dsi_pll_10nm_pclk_ops, }, },
	},
};

int dsi_pll_clock_register_10nm(struct platform_device *pdev,
		struct dsi_pll_resource *pll_res)
{
	struct clk_onecell_data *clk_data;
	struct clk *clk;
	int ndx, i, rc;

	if (!pdev || !pdev->dev.of_node || !pll_res || !pll_res->pll_base ||
			!pll_res->phy_base) {
		DSI_PLL_ERR(pll_res, "invalid params\n");
		return -EINVAL;
	}

	ndx = pll_res->index;
	if (ndx >= DSI_PLL_10NM_MAX) {
		DSI_PLL_ERR(pll_res, "index %d not supported\n", ndx);
		return -EINVAL;
	}

	plls[ndx].rsc = pll_res;
	pll_res->priv = &plls[ndx];
	pll_res->vco_min_rate = VCO_MIN_RATE;
	pll_res->vco_ref_clk_rate = VCO_REF_CLK_RATE;

	clk_data = devm_kzalloc(&pdev->dev, sizeof(*clk_data), GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->clks = devm_kcalloc(&pdev->dev, 2, sizeof(struct clk *),
			GFP_KERNEL);
	if (!clk_data->clks)
		return -ENOMEM;
	clk_data->clk_num = 2;

	for (i = 0; i < 2; i++) {
		dsi_pll_10nm_clks[ndx][i].priv = pll_res;
		clk = devm_clk_register(&pdev->dev,
				&dsi_pll_10nm_clks[ndx][i].hw);
		if (IS_ERR(clk)) {
			DSI_PLL_ERR(pll_res, "clock %d registration failed\n", i);
			return PTR_ERR(clk);
		}
		clk_data->clks[i] = clk;
	}

	rc = of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get,
			clk_data);
	if (!rc)
		DSI_PLL_INFO(pll_res, "registered 10nm PLL clocks\n");

	return rc;
}
