#!/bin/bash

kernel_dir="${PWD}"
CCACHE=$(command -v ccache)
HOME=/home/rasenkai
objdir="${kernel_dir}/out"
anykernel=$HOME/kernel/asus/anykernel
builddir="${kernel_dir}/build"
ZIMAGE=$kernel_dir/out/arch/arm64/boot/Image.gz
kernel_name="Requiem-Nightly"
zip_name="$kernel_name-$(date +"%d%m%Y").zip"
GCC_DIR=$HOME/kernel/toolchain/gcc9
export CONFIG_FILE="phoenix_defconfig"
export ARCH="arm64"
export KBUILD_BUILD_HOST="TheWorld"
export KBUILD_BUILD_USER="BuriBuriZaemon"

PATH=/home/rasenkai/kernel/toolchain/aospclang/bin/:$PATH

# Colors
NC='\033[0m'
RED='\033[0;31m'
LRD='\033[1;31m'
LGR='\033[1;32m'

make_defconfig()
{
	# Needed to make sure we get dtb built and added to kernel image properly
     START=$(date +"%s")
	echo -e ${LGR} "############### Cleaning ################${NC}"
    rm $anykernel/Image.gz
    rm -rf $ZIMAGE

	echo -e ${LGR} "########### Generating Defconfig ############${NC}"
    make -s ARCH=${ARCH} O=${objdir} ${CONFIG_FILE} -j$(nproc --all)
}
compile()
{
	cd ${kernel_dir}
	echo -e ${LGR} "######### Compiling kernel #########${NC}"
	make -j$(nproc --all) O=out \
                      ARCH=${ARCH}\
                      CC="ccache clang" \
	                CLANG_TRIPLE="aarch64-linux-gnu-" \
	                CROSS_COMPILE="aarch64-linux-gnu-" \
	                CROSS_COMPILE_ARM32="arm-linux-gnueabi-" \
	                -j4
}

completion() 
{
	cd ${objdir}
	COMPILED_IMAGE=arch/arm64/boot/Image.gz
	COMPILED_DTBO=arch/arm64/boot/dtbo.img
	if [[ -f ${COMPILED_IMAGE} && ${COMPILED_DTBO} ]]; then
		mv -f $ZIMAGE ${COMPILED_DTBO} $anykernel
        cd $anykernel
        find . -name "*.zip" -type f
        find . -name "*.zip" -type f -delete
        zip -r AnyKernel.zip *
        mv AnyKernel.zip $zip_name
        mv $anykernel/$zip_name $HOME/Desktop/$zip_name
        END=$(date +"%s")
        DIFF=$(($END - $START))
		echo -e ${LGR} "############################################"
		echo -e ${LGR} "############# OkThisIsEpic!  ##############"
		echo -e ${LGR} "############################################${NC}"
	else
		echo -e ${RED} "############################################"
		echo -e ${RED} "##         This Is Not Epic :'(           ##"
		echo -e ${RED} "############################################${NC}"
	fi
}
make_defconfig
compile
completion
cd ${kernel_dir}
