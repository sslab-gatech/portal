./qemu/build/qemu-system-aarch64 -s -S -nographic -serial telnet::54340,server \
  -smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3,iommu=smmuv3 \
  -m 2048 -cpu max,lpa2=off -d unimp,guest_errors \
  -device virtio-gpu-pci \
  -kernel out/bin/Image -initrd initrd -bios flash.bin \
#  -hda ./image.qcow2 \
  -device nvme,drive=./image.qcow2,serial=deadbeaf1,num_queues=8 
  #-drive file=./image.qcow2,format=ext4\

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
