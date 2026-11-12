// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2019, The Linux Foundation. All rights reserved.
 *
 * 14nm DSI PLL (DSI PHY v2.0, sm6150) for the display-drivers dsi_pll
 * framework. The rate calculation and register programming come from the
 * 4.14 drivers/clk/qcom/mdss/mdss-dsi-pll-14nm{,-util}.c. The 4.14 clock tree
 * (vco -> post_n1_div -> byte /8, n2_div -> pixel /2) is folded into the two
 * outputs that 6.1 registers, and the rates are chosen in configure() from
 * the byte and pixel clock rates that dsi_display sets, like the 5nm PLL.
 *
 * Single DSI only: no master/slave split, no dynamic refresh.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include "dsi_pll.h"

#define DSI_PLL_14NM_MAX		2

#define VCO_REF_CLK_RATE		19200000UL
#define VCO_MIN_RATE			1300000000UL
#define VCO_MAX_RATE			2600000000UL

#define DSI_PLL_POLL_MAX_READS		15
#define DSI_PLL_POLL_TIMEOUT_US		1000

#define CEIL(x, y)			(((x) + ((y) - 1)) / (y))

/* PHY common block (offsets from the PHY/PLL base) */
#define DSIPHY_CMN_CLK_CFG0		0x0010
#define DSIPHY_CMN_CLK_CFG1		0x0014
#define DSIPHY_CMN_GLBL_TEST_CTRL	0x0018
#define DSIPHY_CMN_CTRL_0		0x001c
#define DSIPHY_CMN_CTRL_1		0x0020
#define DSIPHY_CMN_PLL_CNTRL		0x0048
#define DSIPHY_CMN_LDO_CNTRL		0x004c

/* PLL block */
#define DSIPHY_PLL_IE_TRIM		0x0400
#define DSIPHY_PLL_IP_TRIM		0x0404
#define DSIPHY_PLL_IPTAT_TRIM		0x0410
#define DSIPHY_PLL_CLKBUFLR_EN		0x041c
#define DSIPHY_PLL_SYSCLK_EN_RESET	0x0428
#define DSIPHY_PLL_RESETSM_CNTRL	0x042c
#define DSIPHY_PLL_RESETSM_CNTRL2	0x0430
#define DSIPHY_PLL_RESETSM_CNTRL3	0x0434
#define DSIPHY_PLL_RESETSM_CNTRL4	0x0438
#define DSIPHY_PLL_RESETSM_CNTRL5	0x043c
#define DSIPHY_PLL_KVCO_DIV_REF1	0x0440
#define DSIPHY_PLL_KVCO_DIV_REF2	0x0444
#define DSIPHY_PLL_KVCO_COUNT1		0x0448
#define DSIPHY_PLL_KVCO_COUNT2		0x044c
#define DSIPHY_PLL_KVCO_CODE		0x0458
#define DSIPHY_PLL_VREF_CFG1		0x045c
#define DSIPHY_PLL_VCO_DIV_REF1		0x046c
#define DSIPHY_PLL_VCO_DIV_REF2		0x0470
#define DSIPHY_PLL_VCO_COUNT1		0x0474
#define DSIPHY_PLL_VCO_COUNT2		0x0478
#define DSIPHY_PLL_PLLLOCK_CMP1		0x047c
#define DSIPHY_PLL_PLLLOCK_CMP2		0x0480
#define DSIPHY_PLL_PLLLOCK_CMP3		0x0484
#define DSIPHY_PLL_PLLLOCK_CMP_EN	0x0488
#define DSIPHY_PLL_PLL_VCO_TUNE		0x048C
#define DSIPHY_PLL_DEC_START		0x0490
#define DSIPHY_PLL_SSC_EN_CENTER	0x0494
#define DSIPHY_PLL_SSC_ADJ_PER1		0x0498
#define DSIPHY_PLL_SSC_ADJ_PER2		0x049c
#define DSIPHY_PLL_SSC_PER1		0x04a0
#define DSIPHY_PLL_SSC_PER2		0x04a4
#define DSIPHY_PLL_SSC_STEP_SIZE1	0x04a8
#define DSIPHY_PLL_SSC_STEP_SIZE2	0x04ac
#define DSIPHY_PLL_DIV_FRAC_START1	0x04b4
#define DSIPHY_PLL_DIV_FRAC_START2	0x04b8
#define DSIPHY_PLL_DIV_FRAC_START3	0x04bc
#define DSIPHY_PLL_TXCLK_EN		0x04c0
#define DSIPHY_PLL_PLL_CRCTRL		0x04c4
#define DSIPHY_PLL_RESET_SM_READY_STATUS 0x04cc
#define DSIPHY_PLL_PLL_MISC1		0x04e8
#define DSIPHY_PLL_CP_SET_CUR		0x04f0
#define DSIPHY_PLL_PLL_ICPMSET		0x04f4
#define DSIPHY_PLL_PLL_ICPCSET		0x04f8
#define DSIPHY_PLL_PLL_ICP_SET		0x04fc
#define DSIPHY_PLL_PLL_LPF1		0x0500
#define DSIPHY_PLL_PLL_LPF2_POSTDIV	0x0504
#define DSIPHY_PLL_PLL_BANDGAP		0x0508

struct dsi_pll_14nm_input {
	u32 fref;		/* 19.2 MHz reference clock */
	u32 dsiclk_sel;		/* reg 0x0014 */
	u32 ssc_en;		/* reg 0x0494, bit 0 */
	u32 ldo_en;		/* reg 0x004c, bit 0 */
	u32 refclk_dbler_en;	/* reg 0x04c0, bit 1 */
	u32 vco_measure_time;
	u32 kvco_measure_time;
	u32 bandgap_timer;	/* reg 0x0430, bit 3 - 5 */
	u32 pll_wakeup_timer;	/* reg 0x043c, bit 0 - 2 */
	u32 plllock_cnt;	/* reg 0x0488, bit 1 - 2 */
	u32 plllock_rng;	/* reg 0x0488, bit 3 - 4 */
	u32 ssc_center;		/* reg 0x0494, bit 1 */
	u32 ssc_adj_period;	/* reg 0x0498, bit 0 - 9 */
	u32 ssc_spread;
	u32 ssc_freq;
	u32 pll_ie_trim;	/* reg 0x0400 */
	u32 pll_ip_trim;	/* reg 0x0404 */
	u32 pll_iptat_trim;	/* reg 0x0410 */
	u32 pll_cpcset_cur;	/* reg 0x04f0, bit 0 - 2 */
	u32 pll_cpmset_cur;	/* reg 0x04f0, bit 3 - 5 */
	u32 pll_icpmset;	/* reg 0x04fc, bit 3 - 5 */
	u32 pll_icpcset;	/* reg 0x04fc, bit 0 - 2 */
	u32 pll_icpmset_p;	/* reg 0x04f4, bit 0 - 2 */
	u32 pll_icpmset_m;	/* reg 0x04f4, bit 3 - 5 */
	u32 pll_icpcset_p;	/* reg 0x04f8, bit 0 - 2 */
	u32 pll_icpcset_m;	/* reg 0x04f8, bit 3 - 5 */
	u32 pll_lpf_res1;	/* reg 0x0504, bit 0 - 3 */
	u32 pll_lpf_cap1;	/* reg 0x0500, bit 0 - 3 */
	u32 pll_lpf_cap2;	/* reg 0x0500, bit 4 - 7 */
	u32 pll_c3ctrl;		/* reg 0x04c4 */
	u32 pll_r3ctrl;		/* reg 0x04c4 */
};

struct dsi_pll_14nm_output {
	u32 pll_txclk_en;	/* reg 0x04c0 */
	u32 dec_start;		/* reg 0x0490 */
	u32 div_frac_start;	/* reg 0x04b4, 0x04b8, 0x04bc */
	u32 ssc_period;		/* reg 0x04a0, 0x04a4 */
	u32 ssc_step_size;	/* reg 0x04a8, 0x04ac */
	u32 plllock_cmp;	/* reg 0x047c, 0x0480, 0x0484 */
	u32 pll_vco_div_ref;	/* reg 0x046c, 0x0470 */
	u32 pll_vco_count;	/* reg 0x0474, 0x0478 */
	u32 pll_kvco_div_ref;	/* reg 0x0440, 0x0444 */
	u32 pll_kvco_count;	/* reg 0x0448, 0x044c */
	u32 pll_misc1;		/* reg 0x04e8 */
	u32 pll_resetsm_cntrl;	/* reg 0x042c */
	u32 pll_resetsm_cntrl2;	/* reg 0x0430 */
	u32 pll_resetsm_cntrl5;	/* reg 0x043c */
	u32 pll_kvco_code;	/* reg 0x0458 */
	u32 cmn_ldo_cntrl;	/* reg 0x004c */
	u32 pll_postdiv;	/* tx_band, 1 */
	u32 pll_n1div;		/* reg 0x0010, bit 0 - 3: bit clock divider */
	u32 pll_n2div;		/* reg 0x0010, bit 4 - 7: pixel clock divider */
};

struct dsi_pll_14nm {
	struct dsi_pll_resource *rsc;
	struct dsi_pll_14nm_input in;
	struct dsi_pll_14nm_output out;
};

static struct dsi_pll_14nm plls[DSI_PLL_14NM_MAX];

static bool dsi_pll_14nm_locked(struct dsi_pll_resource *rsc)
{
	u32 status;

	/* poll for PLL ready status */
	if (readl_poll_timeout_atomic(rsc->pll_base +
			DSIPHY_PLL_RESET_SM_READY_STATUS, status,
			(status & BIT(5)), DSI_PLL_POLL_MAX_READS,
			DSI_PLL_POLL_TIMEOUT_US)) {
		DSI_PLL_ERR(rsc, "status=%x failed to lock\n", status);
		return false;
	}

	if (readl_poll_timeout_atomic(rsc->pll_base +
			DSIPHY_PLL_RESET_SM_READY_STATUS, status,
			(status & BIT(0)), DSI_PLL_POLL_MAX_READS,
			DSI_PLL_POLL_TIMEOUT_US)) {
		DSI_PLL_ERR(rsc, "status=%x PLL not ready\n", status);
		return false;
	}

	return true;
}

static void dsi_pll_14nm_input_init(struct dsi_pll_14nm *pll)
{
	struct dsi_pll_resource *rsc = pll->rsc;
	struct dsi_pll_14nm_input *pin = &pll->in;

	pin->fref = VCO_REF_CLK_RATE;
	pin->dsiclk_sel = 1;
	pin->ssc_en = rsc->ssc_en;
	pin->ldo_en = 0;

	pin->refclk_dbler_en = 0;
	pin->vco_measure_time = 5;
	pin->kvco_measure_time = 5;
	pin->bandgap_timer = 4;
	pin->pll_wakeup_timer = 5;
	pin->plllock_cnt = 1;
	pin->plllock_rng = 0;
	pin->ssc_center = rsc->ssc_center;
	pin->ssc_adj_period = 37;
	pin->ssc_spread = rsc->ssc_ppm / 1000;
	pin->ssc_freq = rsc->ssc_freq;

	pin->pll_ie_trim = 4;
	pin->pll_ip_trim = 4;
	pin->pll_cpcset_cur = 1;
	pin->pll_cpmset_cur = 1;
	pin->pll_icpmset = 7;
	pin->pll_icpcset = 7;
	pin->pll_icpmset_p = 0;
	pin->pll_icpmset_m = 0;
	pin->pll_icpcset_p = 0;
	pin->pll_icpcset_m = 0;
	pin->pll_lpf_res1 = 3;
	pin->pll_lpf_cap1 = 11;
	pin->pll_lpf_cap2 = 1;
	pin->pll_iptat_trim = 7;
	pin->pll_c3ctrl = 2;
	pin->pll_r3ctrl = 1;
	pll->out.pll_postdiv = 1;
}

static void dsi_pll_14nm_ssc_calc(struct dsi_pll_14nm *pll)
{
	struct dsi_pll_resource *rsc = pll->rsc;
	u32 period, ssc_period;
	u32 ref, rem;
	s64 step_size;

	ssc_period = pll->in.ssc_freq / 500;
	if (!ssc_period) {
		DSI_PLL_WARN(rsc, "no SSC frequency, SSC off\n");
		pll->in.ssc_en = 0;
		return;
	}
	period = (u32)(rsc->vco_ref_clk_rate / 1000);
	ssc_period = CEIL(period, ssc_period);
	ssc_period -= 1;
	pll->out.ssc_period = ssc_period;

	step_size = (u32)rsc->vco_current_rate;
	ref = rsc->vco_ref_clk_rate;
	ref /= 1000;
	step_size = div_s64(step_size, ref);
	step_size <<= 20;
	step_size = div_s64(step_size, 1000);
	step_size *= pll->in.ssc_spread;
	step_size = div_s64(step_size, 1000);
	step_size *= (pll->in.ssc_adj_period + 1);

	rem = 0;
	step_size = div_s64_rem(step_size, ssc_period + 1, &rem);
	if (rem)
		step_size++;

	step_size &= 0x0ffff;	/* lower 16 bits */
	pll->out.ssc_step_size = step_size;

	DSI_PLL_DBG(rsc, "ssc freq=%d spread=%d period=%d step=%lld\n",
		pll->in.ssc_freq, pll->in.ssc_spread, ssc_period, step_size);
}

static void dsi_pll_14nm_dec_frac_calc(struct dsi_pll_14nm *pll)
{
	struct dsi_pll_resource *rsc = pll->rsc;
	struct dsi_pll_14nm_input *pin = &pll->in;
	struct dsi_pll_14nm_output *pout = &pll->out;
	u64 multiplier = BIT(20);
	u64 dec_start_multiple, dec_start, pll_comp_val;
	s32 duration, div_frac_start;
	s64 vco_clk_rate = rsc->vco_current_rate;
	s64 fref = rsc->vco_ref_clk_rate;

	dec_start_multiple = div_s64(vco_clk_rate * multiplier, fref);
	div_s64_rem(dec_start_multiple, multiplier, &div_frac_start);

	dec_start = div_s64(dec_start_multiple, multiplier);

	pout->dec_start = (u32)dec_start;
	pout->div_frac_start = div_frac_start;

	if (pin->plllock_cnt == 0)
		duration = 1024;
	else if (pin->plllock_cnt == 1)
		duration = 256;
	else if (pin->plllock_cnt == 2)
		duration = 128;
	else
		duration = 32;

	pll_comp_val = duration * dec_start_multiple;
	pll_comp_val = div_u64(pll_comp_val, multiplier);
	do_div(pll_comp_val, 10);

	pout->plllock_cmp = (u32)pll_comp_val;

	pout->pll_txclk_en = 1;
	/* 4.14: 0x3c only on the MSM8996 v2 PLL revision */
	pout->cmn_ldo_cntrl = 0x1c;
}

static u32 dsi_pll_14nm_kvco_slop(u32 vrate)
{
	u32 slop = 0;

	if (vrate > 1300000000UL && vrate <= 1800000000UL)
		slop = 600;
	else if (vrate > 1800000000UL && vrate < 2300000000UL)
		slop = 400;
	else if (vrate > 2300000000UL && vrate < 2600000000UL)
		slop = 280;

	return slop;
}

static void dsi_pll_14nm_calc_vco_count(struct dsi_pll_14nm *pll)
{
	struct dsi_pll_resource *rsc = pll->rsc;
	struct dsi_pll_14nm_input *pin = &pll->in;
	struct dsi_pll_14nm_output *pout = &pll->out;
	s64 vco_clk_rate = rsc->vco_current_rate;
	s64 fref = rsc->vco_ref_clk_rate;
	u64 data;
	u32 cnt;

	data = fref * pin->vco_measure_time;
	do_div(data, 1000000);
	data &= 0x03ff;	/* 10 bits */
	data -= 2;
	pout->pll_vco_div_ref = data;

	data = (unsigned long)vco_clk_rate / 1000000;	/* MHz */
	data *= pin->vco_measure_time;
	do_div(data, 10);
	pout->pll_vco_count = data;

	data = fref * pin->kvco_measure_time;
	do_div(data, 1000000);
	data &= 0x03ff;	/* 10 bits */
	data -= 1;
	pout->pll_kvco_div_ref = data;

	cnt = dsi_pll_14nm_kvco_slop(vco_clk_rate);
	cnt *= 2;
	cnt /= 100;
	cnt *= pin->kvco_measure_time;
	pout->pll_kvco_count = cnt;

	pout->pll_misc1 = 16;
	pout->pll_resetsm_cntrl = 48;
	pout->pll_resetsm_cntrl2 = pin->bandgap_timer << 3;
	pout->pll_resetsm_cntrl5 = pin->pll_wakeup_timer;
	pout->pll_kvco_code = 0;
}

static void dsi_pll_14nm_commit_ssc(struct dsi_pll_14nm *pll)
{
	void __iomem *base = pll->rsc->pll_base;
	struct dsi_pll_14nm_input *pin = &pll->in;
	struct dsi_pll_14nm_output *pout = &pll->out;
	u32 data;

	data = pin->ssc_adj_period & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_SSC_ADJ_PER1, data);
	data = (pin->ssc_adj_period >> 8) & 0x03;
	DSI_PLL_REG_W(base, DSIPHY_PLL_SSC_ADJ_PER2, data);

	data = pout->ssc_period & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_SSC_PER1, data);
	data = (pout->ssc_period >> 8) & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_SSC_PER2, data);

	data = pout->ssc_step_size & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_SSC_STEP_SIZE1, data);
	data = (pout->ssc_step_size >> 8) & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_SSC_STEP_SIZE2, data);

	data = (pin->ssc_center & 0x01) << 1;
	data |= 0x01; /* enable */
	DSI_PLL_REG_W(base, DSIPHY_PLL_SSC_EN_CENTER, data);

	wmb();	/* make sure register committed */
}

static void dsi_pll_14nm_commit_common(struct dsi_pll_14nm *pll)
{
	void __iomem *base = pll->rsc->pll_base;
	struct dsi_pll_14nm_input *pin = &pll->in;
	struct dsi_pll_14nm_output *pout = &pll->out;
	u32 data;

	/* the non frequency dependent pll registers */
	DSI_PLL_REG_W(base, DSIPHY_PLL_SYSCLK_EN_RESET, 0);

	/* DSIPHY_PLL_CLKBUFLR_EN is set by the PHY enable */

	DSI_PLL_REG_W(base, DSIPHY_PLL_TXCLK_EN, pout->pll_txclk_en);

	DSI_PLL_REG_W(base, DSIPHY_PLL_RESETSM_CNTRL, pout->pll_resetsm_cntrl);
	DSI_PLL_REG_W(base, DSIPHY_PLL_RESETSM_CNTRL2,
			pout->pll_resetsm_cntrl2);
	DSI_PLL_REG_W(base, DSIPHY_PLL_RESETSM_CNTRL5,
			pout->pll_resetsm_cntrl5);

	data = pout->pll_vco_div_ref & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_VCO_DIV_REF1, data);
	data = (pout->pll_vco_div_ref >> 8) & 0x03;
	DSI_PLL_REG_W(base, DSIPHY_PLL_VCO_DIV_REF2, data);

	data = pout->pll_kvco_div_ref & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_KVCO_DIV_REF1, data);
	data = (pout->pll_kvco_div_ref >> 8) & 0x03;
	DSI_PLL_REG_W(base, DSIPHY_PLL_KVCO_DIV_REF2, data);

	DSI_PLL_REG_W(base, DSIPHY_PLL_PLL_MISC1, pout->pll_misc1);

	DSI_PLL_REG_W(base, DSIPHY_PLL_IE_TRIM, pin->pll_ie_trim);
	DSI_PLL_REG_W(base, DSIPHY_PLL_IP_TRIM, pin->pll_ip_trim);

	data = (pin->pll_cpmset_cur << 3) | pin->pll_cpcset_cur;
	DSI_PLL_REG_W(base, DSIPHY_PLL_CP_SET_CUR, data);

	data = (pin->pll_icpcset_p << 3) | pin->pll_icpcset_m;
	DSI_PLL_REG_W(base, DSIPHY_PLL_PLL_ICPCSET, data);

	/* 4.14 writes icpcset_m here too; kept as-is */
	data = (pin->pll_icpmset_p << 3) | pin->pll_icpcset_m;
	DSI_PLL_REG_W(base, DSIPHY_PLL_PLL_ICPMSET, data);

	data = (pin->pll_icpmset << 3) | pin->pll_icpcset;
	DSI_PLL_REG_W(base, DSIPHY_PLL_PLL_ICP_SET, data);

	data = (pin->pll_lpf_cap2 << 4) | pin->pll_lpf_cap1;
	DSI_PLL_REG_W(base, DSIPHY_PLL_PLL_LPF1, data);

	DSI_PLL_REG_W(base, DSIPHY_PLL_IPTAT_TRIM, pin->pll_iptat_trim);

	data = pin->pll_c3ctrl | (pin->pll_r3ctrl << 4);
	DSI_PLL_REG_W(base, DSIPHY_PLL_PLL_CRCTRL, data);
}

static void dsi_pll_14nm_commit(struct dsi_pll_14nm *pll)
{
	void __iomem *base = pll->rsc->pll_base;
	struct dsi_pll_14nm_input *pin = &pll->in;
	struct dsi_pll_14nm_output *pout = &pll->out;
	u32 data;

	DSI_PLL_REG_W(base, DSIPHY_CMN_LDO_CNTRL, pout->cmn_ldo_cntrl);

	dsi_pll_14nm_commit_common(pll);

	/* stop pll and apply pll sw reset */
	DSI_PLL_REG_W(base, DSIPHY_CMN_PLL_CNTRL, 0);
	DSI_PLL_REG_W(base, DSIPHY_CMN_CTRL_1, 0x20);
	wmb();	/* make sure register committed */
	udelay(10);
	DSI_PLL_REG_W(base, DSIPHY_CMN_CTRL_1, 0);
	wmb();	/* make sure register committed */

	/* dsiclk_sel = 1: pixel clock is n2 output / 2 */
	DSI_PLL_REG_W(base, DSIPHY_CMN_CLK_CFG1, pin->dsiclk_sel);

	/* data, clk, pll normal operation */
	DSI_PLL_REG_W(base, DSIPHY_CMN_CTRL_0, 0xff);

	/* the frequency dependent pll registers */
	DSI_PLL_REG_W(base, DSIPHY_PLL_DEC_START, pout->dec_start);

	data = pout->div_frac_start & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_DIV_FRAC_START1, data);
	data = (pout->div_frac_start >> 8) & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_DIV_FRAC_START2, data);
	data = (pout->div_frac_start >> 16) & 0x0f;
	DSI_PLL_REG_W(base, DSIPHY_PLL_DIV_FRAC_START3, data);

	data = pout->plllock_cmp & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_PLLLOCK_CMP1, data);
	data = (pout->plllock_cmp >> 8) & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_PLLLOCK_CMP2, data);
	data = (pout->plllock_cmp >> 16) & 0x03;
	DSI_PLL_REG_W(base, DSIPHY_PLL_PLLLOCK_CMP3, data);

	data = (pin->plllock_cnt << 1) | (pin->plllock_rng << 3);
	DSI_PLL_REG_W(base, DSIPHY_PLL_PLLLOCK_CMP_EN, data);

	data = pout->pll_vco_count & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_VCO_COUNT1, data);
	data = (pout->pll_vco_count >> 8) & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_VCO_COUNT2, data);

	data = pout->pll_kvco_count & 0x0ff;
	DSI_PLL_REG_W(base, DSIPHY_PLL_KVCO_COUNT1, data);
	data = (pout->pll_kvco_count >> 8) & 0x03;
	DSI_PLL_REG_W(base, DSIPHY_PLL_KVCO_COUNT2, data);

	/* tx_band = pll_postdiv - 1: 0 = divided by 1 */
	data = ((pout->pll_postdiv - 1) << 4) | pin->pll_lpf_res1;
	DSI_PLL_REG_W(base, DSIPHY_PLL_PLL_LPF2_POSTDIV, data);

	data = pout->pll_n1div | (pout->pll_n2div << 4);
	DSI_PLL_REG_W(base, DSIPHY_CMN_CLK_CFG0, data);

	if (pin->ssc_en)
		dsi_pll_14nm_commit_ssc(pll);

	wmb();	/* make sure register committed */
}

/*
 * vco -> n1 -> bit clock (byte clock = bit clock / 8)
 *          -> n2 -> /2 -> pixel clock (dsiclk_sel = 1)
 *
 * n1 = roundup(1300 MHz / bit clock) keeps the VCO in 1.3 - 2.6 GHz.
 */
static int dsi_pll_14nm_calc_divs(struct dsi_pll_14nm *pll)
{
	struct dsi_pll_resource *rsc = pll->rsc;
	u64 bitclk, vco;
	u32 n1, n2;

	if (rsc->type != DSI_PHY_TYPE_DPHY) {
		DSI_PLL_ERR(rsc, "only D-PHY is supported\n");
		return -EINVAL;
	}

	bitclk = rsc->byteclk_rate * 8;
	if (!bitclk) {
		DSI_PLL_ERR(rsc, "no byte clock rate\n");
		return -EINVAL;
	}

	n1 = DIV_ROUND_UP_ULL(VCO_MIN_RATE, bitclk);
	n1 = clamp_t(u32, n1, 1, 15);
	vco = bitclk * n1;
	if (vco < VCO_MIN_RATE || vco > VCO_MAX_RATE) {
		DSI_PLL_ERR(rsc, "bit clock %llu needs vco %llu, out of range\n",
				bitclk, vco);
		return -EINVAL;
	}

	if (rsc->pclk_rate)
		n2 = DIV_ROUND_CLOSEST_ULL(bitclk, 2 * rsc->pclk_rate);
	else if (rsc->bpp && rsc->lanes)
		n2 = rsc->bpp / (2 * rsc->lanes);
	else
		n2 = 1;
	n2 = clamp_t(u32, n2, 1, 15);

	pll->out.pll_n1div = n1;
	pll->out.pll_n2div = n2;
	rsc->vco_rate = vco;
	rsc->pclk_rate = div_u64(bitclk, 2 * n2);

	DSI_PLL_DBG(rsc, "byte=%llu bit=%llu vco=%llu n1=%u n2=%u pclk=%llu\n",
			rsc->byteclk_rate, bitclk, vco, n1, n2, rsc->pclk_rate);

	return 0;
}

int dsi_pll_14nm_configure(void *pll_rsc, bool commit)
{
	struct dsi_pll_resource *rsc = pll_rsc;
	struct dsi_pll_14nm *pll;
	int rc;

	if (!rsc || !rsc->priv)
		return -EINVAL;
	pll = rsc->priv;

	if (!commit) {
		DSI_PLL_WARN(rsc, "dynamic clock switch is not supported\n");
		return -EOPNOTSUPP;
	}

	rc = dsi_pll_14nm_calc_divs(pll);
	if (rc)
		return rc;

	rsc->vco_current_rate = rsc->vco_rate;
	rsc->vco_ref_clk_rate = VCO_REF_CLK_RATE;

	dsi_pll_14nm_input_init(pll);
	dsi_pll_14nm_dec_frac_calc(pll);
	if (pll->in.ssc_en)
		dsi_pll_14nm_ssc_calc(pll);
	dsi_pll_14nm_calc_vco_count(pll);
	dsi_pll_14nm_commit(pll);

	return 0;
}

int dsi_pll_14nm_toggle(void *pll_rsc, bool prepare)
{
	struct dsi_pll_resource *rsc = pll_rsc;

	if (!rsc)
		return -EINVAL;

	if (!prepare) {
		DSI_PLL_REG_W(rsc->pll_base, DSIPHY_CMN_PLL_CNTRL, 0);
		wmb();	/* make sure the PLL is stopped */
		DSI_PLL_DBG(rsc, "stopped\n");
		return 0;
	}

	/*
	 * DSIPHY_PLL_CLKBUFLR_EN and DSIPHY_CMN_GLBL_TEST_CTRL are set by
	 * the PHY v2.0 enable.
	 */
	DSI_PLL_REG_W(rsc->pll_base, DSIPHY_PLL_VREF_CFG1, 0x10);
	DSI_PLL_REG_W(rsc->pll_base, DSIPHY_CMN_PLL_CNTRL, 1);
	wmb();	/* start the PLL before polling for lock */

	if (!dsi_pll_14nm_locked(rsc)) {
		DSI_PLL_ERR(rsc, "lock failed\n");
		return -EINVAL;
	}

	DSI_PLL_DBG(rsc, "locked\n");
	return 0;
}

static unsigned long dsi_pll_14nm_bitclk(struct dsi_pll_resource *rsc,
		u32 *n2)
{
	u32 cfg0 = DSI_PLL_REG_R(rsc->pll_base, DSIPHY_CMN_CLK_CFG0);
	u32 n1 = cfg0 & 0xf;

	*n2 = (cfg0 >> 4) & 0xf;
	if (!rsc->vco_current_rate || !n1)
		return 0;

	return div_u64(rsc->vco_current_rate, n1);
}

static unsigned long dsi_pll_14nm_byteclk_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct dsi_pll_clk *clk = to_pll_clk_hw(hw);
	struct dsi_pll_resource *rsc = clk->priv;
	u32 n2;

	return dsi_pll_14nm_bitclk(rsc, &n2) / 8;
}

static unsigned long dsi_pll_14nm_pclk_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct dsi_pll_clk *clk = to_pll_clk_hw(hw);
	struct dsi_pll_resource *rsc = clk->priv;
	unsigned long bitclk;
	u32 n2;

	bitclk = dsi_pll_14nm_bitclk(rsc, &n2);
	if (!n2)
		return 0;

	return bitclk / (2 * n2);
}

static long dsi_pll_14nm_byteclk_round_rate(struct clk_hw *hw,
		unsigned long rate, unsigned long *parent_rate)
{
	struct dsi_pll_clk *clk = to_pll_clk_hw(hw);
	struct dsi_pll_resource *rsc = clk->priv;

	return rsc->byteclk_rate;
}

static long dsi_pll_14nm_pclk_round_rate(struct clk_hw *hw,
		unsigned long rate, unsigned long *parent_rate)
{
	struct dsi_pll_clk *clk = to_pll_clk_hw(hw);
	struct dsi_pll_resource *rsc = clk->priv;

	return rsc->pclk_rate;
}

static int dsi_pll_14nm_set_rate_stub(struct clk_hw *hw, unsigned long rate,
		unsigned long parent_rate)
{
	return 0;
}

static int dsi_pll_14nm_prepare_stub(struct clk_hw *hw)
{
	return 0;
}

static void dsi_pll_14nm_unprepare_stub(struct clk_hw *hw)
{
}

static const struct clk_ops dsi_pll_14nm_byteclk_ops = {
	.recalc_rate = dsi_pll_14nm_byteclk_recalc_rate,
	.set_rate = dsi_pll_14nm_set_rate_stub,
	.round_rate = dsi_pll_14nm_byteclk_round_rate,
	.prepare = dsi_pll_14nm_prepare_stub,
	.unprepare = dsi_pll_14nm_unprepare_stub,
};

static const struct clk_ops dsi_pll_14nm_pclk_ops = {
	.recalc_rate = dsi_pll_14nm_pclk_recalc_rate,
	.set_rate = dsi_pll_14nm_set_rate_stub,
	.round_rate = dsi_pll_14nm_pclk_round_rate,
	.prepare = dsi_pll_14nm_prepare_stub,
	.unprepare = dsi_pll_14nm_unprepare_stub,
};

/* Same output names as the 5nm PLL: dispcc-sm6150 looks them up by name. */
static struct dsi_pll_clk dsi_pll_14nm_clks[DSI_PLL_14NM_MAX][2] = {
	{
		{ .hw.init = &(struct clk_init_data){
			.name = "dsi0_phy_pll_out_byteclk",
			.ops = &dsi_pll_14nm_byteclk_ops, }, },
		{ .hw.init = &(struct clk_init_data){
			.name = "dsi0_phy_pll_out_dsiclk",
			.ops = &dsi_pll_14nm_pclk_ops, }, },
	},
	{
		{ .hw.init = &(struct clk_init_data){
			.name = "dsi1_phy_pll_out_byteclk",
			.ops = &dsi_pll_14nm_byteclk_ops, }, },
		{ .hw.init = &(struct clk_init_data){
			.name = "dsi1_phy_pll_out_dsiclk",
			.ops = &dsi_pll_14nm_pclk_ops, }, },
	},
};

int dsi_pll_clock_register_14nm(struct platform_device *pdev,
		struct dsi_pll_resource *pll_res)
{
	struct clk_onecell_data *clk_data;
	struct clk *clk;
	int ndx, i, rc;

	if (!pdev || !pdev->dev.of_node || !pll_res || !pll_res->pll_base) {
		DSI_PLL_ERR(pll_res, "invalid params\n");
		return -EINVAL;
	}

	ndx = pll_res->index;
	if (ndx >= DSI_PLL_14NM_MAX) {
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
		dsi_pll_14nm_clks[ndx][i].priv = pll_res;
		clk = devm_clk_register(&pdev->dev,
				&dsi_pll_14nm_clks[ndx][i].hw);
		if (IS_ERR(clk)) {
			DSI_PLL_ERR(pll_res, "clock %d registration failed\n", i);
			return PTR_ERR(clk);
		}
		clk_data->clks[i] = clk;
	}

	rc = of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get,
			clk_data);
	if (!rc)
		DSI_PLL_INFO(pll_res, "registered 14nm PLL clocks\n");

	return rc;
}
