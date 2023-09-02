ROOT=$PWD
NUM_PROC=$(nproc)
#build realm-linux
cp ./linux-mk-scripts/common-realm.mk ./optee-build/common.mk
cd optee-build
make -j $NUM_PROC -f qemu_v8.mk linux
cd ../

#build kvmtool
cd kvmtool 
make ARCH=arm64 CROSS_COMPILE=$ROOT/toolchain/aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu- LIBFDT_DIR=./dtc/libfdt lkvm
cd ../


sudo modprobe nbd
sudo qemu-nbd -c /dev/nbd0 ./disk.qcow2
sudo mount /dev/nbd0p1 /mnt/guest

#copy needed script & image to qcow
sudo rm -rf /mnt/guest/*
sudo cp ./realm-linux/arch/arm64/boot/Image /mnt/guest/realm-linux
sudo cp ./kvmtool/lkvm /mnt/guest
sudo cp ./launch-realm.sh /mnt/guest
sudo cp ./rootfs-realm.cpio.gz /mnt/guest

ls /mnt/guest
sudo umount /mnt/guest
sudo qemu-nbd -d /dev/nbd0

