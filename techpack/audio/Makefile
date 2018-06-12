# Use USERINCLUDE when you must reference the UAPI directories only.
USERINCLUDE     += \
                -I$(srctree)/techpack/audio/include/uapi \

# Use LINUXINCLUDE when you must reference the include/ directory.
# Needed to be compatible with the O= option
LINUXINCLUDE    += \
                -I$(srctree)/techpack/audio/include/uapi \
                -I$(srctree)/techpack/audio/include

#for mius start
ifeq ($(CONFIG_US_PROXIMITY), y)
LINUXINCLUDE    += \
                -I$(srctree)/techpack/audio/include/mius
endif
#for mius end

obj-y += soc/
obj-y += dsp/
#for mius start
ifeq ($(CONFIG_US_PROXIMITY), y)
obj-y += dsp/mius
endif
#for mius end
obj-y += ipc/
obj-y += asoc/
