# build RMM
cd tf-rmm
export PATH=$ROOT/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-elf/bin:$PATH
export CROSS_COMPILE=aarch64-none-elf-
#cmake -DRMM_CONFIG=fvp_defcfg -S . -B build 
cmake -DRMM_CONFIG=fvp_defcfg -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLOG_LEVEL=50 -DRMM_PLATFORM=qemu
cmake --build build
cd ../

# Build TF-A for linux
export PATH=/home/jaehyuk/cross/gcc-arm-10.3-2021.07-x86_64-aarch64-none-elf/bin:$PATH
make -C tf-a -j 40 CROSS_COMPILE=aarch64-none-elf- ARCH=aarch64 PLAT=qemu ENABLE_RME=1 DEBUG=1 \
ARM_LINUX_KERNEL_AS_BL33=1 RMM=../tf-rmm/build/Debug/rmm.img all BL33=../QEMU_EFI.fd all fip

# make flash image for qemu
rm -f flash.bin
dd if=tf-a/build/qemu/debug/bl1.bin of=flash.bin bs=4096 conv=notrunc
dd if=tf-a/build/qemu/debug/fip.bin of=flash.bin seek=64 bs=4096 conv=notrunc

