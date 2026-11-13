load(":audio_modules.bzl", "audio_modules")
load(":module_mgr.bzl", "define_target_modules")

def define_mdm9607():
     define_target_modules(
        target = "mdm9607",
        variants = ["debug-defconfig", "perf-defconfig"],
        registry = audio_modules,
        modules = [
            "q6_dlkm",
            "q6_notifier_dlkm",
            "adsp_loader_dlkm",
            "apr_dlkm",
            "swr_dlkm",
            "swr_ctrl_dlkm",
            "snd_event_dlkm",
            "machine_dlkm",
            "native_dlkm",
            "wcd_core_dlkm",
            "mbhc_dlkm",
            "wcd9xxx_dlkm",
            "wcd9330_dlkm",
            "stub_dlkm",
            "platform_dlkm",
            "pinctrl_wcd_dlkm",
        ],
        config_options = [
            "CONFIG_SND_SOC_MDM9607",
            "CONFIG_SND_SOC_MSM_QDSP6V2_INTF",
            "CONFIG_SOUNDWIRE_WCD_CTRL",
            "CONFIG_WCD9XXX_CODEC_CORE",
            "CONFIG_MSM_CDC_PINCTRL",
            "CONFIG_SND_SOC_WCD9XXX_V2",
            "CONFIG_SND_SOC_WCD_MBHC",
            "CONFIG_SND_SOC_WCD_MBHC_ADC",
            "CONFIG_SND_SOC_QDSP6V2",
            "CONFIG_MSM_QDSP6V2_CODECS",
            "CONFIG_MSM_QDSP6_APRV3_RPMSG",
            "CONFIG_MSM_ADSP_LOADER",
            "CONFIG_SOUNDWIRE",
            "CONFIG_SND_EVENT",
            "CONFIG_SND_SOC_MSM_STUB",
            "CONFIG_SND_SOC_WCD9330",
            "CONFIG_QTI_PP",
            "CONFIG_MSM_QDSP6_SSR",
        ]
    )
