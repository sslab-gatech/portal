NUM_PROC=$(nproc)
ROOT=$PWD
# build RMM
cd ./tf-rmm
rm ./build -rf 
export CROSS_COMPILE=aarch64-none-elf-
export PATH=$ROOT/toolchain/aarch64-none-elf/bin:$PATH
cmake -DRMM_CONFIG=qemu_defcfg -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLOG_LEVEL=50 -DRMM_PLATFORM=qemu
cmake --build build
cd ../

# Build TF-A for linux
cd ./tf-a
export PATH=$ROOT/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-elf/bin:$PATH
make -j $NUM_PROC CROSS_COMPILE=aarch64-none-elf- ARCH=aarch64 PLAT=qemu ENABLE_RME=1 DEBUG=1 \
ARM_LINUX_KERNEL_AS_BL33=1 RMM=../tf-rmm/build/Debug/rmm.img all BL33=../QEMU_EFI.fd all fip
cd ../

# make flash image for qemu
rm -f flash.bin
dd if=tf-a/build/qemu/debug/bl1.bin of=flash.bin bs=4096 conv=notrunc
dd if=tf-a/build/qemu/debug/fip.bin of=flash.bin seek=64 bs=4096 conv=notrunc
