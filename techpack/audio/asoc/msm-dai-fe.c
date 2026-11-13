// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2012-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */


#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>

#include "msm-compress-q6-v2.h"

static struct snd_soc_dai_ops msm_fe_dai_ops = {};

/* Conventional and unconventional sample rate supported */
static unsigned int supported_sample_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
	88200, 96000, 176400, 192000, 352800, 384000
};

static struct snd_pcm_hw_constraint_list constraints_sample_rates = {
	.count = ARRAY_SIZE(supported_sample_rates),
	.list = supported_sample_rates,
	.mask = 0,
};

static int multimedia_startup(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	snd_pcm_hw_constraint_list(substream->runtime, 0,
		SNDRV_PCM_HW_PARAM_RATE,
		&constraints_sample_rates);
	return 0;
}

struct fe_dai_stream_aif_name {
	const char *stream_name;
	const char *aif_name;
};


static struct fe_dai_stream_aif_name fe_dai_stream_aif_names[] = {
	{"MultiMedia1 Playback",               "MM_DL1"},
	{"MultiMedia1 Capture",                "MM_UL1"},
	{"MultiMedia2 Playback",               "MM_DL2"},
	{"MultiMedia2 Capture",                "MM_UL2"},
	{"VoIP Playback",                      "VOIP_DL"},
	{"VoIP Capture",                       "VOIP_UL"},
	{"MultiMedia3 Playback",               "MM_DL3"},
	{"MultiMedia3 Capture",                "MM_UL3"},
	{"MultiMedia4 Playback",               "MM_DL4"},
	{"MultiMedia5 Playback",               "MM_DL5"},
	{"MultiMedia5 Capture",                "MM_UL5"},
	{"MultiMedia6 Playback",               "MM_DL6"},
	{"MultiMedia6 Capture",                "MM_UL6"},
	{"MultiMedia7 Playback",               "MM_DL7"},
	{"MultiMedia8 Playback",               "MM_DL8"},
	{"MultiMedia8 Capture",                "MM_UL8"},
	{"SLIMBUS0_HOSTLESS Playback",         "SLIM0_DL_HL"},
	{"SLIMBUS0_HOSTLESS Capture",          "SLIM0_UL_HL"},
	{"SLIMBUS1_HOSTLESS Playback",         "SLIM1_DL_HL"},
	{"SLIMBUS1_HOSTLESS Capture",          "SLIM1_UL_HL"},
	{"SLIMBUS3_HOSTLESS Playback",         "SLIM3_DL_HL"},
	{"SLIMBUS3_HOSTLESS Capture",          "SLIM3_UL_HL"},
	{"SLIMBUS4_HOSTLESS Playback",         "SLIM4_DL_HL"},
	{"SLIMBUS4_HOSTLESS Capture",          "SLIM4_UL_HL"},
	{"SLIMBUS6_HOSTLESS Playback",         "SLIM6_DL_HL"},
	{"SLIMBUS7_HOSTLESS Playback",         "SLIM7_DL_HL"},
	{"SLIMBUS7_HOSTLESS Capture",          "SLIM7_UL_HL"},
	{"SLIMBUS8_HOSTLESS Playback",         "SLIM8_DL_HL"},
	{"SLIMBUS8_HOSTLESS Capture",          "SLIM8_UL_HL"},
	{"CDC_DMA_HOSTLESS Playback",          "CDC_DMA_DL_HL"},
	{"CDC_DMA_HOSTLESS Capture",           "CDC_DMA_UL_HL"},
	{"TX3_CDC_DMA_HOSTLESS Capture",       "TX3_CDC_DMA_UL_HL"},
	{"TX4_CDC_DMA_HOSTLESS Capture",       "TX4_CDC_DMA_UL_HL"},
	{"INT_FM_HOSTLESS Playback",           "INTFM_DL_HL"},
	{"INT_FM_HOSTLESS Capture",            "INTFM_UL_HL"},
	{"INT_HFP_BT Hostless Playback",       "INTHFP_DL_HL"},
	{"INT_HFP_BT Hostless Capture",        "INTHFP_UL_HL"},
	{"USBAUDIO_HOSTLESS Playback",         "USBAUDIO_DL_HL"},
	{"USBAUDIO_HOSTLESS Capture",          "USBAUDIO_UL_HL"},
	{"HDMI_HOSTLESS Playback",             "HDMI_DL_HL"},
	{"DISPLAY_PORT_HOSTLESS Playback",     "DP_DL_HL"},
	{"AUXPCM_HOSTLESS Playback",           "AUXPCM_DL_HL"},
	{"AUXPCM_HOSTLESS Capture",            "AUXPCM_UL_HL"},
	{"SEC_AUXPCM_HOSTLESS Playback",       "SEC_AUXPCM_DL_HL"},
	{"SEC_AUXPCM_HOSTLESS Capture",        "SEC_AUXPCM_UL_HL"},
	{"TERT_AUXPCM_HOSTLESS Playback",      "TERT_AUXPCM_DL_HL"},
	{"TERT_AUXPCM_HOSTLESS Capture",       "TERT_AUXPCM_UL_HL"},
	{"VOICE_STUB Playback",                "VOICE_STUB_DL"},
	{"VOICE_STUB Capture",                 "VOICE_STUB_UL"},
	{"VoLTE Playback",                     "VoLTE_DL"},
	{"VoLTE Capture",                      "VoLTE_UL"},
	{"MI2S_RX_HOSTLESS Playback",          "MI2S_DL_HL"},
	{"MI2S_TX_HOSTLESS Capture",           "MI2S_UL_HL"},
	{"SEC_I2S_RX_HOSTLESS Playback",       "SEC_I2S_DL_HL"},
	{"Primary MI2S_TX Hostless Capture",   "PRI_MI2S_UL_HL"},
	{"Primary MI2S_RX Hostless Playback",  "PRI_MI2S_DL_HL"},
	{"Secondary MI2S_TX Hostless Capture", "SEC_MI2S_UL_HL"},
	{"Secondary MI2S_RX Hostless Playback","SEC_MI2S_DL_HL"},
	{"Tertiary MI2S_TX Hostless Capture",  "TERT_MI2S_UL_HL"},
	{"Tertiary MI2S_RX Hostless Playback", "TERT_MI2S_DL_HL"},
	{"Quaternary MI2S_TX Hostless Capture", "QUAT_MI2S_UL_HL"},
	{"Senary MI2S_TX Hostless Capture",    "SEN_MI2S_UL_HL"},
	{"Senary MI2S_RX Hostless Playback",   "SEN_MI2S_DL_HL"},
	{"Quaternary MI2S_RX Hostless Playback","QUAT_MI2S_DL_HL"},
	{"INT0 MI2S_RX Hostless Playback",      "INT0_MI2S_DL_HL"},
	{"INT4 MI2S_RX Hostless Playback",      "INT4_MI2S_DL_HL"},
	{"INT3 MI2S_TX Hostless Capture",      "INT3_MI2S_UL_HL"},
	{"Primary TDM0 Hostless Capture",       "PRI_TDM_TX_0_UL_HL"},
	{"Primary TDM0 Hostless Playback",      "PRI_TDM_RX_0_DL_HL"},
	{"Primary TDM1 Hostless Capture",       "PRI_TDM_TX_1_UL_HL"},
	{"Primary TDM1 Hostless Playback",      "PRI_TDM_RX_1_DL_HL"},
	{"Primary TDM2 Hostless Capture",       "PRI_TDM_TX_2_UL_HL"},
	{"Primary TDM2 Hostless Playback",      "PRI_TDM_RX_2_DL_HL"},
	{"Primary TDM3 Hostless Capture",       "PRI_TDM_TX_3_UL_HL"},
	{"Primary TDM3 Hostless Playback",      "PRI_TDM_RX_3_DL_HL"},
	{"Primary TDM4 Hostless Capture",       "PRI_TDM_TX_4_UL_HL"},
	{"Primary TDM4 Hostless Playback",      "PRI_TDM_RX_4_DL_HL"},
	{"Primary TDM5 Hostless Capture",       "PRI_TDM_TX_5_UL_HL"},
	{"Primary TDM5 Hostless Playback",      "PRI_TDM_RX_5_DL_HL"},
	{"Primary TDM6 Hostless Capture",       "PRI_TDM_TX_6_UL_HL"},
	{"Primary TDM6 Hostless Playback",      "PRI_TDM_RX_6_DL_HL"},
	{"Primary TDM7 Hostless Capture",       "PRI_TDM_TX_7_UL_HL"},
	{"Primary TDM7 Hostless Playback",      "PRI_TDM_RX_7_DL_HL"},
	{"Secondary TDM0 Hostless Capture",     "SEC_TDM_TX_0_UL_HL"},
	{"Secondary TDM0 Hostless Playback",    "SEC_TDM_RX_0_DL_HL"},
	{"Secondary TDM1 Hostless Capture",     "SEC_TDM_TX_1_UL_HL"},
	{"Secondary TDM1 Hostless Playback",    "SEC_TDM_RX_1_DL_HL"},
	{"Secondary TDM2 Hostless Capture",     "SEC_TDM_TX_2_UL_HL"},
	{"Secondary TDM2 Hostless Playback",    "SEC_TDM_RX_2_DL_HL"},
	{"Secondary TDM3 Hostless Capture",     "SEC_TDM_TX_3_UL_HL"},
	{"Secondary TDM3 Hostless Playback",    "SEC_TDM_RX_3_DL_HL"},
	{"Secondary TDM4 Hostless Capture",     "SEC_TDM_TX_4_UL_HL"},
	{"Secondary TDM4 Hostless Playback",    "SEC_TDM_RX_4_DL_HL"},
	{"Secondary TDM5 Hostless Capture",     "SEC_TDM_TX_5_UL_HL"},
	{"Secondary TDM5 Hostless Playback",    "SEC_TDM_RX_5_DL_HL"},
	{"Secondary TDM6 Hostless Capture",     "SEC_TDM_TX_6_UL_HL"},
	{"Secondary TDM6 Hostless Playback",    "SEC_TDM_RX_6_DL_HL"},
	{"Secondary TDM7 Hostless Capture",     "SEC_TDM_TX_7_UL_HL"},
	{"Secondary TDM7 Hostless Playback",	"SEC_TDM_RX_7_DL_HL"},
	{"Tertiary TDM0 Hostless Capture",      "TERT_TDM_TX_0_UL_HL"},
	{"Tertiary TDM0 Hostless Playback",     "TERT_TDM_RX_0_DL_HL"},
	{"Tertiary TDM1 Hostless Capture",      "TERT_TDM_TX_1_UL_HL"},
	{"Tertiary TDM1 Hostless Playback",     "TERT_TDM_RX_1_DL_HL"},
	{"Tertiary TDM2 Hostless Capture",      "TERT_TDM_TX_2_UL_HL"},
	{"Tertiary TDM2 Hostless Playback",     "TERT_TDM_RX_2_DL_HL"},
	{"Tertiary TDM3 Hostless Capture",      "TERT_TDM_TX_3_UL_HL"},
	{"Tertiary TDM3 Hostless Playback",     "TERT_TDM_RX_3_DL_HL"},
	{"Tertiary TDM4 Hostless Capture",      "TERT_TDM_TX_4_UL_HL"},
	{"Tertiary TDM4 Hostless Playback",     "TERT_TDM_RX_4_DL_HL"},
	{"Tertiary TDM5 Hostless Capture",      "TERT_TDM_TX_5_UL_HL"},
	{"Tertiary TDM5 Hostless Playback",     "TERT_TDM_RX_5_DL_HL"},
	{"Tertiary TDM6 Hostless Capture",      "TERT_TDM_TX_6_UL_HL"},
	{"Tertiary TDM6 Hostless Playback",     "TERT_TDM_RX_6_DL_HL"},
	{"Tertiary TDM7 Hostless Capture",      "TERT_TDM_TX_7_UL_HL"},
	{"Tertiary TDM7 Hostless Playback", 	"TERT_TDM_RX_7_DL_HL"},
	{"Quaternary TDM0 Hostless Capture",	"QUAT_TDM_TX_0_UL_HL"},
	{"Quaternary TDM0 Hostless Playback",	"QUAT_TDM_RX_0_DL_HL"},
	{"Quaternary TDM1 Hostless Capture",	"QUAT_TDM_TX_1_UL_HL"},
	{"Quaternary TDM1 Hostless Playback",	"QUAT_TDM_RX_1_DL_HL"},
	{"Quaternary TDM2 Hostless Capture",	"QUAT_TDM_TX_2_UL_HL"},
	{"Quaternary TDM2 Hostless Playback",	"QUAT_TDM_RX_2_DL_HL"},
	{"Quaternary TDM3 Hostless Capture",	"QUAT_TDM_TX_3_UL_HL"},
	{"Quaternary TDM3 Hostless Playback",	"QUAT_TDM_RX_3_DL_HL"},
	{"Quaternary TDM4 Hostless Capture",	"QUAT_TDM_TX_4_UL_HL"},
	{"Quaternary TDM4 Hostless Playback",	"QUAT_TDM_RX_4_DL_HL"},
	{"Quaternary TDM5 Hostless Capture",	"QUAT_TDM_TX_5_UL_HL"},
	{"Quaternary TDM5 Hostless Playback",	"QUAT_TDM_RX_5_DL_HL"},
	{"Quaternary TDM6 Hostless Capture",	"QUAT_TDM_TX_6_UL_HL"},
	{"Quaternary TDM6 Hostless Playback",	"QUAT_TDM_RX_6_DL_HL"},
	{"Quaternary TDM7 Hostless Capture",	"QUAT_TDM_TX_7_UL_HL"},
	{"Quaternary TDM7 Hostless Playback",	"QUAT_TDM_RX_7_DL_HL"},
	{"Quinary TDM7 Hostless Capture",       "QUIN_TDM_TX_7_UL_HL"},
	{"Quinary TDM7 Hostless Playback",      "QUIN_TDM_RX_7_DL_HL"},
	{"DTMF_RX_HOSTLESS Playback",           "DTMF_DL_HL"},
	{"CPE Listen Audio capture",            "CPE_LSM_UL_HL"},
	{"VOLTE_STUB Playback",                 "VOLTE_STUB_DL"},
	{"VOLTE_STUB Capture",                  "VOLTE_STUB_UL"},
	{"VOICE2_STUB Playback",                "VOICE2_STUB_DL"},
	{"VOICE2_STUB Capture",                 "VOICE2_STUB_UL"},
	{"MultiMedia9 Playback",                "MM_DL9"},
	{"MultiMedia9 Capture",                 "MM_UL9"},
	{"QCHAT Playback",                      "QCHAT_DL"},
	{"QCHAT Capture",                       "QCHAT_UL"},
	{"Listen 1 Audio Service Capture",      "LSM1_UL_HL"},
	{"Listen 2 Audio Service Capture",      "LSM2_UL_HL"},
	{"Listen 3 Audio Service Capture",      "LSM3_UL_HL"},
	{"Listen 4 Audio Service Capture",      "LSM4_UL_HL"},
	{"Listen 5 Audio Service Capture",      "LSM5_UL_HL"},
	{"Listen 6 Audio Service Capture",      "LSM6_UL_HL"},
	{"Listen 7 Audio Service Capture",      "LSM7_UL_HL"},
	{"Listen 8 Audio Service Capture",      "LSM8_UL_HL"},
	{"MultiMedia10 Playback",               "MM_DL10"},
	{"MultiMedia10 Capture",                "MM_UL10"},
	{"MultiMedia11 Playback",               "MM_DL11"},
	{"MultiMedia12 Playback",               "MM_DL12"},
	{"MultiMedia13 Playback",               "MM_DL13"},
	{"MultiMedia14 Playback",               "MM_DL14"},
	{"MultiMedia15 Playback",               "MM_DL15"},
	{"MultiMedia16 Playback",               "MM_DL16"},
	{"MultiMedia16 Capture",                "MM_UL16"},
	{"VoiceMMode1 Playback",                "VOICEMMODE1_DL"},
	{"VoiceMMode1 Capture",                 "VOICEMMODE1_UL"},
	{"VoiceMMode2 Playback",                "VOICEMMODE2_DL"},
	{"VoiceMMode2 Capture",                 "VOICEMMODE2_UL"},
	{"MultiMedia17 Capture",                "MM_UL17"},
	{"MultiMedia18 Capture",                "MM_UL18"},
	{"MultiMedia19 Capture",                "MM_UL19"},
	{"MultiMedia20 Playback",               "MM_DL20"},
	{"MultiMedia20 Capture",                "MM_UL20"},
	{"MultiMedia21 Playback",               "MM_DL21"},
	{"MultiMedia21 Capture",                "MM_UL21"},
	{"MultiMedia22 Playback",               "MM_DL22"},
	{"MultiMedia22 Capture",                "MM_UL22"},
	{"MultiMedia23 Playback",               "MM_DL23"},
	{"MultiMedia23 Capture",                "MM_UL23"},
	{"MultiMedia24 Playback",               "MM_DL24"},
	{"MultiMedia24 Capture",                "MM_UL24"},
	{"MultiMedia25 Playback",               "MM_DL25"},
	{"MultiMedia25 Capture",                "MM_UL25"},
	{"MultiMedia26 Playback",               "MM_DL26"},
	{"MultiMedia27 Capture",                "MM_UL27"},
	{"MultiMedia28 Capture",                "MM_UL28"},
	{"MultiMedia29 Capture",                "MM_UL29"},
	{"MultiMedia30 Capture",                "MM_UL30"},
	{"MultiMedia31 Playback",               "MM_DL31"},
	{"MultiMedia31 Capture",                "MM_UL31"},
	{"MultiMedia32 Playback",               "MM_DL32"},
	{"MultiMedia32 Capture",                "MM_UL32"},
	{"MultiMedia33 Playback",               "MM_DL33"},
	{"MultiMedia33 Capture",                "MM_UL33"},
	{"MultiMedia34 Playback",               "MM_DL34"},
	{"MultiMedia34 Capture",                "MM_UL34"},
	{"MultiMedia35 Playback",               "MM_DL35"},
	{"MultiMedia35 Capture",                "MM_UL35"},
	{"MultiMedia36 Playback",               "MM_DL36"},
	{"MultiMedia36 Capture",                "MM_UL36"},
	{"CS-VOICE Playback",                   "CS-VOICE_DL1"},
	{"CS-VOICE Capture",                    "CS-VOICE_UL1"},

};

static const char* fe_dai_get_aifname(const char *stream_name)
{
        int i = 0,N = ARRAY_SIZE(fe_dai_stream_aif_names);
        for (i = 0; i < N; i++) {
                if (fe_dai_stream_aif_names[i].stream_name &&
                        !strcmp(stream_name,fe_dai_stream_aif_names[i].stream_name))
                        return fe_dai_stream_aif_names[i].aif_name;
        }
        return 0;
}

static int fe_dai_probe(struct snd_soc_dai *dai)
{
	struct snd_soc_dapm_route intercon;
	struct snd_soc_dapm_context *dapm;
	const char *aif_name;

	if (!dai || !dai->driver) {
		pr_err("%s invalid params\n", __func__);
		return -EINVAL;
	}
	dapm = snd_soc_component_get_dapm(dai->component);
	memset(&intercon, 0, sizeof(intercon));
	if (dai->driver->playback.stream_name) {
		dev_dbg(dai->dev, "%s add route for widget %s",
			__func__, dai->driver->playback.stream_name);
		aif_name = fe_dai_get_aifname(dai->driver->playback.stream_name);
		if(aif_name){
			intercon.source = dai->driver->playback.stream_name;
			intercon.sink = aif_name;
			dev_dbg(dai->dev, "%s src %s sink %s\n",
				__func__, intercon.source, intercon.sink);
			snd_soc_dapm_add_routes(dapm, &intercon, 1);
			snd_soc_dapm_ignore_suspend(dapm, intercon.source);
		}
	}
	if (dai->driver->capture.stream_name) {
		dev_dbg(dai->dev, "%s add route for widget %s",
			__func__, dai->driver->capture.stream_name);
		aif_name = fe_dai_get_aifname(dai->driver->capture.stream_name);
		if(aif_name){
			intercon.sink = dai->driver->capture.stream_name;
			intercon.source = aif_name;
			dev_dbg(dai->dev, "%s src %s sink %s\n",
				__func__, intercon.source, intercon.sink);
			snd_soc_dapm_add_routes(dapm, &intercon, 1);
			snd_soc_dapm_ignore_suspend(dapm, intercon.sink);
		}
	}
	return 0;
}

static struct snd_soc_dai_ops msm_fe_Multimedia_dai_ops = {
	.startup	= multimedia_startup,
};

static const struct snd_soc_component_driver msm_fe_dai_component = {
	.name		= "msm-dai-fe",
};

static struct snd_soc_dai_driver msm_fe_dais[] = {
	{
		.playback = {
			.stream_name = "MultiMedia1 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE |
						SNDRV_PCM_FMTBIT_S24_3LE |
						SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =	384000,
		},
		.capture = {
			.stream_name = "MultiMedia1 Capture",
			.rates = (SNDRV_PCM_RATE_8000_384000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =	384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia1",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia2 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE |
						SNDRV_PCM_FMTBIT_S24_3LE |
						SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =	384000,
		},
		.capture = {
			.stream_name = "MultiMedia2 Capture",
			.rates = (SNDRV_PCM_RATE_8000_384000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =	384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia2",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "VoIP Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
					SNDRV_PCM_FMTBIT_SPECIAL,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =	8000,
			.rate_max = 48000,
		},
		.capture = {
			.stream_name = "VoIP Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
					SNDRV_PCM_FMTBIT_SPECIAL,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =	8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "VoIP",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia3 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE |
						SNDRV_PCM_FMTBIT_S24_3LE |
						SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia3 Capture",
			.rates = (SNDRV_PCM_RATE_8000_384000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE |
						SNDRV_PCM_FMTBIT_S24_3LE |
						SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =	384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia3",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia4 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE |
						SNDRV_PCM_FMTBIT_S24_3LE |
						SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia4",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia5 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE |
						SNDRV_PCM_FMTBIT_S24_3LE |
						SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia5 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =	48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia5",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia6 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE |
						SNDRV_PCM_FMTBIT_S24_3LE |
						SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia6 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =	48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia6",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia7 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE |
						SNDRV_PCM_FMTBIT_S24_3LE |
						SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia7",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia8 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE |
						SNDRV_PCM_FMTBIT_S24_3LE |
						SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia8 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =	48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia8",
		.probe = fe_dai_probe,
	},
	/* FE DAIs created for hostless operation purpose */
	{
		.playback = {
			.stream_name = "SLIMBUS0_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.capture = {
			.stream_name = "SLIMBUS0_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000_96000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SLIMBUS0_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "SLIMBUS1_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.capture = {
			.stream_name = "SLIMBUS1_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SLIMBUS1_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "SLIMBUS3_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.capture = {
			.stream_name = "SLIMBUS3_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SLIMBUS3_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "SLIMBUS4_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
						SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.capture = {
			.stream_name = "SLIMBUS4_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SLIMBUS4_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "SLIMBUS6_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SLIMBUS6_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "SLIMBUS7_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.capture = {
			.stream_name = "SLIMBUS7_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SLIMBUS7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "SLIMBUS8_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.capture = {
			.stream_name = "SLIMBUS8_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SLIMBUS8_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "CDC_DMA_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |
				SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 |
				SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
				SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
				SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
				SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_352800 |
				SNDRV_PCM_RATE_384000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE |
				   SNDRV_PCM_FMTBIT_S24_3LE |
				   SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 1,
			.channels_max = 4,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "CDC_DMA_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |
				SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 |
				SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
				SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
				SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
				SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_352800 |
				SNDRV_PCM_RATE_384000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE |
				   SNDRV_PCM_FMTBIT_S24_3LE,
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "CDC_DMA_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "TX3_CDC_DMA_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |
				SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 |
				SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
				SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
				SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
				SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_352800 |
				SNDRV_PCM_RATE_384000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE |
				   SNDRV_PCM_FMTBIT_S24_3LE,
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TX3_CDC_DMA_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "TX4_CDC_DMA_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |
				SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 |
				SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
				SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
				SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
				SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_352800 |
				SNDRV_PCM_RATE_384000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE |
				   SNDRV_PCM_FMTBIT_S24_3LE,
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TX4_CDC_DMA_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "INT_FM_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.capture = {
			.stream_name = "INT_FM_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "INT_FM_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "INT_HFP_BT Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     16000,
		},
		.capture = {
			.stream_name = "INT_HFP_BT Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     16000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "INT_HFP_BT_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "USBAUDIO_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |
				SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 |
				SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
				SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
				SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
				SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_352800 |
				SNDRV_PCM_RATE_384000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S24_3LE |
				SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.capture = {
			.stream_name = "USBAUDIO_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |
				SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 |
				SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
				SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
				SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
				SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_352800 |
				SNDRV_PCM_RATE_384000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S24_3LE |
				SNDRV_PCM_FMTBIT_S32_LE,
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "USBAUDIO_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "HDMI_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "HDMI_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "DISPLAY_PORT_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "DISPLAY_PORT_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "AUXPCM_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 1,
			.rate_min =     8000,
			.rate_max =     16000,
		},
		.capture = {
			.stream_name = "AUXPCM_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 1,
			.rate_min =     8000,
			.rate_max =    16000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "AUXPCM_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "SEC_AUXPCM_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 1,
			.rate_min =     8000,
			.rate_max =     16000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_AUXPCM_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "SEC_AUXPCM_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 1,
			.rate_min =     8000,
			.rate_max =    16000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_AUXPCM_TX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "TERT_AUXPCM_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 1,
			.rate_min =     8000,
			.rate_max =     16000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_AUXPCM_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "TERT_AUXPCM_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 1,
			.rate_min =     8000,
			.rate_max =    16000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_AUXPCM_TX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "VOICE_STUB Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.capture = {
			.stream_name = "VOICE_STUB Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "VOICE_STUB",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "VoLTE Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.capture = {
			.stream_name = "VoLTE Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "VoLTE",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MI2S_RX_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.capture = {
			.stream_name = "MI2S_TX_HOSTLESS Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "MI2S_TX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "SEC_I2S_RX_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =    48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_I2S_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Primary MI2S_TX Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_MI2S_TX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Primary MI2S_RX Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =    384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_MI2S_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Secondary MI2S_TX Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_MI2S_TX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Secondary MI2S_RX Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_MI2S_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Tertiary MI2S_TX Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_MI2S_TX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Tertiary MI2S_RX Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =	8000,
			.rate_max =    384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_MI2S_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Quaternary MI2S_TX Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_MI2S_TX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Quaternary MI2S_RX Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_MI2S_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Senary MI2S_TX Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEN_MI2S_TX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Senary MI2S_RX Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_384000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE,
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEN_MI2S_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "INT0 MI2S_RX Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =    192000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "INT0_MI2S_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "INT4 MI2S_RX Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 4,
			.rate_min =     8000,
			.rate_max =    192000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "INT4_MI2S_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "INT3 MI2S_TX Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "INT3_MI2S_TX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	/* TDM Hostless */
	{
		.capture = {
			.stream_name = "Primary TDM0 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_TX_0_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Primary TDM0 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_RX_0_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Primary TDM1 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_TX_1_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Primary TDM1 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_RX_1_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Primary TDM2 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_TX_2_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Primary TDM2 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_RX_2_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Primary TDM3 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_TX_3_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Primary TDM3 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_RX_3_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Primary TDM4 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_TX_4_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Primary TDM4 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_RX_4_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Primary TDM5 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_TX_5_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Primary TDM5 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_RX_5_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Primary TDM6 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_TX_6_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Primary TDM6 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_RX_6_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Primary TDM7 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_TX_7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Primary TDM7 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "PRI_TDM_RX_7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Secondary TDM0 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_TX_0_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Secondary TDM0 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_RX_0_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Secondary TDM1 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_TX_1_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Secondary TDM1 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_RX_1_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Secondary TDM2 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_TX_2_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Secondary TDM2 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_RX_2_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Secondary TDM3 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_TX_3_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Secondary TDM3 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_RX_3_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Secondary TDM4 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_TX_4_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Secondary TDM4 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_RX_4_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Secondary TDM5 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_TX_5_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Secondary TDM5 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_RX_5_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Secondary TDM6 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_TX_6_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Secondary TDM6 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_RX_6_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Secondary TDM7 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_TX_7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Secondary TDM7 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "SEC_TDM_RX_7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Tertiary TDM0 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_TX_0_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Tertiary TDM0 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_RX_0_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Tertiary TDM1 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_TX_1_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Tertiary TDM1 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_RX_1_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Tertiary TDM2 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_TX_2_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Tertiary TDM2 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_RX_2_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Tertiary TDM3 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_TX_3_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Tertiary TDM3 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_RX_3_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Tertiary TDM4 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_TX_4_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Tertiary TDM4 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_RX_4_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Tertiary TDM5 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_TX_5_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Tertiary TDM5 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_RX_5_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Tertiary TDM6 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_TX_6_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Tertiary TDM6 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_RX_6_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Tertiary TDM7 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_TX_7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Tertiary TDM7 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "TERT_TDM_RX_7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Quaternary TDM0 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_TX_0_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Quaternary TDM0 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_RX_0_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Quaternary TDM1 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_TX_1_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Quaternary TDM1 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_RX_1_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Quaternary TDM2 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_TX_2_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Quaternary TDM2 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_RX_2_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Quaternary TDM3 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_TX_3_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Quaternary TDM3 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_RX_3_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Quaternary TDM4 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_TX_4_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Quaternary TDM4 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_RX_4_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Quaternary TDM5 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_TX_5_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Quaternary TDM5 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_RX_5_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Quaternary TDM6 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_TX_6_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Quaternary TDM6 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_RX_6_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Quaternary TDM7 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 16,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_TX_7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Quaternary TDM7 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 16,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUAT_TDM_RX_7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Quinary TDM7 Hostless Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 16,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUIN_TDM_TX_7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "Quinary TDM7 Hostless Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 16,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QUIN_TDM_RX_7_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "DTMF_RX_HOSTLESS Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =	8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "DTMF_RX_HOSTLESS",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "CPE Listen Audio capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 1,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "CPE_LSM_NOHOST",
	},
	{
		.playback = {
			.stream_name = "VOLTE_STUB Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.capture = {
			.stream_name = "VOLTE_STUB Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "VOLTE_STUB",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "VOICE2_STUB Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.capture = {
			.stream_name = "VOICE2_STUB Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "VOICE2_STUB",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia9 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000|
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =	384000,
		},
		.capture = {
			.stream_name = "MultiMedia9 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
				  SNDRV_PCM_RATE_KNOT),
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =	48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia9",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "QCHAT Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.capture = {
			.stream_name = "QCHAT Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "QCHAT",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Listen 1 Audio Service Capture",
			.rates = (SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 4,
			.rate_min = 16000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "LSM1",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Listen 2 Audio Service Capture",
			.rates = (SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 4,
			.rate_min = 16000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "LSM2",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Listen 3 Audio Service Capture",
			.rates = (SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 4,
			.rate_min = 16000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "LSM3",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Listen 4 Audio Service Capture",
			.rates = (SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 4,
			.rate_min = 16000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "LSM4",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Listen 5 Audio Service Capture",
			.rates = (SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 4,
			.rate_min = 16000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "LSM5",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Listen 6 Audio Service Capture",
			.rates = (SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 4,
			.rate_min = 16000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "LSM6",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Listen 7 Audio Service Capture",
			.rates = (SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 4,
			.rate_min = 16000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "LSM7",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "Listen 8 Audio Service Capture",
			.rates = (SNDRV_PCM_RATE_16000 |
				  SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE),
			.channels_min = 1,
			.channels_max = 4,
			.rate_min = 16000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "LSM8",
		.probe = fe_dai_probe,
	},
	/* FE DAIs created for multiple instances of offload playback */
	{
		.playback = {
			.stream_name = "MultiMedia10 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia10 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia10",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia11 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia11",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia12 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia12",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia13 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia13",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia14 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia14",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia15 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia15",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia16 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =	8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia16 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia16",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "VoiceMMode1 Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
	 },
		.capture = {
			.stream_name = "VoiceMMode1 Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "VoiceMMode1",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "VoiceMMode2 Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.capture = {
			.stream_name = "VoiceMMode2 Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "VoiceMMode2",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "MultiMedia17 Capture",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     192000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia17",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "MultiMedia18 Capture",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     192000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia18",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "MultiMedia19 Capture",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     192000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia19",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia20 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.capture = {
			.stream_name = "MultiMedia20 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia20",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia21 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia21 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia21",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia22 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia22 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia22",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia23 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia23 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia23",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia24 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia24 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia24",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia25 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia25 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 8,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia25",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia26 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia26",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "MultiMedia27 Capture",
			.rates = (SNDRV_PCM_RATE_8000_192000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 192000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia27",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
		.stream_name = "MultiMedia28 Capture",
		.rates = (SNDRV_PCM_RATE_8000_192000|
			  SNDRV_PCM_RATE_KNOT),
		.formats = (SNDRV_PCM_FMTBIT_S16_LE |
			    SNDRV_PCM_FMTBIT_S24_LE |
			    SNDRV_PCM_FMTBIT_S24_3LE),
		.channels_min = 1,
		.channels_max = 32,
		.rate_min =     8000,
		.rate_max =     192000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia28",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "MultiMedia29 Capture",
			.rates = (SNDRV_PCM_RATE_8000_192000|
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     192000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia29",
		.probe = fe_dai_probe,
	},
	{
		.capture = {
			.stream_name = "MultiMedia30 Capture",
			.rates = (SNDRV_PCM_RATE_8000_192000|
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     192000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.compress_new = msm_compr_new,
		.name = "MultiMedia30",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia31 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000|
				SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S24_3LE |
				SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.capture = {
			.stream_name = "MultiMedia31 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia31",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia32 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000|
				SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE |
				SNDRV_PCM_FMTBIT_S24_3LE |
				SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     384000,
		},
		.capture = {
			.stream_name = "MultiMedia32 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia32",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia33 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia33 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia33",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia34 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia34 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
				    SNDRV_PCM_FMTBIT_S24_LE |
				    SNDRV_PCM_FMTBIT_S24_3LE |
				    SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia34",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia35 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
					SNDRV_PCM_FMTBIT_S24_LE |
					SNDRV_PCM_FMTBIT_S24_3LE |
					SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia35 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
					SNDRV_PCM_FMTBIT_S24_LE |
					SNDRV_PCM_FMTBIT_S24_3LE |
					SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia35",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "MultiMedia36 Playback",
			.rates = (SNDRV_PCM_RATE_8000_384000 |
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
					SNDRV_PCM_FMTBIT_S24_LE |
					SNDRV_PCM_FMTBIT_S24_3LE |
					SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min = 8000,
			.rate_max = 384000,
		},
		.capture = {
			.stream_name = "MultiMedia36 Capture",
			.rates = (SNDRV_PCM_RATE_8000_48000|
					SNDRV_PCM_RATE_KNOT),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
					SNDRV_PCM_FMTBIT_S24_LE |
					SNDRV_PCM_FMTBIT_S24_3LE |
					SNDRV_PCM_FMTBIT_S32_LE),
			.channels_min = 1,
			.channels_max = 32,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_Multimedia_dai_ops,
		.name = "MultiMedia36",
		.probe = fe_dai_probe,
	},
	{
		.playback = {
			.stream_name = "CS-VOICE Playback",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.capture = {
			.stream_name = "CS-VOICE Capture",
			.rates = SNDRV_PCM_RATE_8000_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.channels_min = 1,
			.channels_max = 2,
			.rate_min =     8000,
			.rate_max =     48000,
		},
		.ops = &msm_fe_dai_ops,
		.name = "CS-VOICE",
		.probe = fe_dai_probe,
	},
};

static int msm_fe_dai_dev_probe(struct platform_device *pdev)
{

	dev_dbg(&pdev->dev, "%s: dev name %s\n", __func__,
		dev_name(&pdev->dev));
	return snd_soc_register_component(&pdev->dev, &msm_fe_dai_component,
		msm_fe_dais, ARRAY_SIZE(msm_fe_dais));
}

static int msm_fe_dai_dev_remove(struct platform_device *pdev)
{
	snd_soc_unregister_component(&pdev->dev);
	return 0;
}

static const struct of_device_id msm_dai_fe_dt_match[] = {
	{.compatible = "qcom,msm-dai-fe"},
	{}
};

static struct platform_driver msm_fe_dai_driver = {
	.probe  = msm_fe_dai_dev_probe,
	.remove = msm_fe_dai_dev_remove,
	.driver = {
		.name = "msm-dai-fe",
		.owner = THIS_MODULE,
		.of_match_table = msm_dai_fe_dt_match,
		.suppress_bind_attrs = true,
	},
};

int __init msm_fe_dai_init(void)
{
	return platform_driver_register(&msm_fe_dai_driver);
}

void msm_fe_dai_exit(void)
{
	platform_driver_unregister(&msm_fe_dai_driver);
}

/* Module information */
MODULE_DESCRIPTION("MSM Frontend DAI driver");
MODULE_LICENSE("GPL v2");
