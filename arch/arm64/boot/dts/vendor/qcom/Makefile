# SPDX-License-Identifier: GPL-2.0
dtb-$(CONFIG_ARCH_QCOM) += violet-sm6150.dtb
dtb-$(CONFIG_MACH_XIAOMI_SWEET) += sweet-sdmmagpie.dtb

# The violet ABL hangs before the kernel on a DTB of 381,880 bytes or more
# (4.14's 630 KB one passes; the mechanism is unknown). A full __symbols__
# table pushes this DTB past that, so build without -@: violet-sm6150.dts
# carries the only symbol the dtbo overlay needs (soc).
DTC_NO_SYMBOLS := y
