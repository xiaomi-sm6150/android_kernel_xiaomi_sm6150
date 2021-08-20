#!/bin/bash

kernel_dir="${PWD}"
CCACHE=$(command -v ccache)
HOME=/home/rasenkai
objdir="${kernel_dir}/out"
anykernel=$HOME/kernel/x2/anykernel
kernel_name="Requiem-Nightly"
export ARCH="arm64"
export KBUILD_BUILD_HOST="Requiem"
export KBUILD_BUILD_USER="Rasenkai"

PATH=/home/rasenkai/kernel/toolchain/aospclang/bin/:$PATH

# Colors
NC='\033[0m'
RED='\033[0;31m'
LRD='\033[1;31m'
LGR='\033[1;32m'

make_defconfig()
{
     START=$(date +"%s")
	echo -e "${LGR}" "############### Cleaning ################${NC}"
    rm $anykernel/dtb
    rm $anykernel/dtbo.img
    rm $anykernel/Image.gz-dtb
    rm $anykernel/Image.gz
    rm -rf $kernel_dir/out/arch/arm64/boot/Image.gz
    rm -rf $kernel_dir/out/arch/arm64/boot/Image.gz-dtb

	echo -n "Choose Device Defconfig :"
	read -r CONFIG_FILE
	echo -n "Appended? y/n:"
	read -r Appended
	if [ "$CONFIG_FILE" = "phoenix_defconfig" ]
	then
	zip_name="$kernel_name-$(date +"%d%m%Y")-PHOENIX.zip"
	else
	zip_name="$kernel_name-$(date +"%d%m%Y")-SWEET.zip"
	fi
	
	echo -e "${LGR}" "########### Generating Defconfig ############${NC}"
    make -s ARCH=${ARCH} O="${objdir}" "${CONFIG_FILE}" -j$(nproc --all)
}
compile()
{
	cd "${kernel_dir}"
	echo -e "${LGR}" "######### Compiling kernel #########${NC}"
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
        if [ "$CONFIG_FILE" = "sweet_defconfig" ]
        then
        cd "${objdir}"
        cd arch/arm64/boot/
        curl https://android.googlesource.com/platform/external/avb/+/refs/heads/master/avbtool.py?format=TEXT | base64 --decode > avbtool.py
        python3 avbtool.py add_hash_footer --image dtbo.img --partition_size=33554432 --partition_name dtbo
        fi
        cd "${objdir}"
	if [ "$Appended" = "y" ]
	then
	COMPILED_IMAGE=arch/arm64/boot/Image.gz-dtb
	else
	COMPILED_IMAGE=arch/arm64/boot/Image.gz
	fi
	COMPILED_DTBO=arch/arm64/boot/dtbo.img
	if [ "$Appended" = "n" ]
	then
        COMPILED_DTB=arch/arm64/boot/dts/qcom/*.dtb
	mv -f ${COMPILED_DTB} $anykernel
        fi
	if [[ -f ${COMPILED_IMAGE} && ${COMPILED_DTBO} ]]; then
		mv -f ${COMPILED_IMAGE} ${COMPILED_DTBO} $anykernel
        cd $anykernel
        if [ "$Appended" = "n" ]
        then
        mv *.dtb dtb
        fi
        find . -name "*.zip" -type f
        find . -name "*.zip" -type f -delete
        zip -r AnyKernel.zip *
        mv AnyKernel.zip "$zip_name"
        mv $anykernel/"$zip_name" $HOME/Desktop/"$zip_name"
        END=$(date +"%s")
        DIFF=$(($END - $START))
		echo -e "${LGR}" "############################################"
		echo -e "${LGR}" "############# OkThisIsEpic!  ##############"
		echo -e "${LGR}" "############################################${NC}"
	else
		echo -e "${RED}" "############################################"
		echo -e "${RED}" "##         This Is Not Epic :'(           ##"
		echo -e "${RED}" "############################################${NC}"
	fi
}
make_defconfig
compile
completion
cd "${kernel_dir}"
