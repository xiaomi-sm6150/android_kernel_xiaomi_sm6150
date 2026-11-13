# auto-detect subdirs
ifeq ($(CONFIG_QTI_AUDIO_LAHAINA), y)
# built into the kernel from techpack/audio
LEGACY_PATH=$(srctree)/techpack/audio
CONFIG_ARCH_LAHAINA := y
else ifeq ($(QTI_TECHPACK_KERNEL_6_1), true)
LEGACY_PATH=$(PWD)/../../../../vendor/qcom/opensource/audio-kernel
else
ifeq ($(QTI_TECHPACK), true)
LEGACY_PATH=$(PWD)/../vendor/qcom/opensource/audio-kernel
else
LEGACY_PATH=$(PWD)/../vendor/qcom/opensource/audio-kernel/legacy
endif
endif
ifneq ($(CONFIG_ARCH_QTI_VM), y)
ifeq ($(CONFIG_QTI_QUIN_GVM), y)
ifeq ($(QTI_TECHPACK_KERNEL_6_1), true)
include $(LEGACY_PATH)/config/gvmautolv.conf
else
include $(LEGACY_PATH)/config/gvmauto.conf
endif
endif
ifeq ($(CONFIG_ARCH_SDXPOORWILLS), y)
include $(LEGACY_PATH)/config/sdxpoorwillsauto.conf
export
endif
ifeq ($(CONFIG_ARCH_SM8150), y)
include $(LEGACY_PATH)/config/sm8150auto.conf
export
endif
ifeq ($(CONFIG_ARCH_SDMSHRIKE), y)
include $(LEGACY_PATH)/config/sm8150auto.conf
export
endif
ifeq ($(CONFIG_ARCH_KONA), y)
include $(LEGACY_PATH)/config/konaauto.conf
endif
ifeq ($(CONFIG_ARCH_LAHAINA), y)
include $(LEGACY_PATH)/config/lahainaauto.conf
endif
ifeq ($(CONFIG_ARCH_HOLI), y)
include $(LEGACY_PATH)/config/holiauto.conf
endif
ifeq ($(CONFIG_ARCH_SA8155), y)
include $(LEGACY_PATH)/config/sa8155auto.conf
endif
ifeq ($(CONFIG_ARCH_SA6155), y)
include $(LEGACY_PATH)/config/sa6155auto.conf
endif
ifeq ($(CONFIG_ARCH_SM6150), y)
include $(LEGACY_PATH)/config/sm6150auto.conf
endif
endif
# Use USERINCLUDE when you must reference the UAPI directories only.
USERINCLUDE     += \
                -I$(LEGACY_PATH)/include/uapi

# Use LINUXINCLUDE when you must reference the include/ directory.
# Needed to be compatible with the O= option
LINUXINCLUDE    += \
                -I$(LEGACY_PATH)/include/uapi \
                -I$(LEGACY_PATH)/include/asoc \
                -I$(LEGACY_PATH)/include

ifeq ($(CONFIG_QTI_QUIN_GVM), y)
ifeq ($(QTI_TECHPACK_KERNEL_6_1), true)
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/gvmautolvconf.h
else
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/gvmautoconf.h
endif
endif
ifeq ($(CONFIG_ARCH_SDXPOORWILLS), y)
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/sdxpoorwillsautoconf.h
endif
ifeq ($(CONFIG_ARCH_SM8150), y)
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/sm8150autoconf.h
endif
ifeq ($(CONFIG_ARCH_SDMSHRIKE), y)
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/sm8150autoconf.h
endif
ifeq ($(CONFIG_ARCH_KONA), y)
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/konaautoconf.h
endif
ifeq ($(CONFIG_ARCH_LAHAINA), y)
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/lahainaautoconf.h
endif
ifeq ($(CONFIG_ARCH_HOLI), y)
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/holiautoconf.h
endif
ifeq ($(CONFIG_ARCH_SA8155), y)
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/sa8155autoconf.h
endif
ifeq ($(CONFIG_ARCH_SA6155), y)
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/sa6155autoconf.h
endif
ifeq ($(CONFIG_ARCH_SM6150), y)
LINUXINCLUDE    += \
                -include $(LEGACY_PATH)/config/sm6150autoconf.h
endif
obj-y += soc/
obj-y += dsp/
obj-y += ipc/
obj-y += asoc/

M=$(PWD)
AUDIO_ROOT=$(KERNEL_SRC)/$(M)

KBUILD_OPTIONS+=  AUDIO_ROOT=$(AUDIO_ROOT)

all: modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)
