# SPDX-License-Identifier: GPL-2.0-only
# Qualcomm builds this out of tree with a per-target
# config header (config/gki_bengaltouchconf.h); mirror that here.
ccflags-y += -DCONFIG_TOUCHSCREEN_NT36XXX_LATE_EN=1

nt36xxx-i2c-y := nt36xxx.o nt36xxx_fw_update.o nt36xxx_ext_proc.o nt36xxx_mp_ctrlram.o
obj-$(CONFIG_TOUCHSCREEN_NT36XXX_I2C) += nt36xxx-i2c.o
