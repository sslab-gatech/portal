NUM_PROC=$(nproc)
ROOT=$PWD
## Build TF-A for linux
cd tf-a
export PATH=$ROOT/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-elf/bin:$PATH; 
make -j $NUM_PROC CROSS_COMPILE=aarch64-none-elf- ARCH=aarch64 PLAT=qemu ENABLE_RME=1 DEBUG=1 \
ARM_LINUX_KERNEL_AS_BL33=1 RMM=../tf-rmm/build/Debug/rmm.img all BL33=../QEMU_EFI.fd all fip
cd ../

# Build nw-linux
cp ./linux-mk-scripts/common-nw.mk ./optee-build/common.mk
cd optee-build
make -j $NUM_PROC -f qemu_v8.mk linux
cd ../


# make flash image for qemu
rm -f flash.bin
dd if=tf-a/build/qemu/debug/bl1.bin of=flash.bin bs=4096 conv=notrunc
dd if=tf-a/build/qemu/debug/fip.bin of=flash.bin seek=64 bs=4096 conv=notrunc
