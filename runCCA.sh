#  ./qemu/build/qemu-system-aarch64 -s -S -nographic -serial telnet::54340,server \
#  -smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3,iommu=smmuv3,dumpdtb=qemu.dtb \
#  -m 2048 -cpu max,lpa2=off -d unimp,guest_errors  -D qemu.log -kernel out/bin/Image -bios flash.bin \
#  -initrd initrd -device virtio-gpu-pci,bus=pcie.0
#
#  dtc -I dtb -O dts -o qemu.dts qemu.dtb
#

sudo ./qemu/build/qemu-system-aarch64 -s -S -nographic \
-serial telnet::6000,server,nowait \
-smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3,iommu=smmuv3 \
-m 2048 -cpu max,lpa2=off \
-d unimp,guest_errors \
-initrd initrd \
-hda disk.qcow2 -append "root=/dev/vda1" \
-kernel ./nw-linux/arch/arm64/boot/Image \
-bios flash.bin 
#
#-serial telnet::6001,server,nowait \
#-serial telnet::6002,server,nowait \
#
#-netdev user,id=net0 -device e1000,netdev=net0 \
#-netdev user,id=net1 -device virtio-net-pci,netdev=net1 \
#-smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3,iommu=smmuv3 \


#-drive file=blknvme,if=none,id=nvm \
#-device nvme,serial=deadbeef,drive=nvm 
#-device virtio-gpu-pci,bus=pcie.0 \
#-drive file=./rootfs-target.ext4,if=none,format=raw,id=hd0 -device virtio-blk-device,#drive=hd0 \



#  ./qemu/build/qemu-system-aarch64 -s -S -nographic -serial telnet::54340,server \
#  -smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3,iommu=smmuv3 \
#  -m 2048 -cpu max,lpa2=off -d unimp,guest_errors  -D qemu.log -kernel out/bin/Image -bios flash.bin \
#  -device virtio-gpu-pci,bus=pcie.0

#./qemu/build/qemu-system-aarch64 -s -S -nographic -serial telnet::54340,server \
#  -smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3 \
#  -m 2048 -cpu max,lpa2=off -d unimp,guest_errors -D qemu.log \
#  -kernel Image -bios flash.bin -initrd rootfs.cpio.gz
#

#./qemu/build/qemu-system-aarch64 -s -S -nographic -serial telnet::54340,server \
#  -smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3 \
#  -m 2048 -cpu max,lpa2=off -d unimp,guest_errors -D qemu.log \
#  -kernel Image -bios flash.bin -initrd rootfs.cpio.gz
#
#./qemu-cca/build/qemu-system-aarch64 -s -S -nographic -serial telnet::54340,server \
#  -smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3 \
#  -m 2048 -cpu max,lpa2=off -d unimp,guest_errors -D qemu.log \
#  -bios flash.bin
#./qemu/build/qemu-system-aarch64 -s -S -nographic -serial telnet::54340,server \
#  -smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3 \
#  -m 2048 -cpu max,lpa2=off -d unimp,guest_errors -D qemu.log \
#  -bios flash.bin
