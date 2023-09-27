ROOT=$(pwd)
NUM_PROC=$(nproc)
make -C $ROOT/realm/linux

#build kvmtool
cd kvmtool 
make clean
make ARCH=arm64 CROSS_COMPILE=$ROOT/toolchain/aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu- LIBFDT_DIR=./dtc/libfdt lkvm
cd ../


#mount virtual disk for realm
rm disk.qcow2
qemu-img create -f qcow2 disk.qcow2 2G
sudo modprobe nbd
sudo qemu-nbd -c /dev/nbd0 ./disk.qcow2; 
echo -e "n\np\n1\n\n\nw" | sudo fdisk /dev/nbd0
sudo mkfs.ext4 /dev/nbd0p1
sudo mount /dev/nbd0p1 /mnt/guest

#copy needed script & image to qcow
sudo cp $ROOT/realm-linux/arch/arm64/boot/Image /mnt/guest/realm-linux
sudo cp $ROOT/kvmtool/lkvm /mnt/guest/
sudo cp $ROOT/launch-realm.sh /mnt/guest/
sudo cp $ROOT/launch-no-realm.sh /mnt/guest/
sudo cp $ROOT/rootfs-realm.cpio.gz /mnt/guest/
cat /mnt/guest/launch-realm.sh

#unmount
ls /mnt/guest
cd $ROOT
sudo umount /mnt/guest; sudo qemu-nbd -d /dev/nbd0
