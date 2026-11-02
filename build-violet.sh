#!/bin/bash
# Kernel-only build for violet bring-up.
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$SRC/../out-violet}
CLANG=${CLANG:-$SRC/../toolchain/clang-r487747c/bin}
export KBUILD_BUILD_HOST=${KBUILD_BUILD_HOST:-localhost}
export PATH=$CLANG:$PATH
M="make -C $SRC O=$OUT ARCH=arm64 LLVM=1 LLVM_IAS=1"
mkdir -p "$OUT"
$M gki_defconfig
if [ -f "$SRC/arch/arm64/configs/vendor/violet.config" ]; then
  $SRC/scripts/kconfig/merge_config.sh -m -O "$OUT" "$OUT/.config" \
    "$SRC/arch/arm64/configs/vendor/violet.config" >/dev/null
fi
$M olddefconfig
nice -n 19 $M -j${JOBS:-32} Image.gz dtbs
