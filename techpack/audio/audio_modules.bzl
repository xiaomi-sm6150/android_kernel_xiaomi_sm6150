load(":module_mgr.bzl", "create_module_registry")

DSP_PATH = "dsp"
IPC_PATH = "ipc"
SOC_PATH = "soc"
ASOC_PATH = "asoc"
DSP_CODECS_PATH = DSP_PATH + "/codecs"
ASOC_CODECS_PATH = ASOC_PATH + "/codecs"
ASOC_CODECS_WCD9330_PATH = ASOC_CODECS_PATH + "/wcd9330"
ASOC_CODECS_LPASS_CDC_PATH = ASOC_CODECS_PATH + "/lpass-cdc"
ASOC_CODECS_BOLERO_PATH = ASOC_CODECS_PATH + "/bolero"

audio_modules = create_module_registry([":audio_headers"])
# ------------------------------------ AUDIO MODULE DEFINITIONS ---------------------------------
# >>>> DSP MODULES <<<<
audio_modules.register(
    name = "q6_dlkm",
    path = DSP_PATH,
    conditional_srcs = {
        "CONFIG_SND_SOC_MSM_QDSP6V2_INTF": [
            "msm-audio-event-notify.c",
            "q6_init.c",
            "audio_calibration.c",
            "audio_cal_utils.c",
            "q6adm.c",
            "q6afe.c",
            "q6asm.c",
            "q6audio-v2.c",
            "q6voice.c",
            "q6core.c",
            "q6common.c",
            "rtac.c",
            "q6lsm.c",
            "adsp_err.c",
            "msm_audio_ion.c",
        ],
        "CONFIG_SND_SOC_MSM_QDSP6V2_VM": [
            "msm-audio-event-notify.c",
            "msm_audio_ion_vm.c",
            "q6_init.c",
        ],
        "CONFIG_MSM_AVTIMER": [
            "avtimer.c"
        ],
        "CONFIG_XT_LOGGING": [
            "sp_params.c"
        ],
        "CONFIG_DIGITAL_CDC_RSC_MGR": [
            "digital-cdc-rsc-mgr.c"
        ],
        "CONFIG_WCD9XXX_CODEC_CORE": [
            "audio_slimslave.c"
        ]
    }
)

audio_modules.register(
    name = "platform_dlkm",
    path = ASOC_PATH,
    conditional_srcs = {
         "CONFIG_SND_SOC_QDSP6V2": [
            "msm-audio-effects-q6-v2.c",
            "msm-compress-q6-v2.c",
            "msm-common.c",
            "msm-dai-fe.c",
            "msm-dai-q6-hdmi-v2.c",
            "msm-dai-q6-v2.c",
            "msm-dai-stub-v2.c",
            "msm-lsm-client.c",
            "msm-pcm-afe-v2.c",
            "msm-pcm-dtmf-v2.c",
            "msm-pcm-hostless.c",
            "msm-pcm-loopback-v2.c",
            "msm-pcm-q6-v2.c",
            "msm-pcm-host-voice-v2.c",
            ],
         "CONFIG_QTI_PP": [
            "msm-qti-pp-config.c",
          ],
         "CONFIG_BOARD_AUTO_AUDIO": {
                True: [
                    "msm-pcm-routing-auto.c",
                    "platform_init_auto.c",
                ],
                False: [
                    "msm-pcm-routing-v2.c",
                    "msm-pcm-voice-v2.c",
                    "msm-pcm-voip-v2.c",
                    "platform_init.c",
                ]
         },

    }
)




audio_modules.register(
    name = "native_dlkm",
    path = DSP_CODECS_PATH,
    config_option = "CONFIG_MSM_QDSP6V2_CODECS",
    srcs = [
        "q6audio_v2.c",
        "q6audio_v2_aio.c",
        "audio_utils_aio.c",
        "audio_utils.c",
        "audio_native.c",
        "aac_in.c",
        "amrnb_in.c",
        "amrwb_in.c",
        "audio_aac.c",
        "audio_alac.c",
        "audio_amrnb.c",
        "audio_amrwb.c",
        "audio_amrwbplus.c",
        "audio_ape.c",
        "audio_evrc.c",
        "audio_g711alaw.c",
        "audio_g711mlaw.c",
        "audio_hwacc_effects.c",
        "audio_mp3.c",
        "audio_multi_aac.c",
        "audio_qcelp.c",
        "audio_wma.c",
        "audio_wmapro.c",
        "evrc_in.c",
        "g711alaw_in.c",
        "g711mlaw_in.c",
        "qcelp_in.c"
],
)

audio_modules.register(
    name = "q6_notifier_dlkm",
    path = DSP_PATH,
    config_option = "CONFIG_MSM_QDSP6_NOTIFIER",
    srcs = [
        "audio_notifier.c",
        "audio_ssr.c"
    ],
)
audio_modules.register(
    name = "apr_dlkm",
    path = IPC_PATH,
    conditional_srcs = {
        "CONFIG_MSM_QDSP6_APRV2_RPMSG": [
            "apr.c",
            "apr_v2.c",
            "apr_tal_rpmsg.c",
        ],
        "CONFIG_MSM_QDSP6_APRV3_RPMSG": [
            "apr.c",
            "apr_v3.c",
            "apr_tal_rpmsg.c"
        ],
        "CONFIG_MSM_QDSP6_APRV2_VM": [
            "apr_vm.c",
            "apr_v2.c"
        ]
    },
)

audio_modules.register(
    name = "adsp_loader_dlkm",
    path = DSP_PATH,
    config_option = "CONFIG_MSM_ADSP_LOADER",
    srcs = ["adsp-loader.c"],
)

# >>>> SOC MODULES <<<<
audio_modules.register(
    name = "swr_dlkm",
    path = SOC_PATH,
    config_option = "CONFIG_SOUNDWIRE",
    srcs = [
        "regmap-swr.c",
        "soundwire.c"
    ]
)
audio_modules.register(
    name = "swr_ctrl_dlkm",
    path = SOC_PATH,
    conditional_srcs = {
        "CONFIG_SOUNDWIRE_WCD_CTRL": [
            "swr-wcd-ctrl.c"
        ],
        "CONFIG_SOUNDWIRE_MSTR_CTRL": [
            "swr-mstr-ctrl.c"
        ]
    },
)
audio_modules.register(
    name = "snd_event_dlkm",
    path = SOC_PATH,
    config_option = "CONFIG_SND_EVENT",
    srcs = ["snd_event.c"]
)

audio_modules.register(
    name = "pinctrl_wcd_dlkm",
    path = SOC_PATH,
    config_option = "CONFIG_PINCTRL_WCD",
    srcs = ["pinctrl-wcd.c"]
)

# >>>> ASOC MODULES <<<<
audio_modules.register(
    name = "machine_dlkm",
    path = ASOC_PATH,
    conditional_srcs = {
        "CONFIG_SND_SOC_SM8150": [
            "sm8150.c",
            "machine_815x_init.c"
        ],
        "CONFIG_SND_SOC_SM6150": [
            "sm6150.c",
            "machine_615x_init.c"
        ],
        "CONFIG_SND_SOC_SA6155": [
            "sa6155.c"
        ],
        "CONFIG_SND_SOC_QCS405": [
            "qcs405.c"
        ],
        "CONFIG_SND_SOC_KONA": [
            "kona.c"
        ],
        "CONFIG_SND_SOC_LAHAINA": [
            "lahaina.c"
        ],
        "CONFIG_SND_SOC_WAIPIO": [
            "waipio.c"
        ],
        "CONFIG_SND_SOC_KALAMA": [
            "kalama.c"
        ],
        "CONFIG_SND_SOC_PINEAPPLE": [
            "pineapple.c"
        ],
        "CONFIG_SND_SOC_HOLI": [
            "holi.c"
        ],
        "CONFIG_SND_SOC_LITO": [
            "kona.c"
        ],
        "CONFIG_SND_SOC_BENGAL": [
            "bengal.c"
        ],
        "CONFIG_SND_SOC_SA8155": [
            "sa8155.c"
        ],
        "CONFIG_SND_SOC_SDX": [
            "sdx-target.c"
        ],
        "CONFIG_SND_SOC_MDM9607":[
            "mdm9607.c"
        ]
    },
)
# >>>> ASOC/CODEC MODULES <<<<
audio_modules.register(
    name = "wcd_core_dlkm",
    path = ASOC_CODECS_PATH,
    conditional_srcs = {
        "CONFIG_WCD9XXX_CODEC_CORE": [
            "wcd9xxx-rst.c",
            "wcd9xxx-core-init.c",
            "wcd9xxx-core.c",
            "wcd9xxx-irq.c",
            "wcd9xxx-slimslave.c",
            "wcd9xxx-utils.c",
            "wcd9335-regmap.c",
            "wcd9335-tables.c",
            "msm-cdc-pinctrl.c",
            "msm-cdc-supply.c",
            "wcd934x/wcd934x-regmap.c",
            "wcd934x/wcd934x-tables.c",
            "wcd9330/wcd9330-regmap.c",
            "wcd9330/wcd9330-tables.c",
        ],
        "CONFIG_WCD9XXX_CODEC_CORE_V2": [
            "wcd9xxx-core-init.c",
            "msm-cdc-pinctrl.c",
            "msm-cdc-supply.c",
        ],
        "CONFIG_SND_SOC_WCD_IRQ": [
            "wcd-irq.c"
        ]
    }
)

audio_modules.register(
    name = "mbhc_dlkm",
    path = ASOC_CODECS_PATH,
    config_option = "CONFIG_SND_SOC_WCD_MBHC",
    srcs = ["wcd-mbhc-v2.c"],
    conditional_srcs = {
        "CONFIG_SND_SOC_WCD_MBHC_ADC": [
            "wcd-mbhc-adc.c"
        ],
        "CONFIG_SND_SOC_WCD_MBHC_LEGACY": [
            "wcd-mbhc-legacy.c"
        ]
    },
)
audio_modules.register(
    name = "wcd9xxx_dlkm",
    path = ASOC_CODECS_PATH,
    config_option = "CONFIG_SND_SOC_WCD9XXX_V2",
    srcs = [
        "wcdcal-hwdep.c",
        "wcd9xxx-soc-init.c",
        "audio-ext-clk-up.c"
    ],
    conditional_srcs = {
        "CONFIG_WCD9XXX_CODEC_CORE": {
            True: [
                "wcd9xxx-common-v2.c",
                "wcd9xxx-resmgr-v2.c",
                "wcd-dsp-utils.c",
            ],
            False: [
                "wcd-clsh.c"
            ]
        },
        "CONFIG_SND_SOC_WCD9330": [
                "wcd9xxx-common.c",
                "wcd9xxx-resmgr.c",
                "wcd9xxx-mbhc.c"
        ]
    }
)
audio_modules.register(
    name = "stub_dlkm",
    path = ASOC_CODECS_PATH,
    config_option = "CONFIG_SND_SOC_MSM_STUB",
    srcs = ["msm_stub.c"]
)
audio_modules.register(
    name = "wcd9330_dlkm",
    path = ASOC_CODECS_WCD9330_PATH,
    config_option = "CONFIG_SND_SOC_WCD9330",
    srcs = ["wcd9330.c"]
)

