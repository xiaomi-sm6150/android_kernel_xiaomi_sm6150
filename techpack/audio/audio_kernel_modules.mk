# Build audio kernel driver

AUDIO_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/native_dlkm.ko \
	$(KERNEL_MODULES_OUT)/apr_dlkm.ko \
	$(KERNEL_MODULES_OUT)/platform_dlkm.ko

ifneq (,$(filter $(TARGET_BOARD_PLATFORM), msmnile $(MSMSTEPPE)))
AUDIO_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/q6_notifier_dlkm_legacy.ko\
	$(KERNEL_MODULES_OUT)/q6_dlkm_legacy.ko \
	$(KERNEL_MODULES_OUT)/adsp_loader_dlkm_legacy.ko \
	$(KERNEL_MODULES_OUT)/snd_event_dlkm_legacy.ko \
	$(KERNEL_MODULES_OUT)/stub_dlkm_legacy.ko \
	$(KERNEL_MODULES_OUT)/hdmi_dlkm_legacy.ko \
	$(KERNEL_MODULES_OUT)/machine_dlkm_legacy.ko
else

AUDIO_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/q6_notifier_dlkm.ko\
	$(KERNEL_MODULES_OUT)/q6_dlkm.ko \
	$(KERNEL_MODULES_OUT)/adsp_loader_dlkm.ko \
	$(KERNEL_MODULES_OUT)/snd_event_dlkm.ko \
	$(KERNEL_MODULES_OUT)/stub_dlkm.ko \
	$(KERNEL_MODULES_OUT)/hdmi_dlkm.ko \
	$(KERNEL_MODULES_OUT)/machine_dlkm.ko
endif
