# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(srctree)/$(src) -DDSP_TRACE_INCLUDE_PATH=.

frpc-adsprpc-y := adsprpc.o adsprpc_rpmsg.o
frpc-adsprpc-$(CONFIG_COMPAT) += adsprpc_compat.o

obj-$(CONFIG_MSM_ADSPRPC) += frpc-adsprpc.o
