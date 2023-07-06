rm qemu/build/qemu-system-aarch64
rm -rf tf-a-tests/build
rm -rf tf-rmm/build
rm -rf tf-a/build 
rm ./QEMU_EFI.fd
rm -rf edk2_build/Build

ROOT=$PWD

# build qemu
mkdir -p ./qemu/build
cd ./qemu/build
../configure --target-list=aarch64-softmmu --disable-docs 
make -j 48
cd ../../

# build RMM
cd tf-rmm
export PATH=$ROOT/toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-elf/bin:$PATH
export CROSS_COMPILE=aarch64-none-elf- 
#cmake -DRMM_CONFIG=fvp_defcfg -S . -B build 
cmake -DRMM_CONFIG=fvp_defcfg -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLOG_LEVEL=50 -DRMM_PLATFORM=qemu
cmake --build build
cd ../

# build tf-a tests
#export PATH=/home/jaehyuk/cross/gcc-arm-10.3-2021.07-x86_64-aarch64-none-elf/bin:$PATH
#make -C tf-a-tests -j 40 CROSS_COMPILE=aarch64-none-elf- PLAT=qemu DEBUG=1 LOG_LEVEL=50 TESTS=realm-payload pack_realm 

## build edk2
cd ./edk2_build
./build.sh
cd ../

# Build buildroot
cd buildroot
make qemu_aarch64_virt_defconfig
utils/config -e BR2_TARGET_ROOTFS_CPIO
utils/config -e BR2_TARGET_ROOTFS_CPIO_GZIP
make olddefconfig
make
cp ./output/images/rootfs.cpio.gz  ../
cd ../


# Build TF-A for linux
export PATH=/home/jaehyuk/cross/gcc-arm-10.3-2021.07-x86_64-aarch64-none-elf/bin:$PATH
make -C tf-a -j 40 CROSS_COMPILE=aarch64-none-elf- ARCH=aarch64 PLAT=qemu ENABLE_RME=1 ENABLE_PORTAL=1 DEBUG=1 \
ARM_LINUX_KERNEL_AS_BL33=1 RMM=../tf-rmm/build/Debug/rmm.img all BL33=../QEMU_EFI.fd all fip

# Build TF-A for tf-a-tests
#export PATH=/home/jaehyuk/cross/gcc-arm-10.3-2021.07-x86_64-aarch64-none-elf/bin:$PATH
#make -C tf-a -j 40 CROSS_COMPILE=aarch64-none-elf- ARCH=aarch64 PLAT=qemu ENABLE_RME=1 DEBUG=1 \
#RMM=../tf-rmm/build/Debug/rmm.img all BL33=../tf-a-tests/build/qemu/debug/tftf.bin all fip

# make flash image for qemu
rm -f flash.bin
dd if=tf-a/build/qemu/debug/bl1.bin of=flash.bin bs=4096 conv=notrunc
dd if=tf-a/build/qemu/debug/fip.bin of=flash.bin seek=64 bs=4096 conv=notrunc

# Build linux
cd optee-build
make -j 48 -f qemu_v8.mk linux
cd ../


