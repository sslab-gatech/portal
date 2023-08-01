#  ./qemu/build/qemu-system-aarch64 -s -S -nographic -serial telnet::54340,server \
#  -smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3,iommu=smmuv3,dumpdtb=qemu.dtb \
#  -m 2048 -cpu max,lpa2=off -d unimp,guest_errors  -D qemu.log -kernel out/bin/Image -bios flash.bin \
#  -initrd initrd -device virtio-gpu-pci,bus=pcie.0
#
#  dtc -I dtb -O dts -o qemu.dts qemu.dtb
#

./qemu/build/qemu-system-aarch64 -s -S -nographic -serial telnet::54340,server \
-smp clusters=2,cores=4 -machine virt,secure=on,rmm=on,virtualization=on,gic-version=3,iommu=smmuv3 \
-m 2048 -cpu max,lpa2=off -d unimp,guest_errors  -D qemu.log -kernel out/bin/Image -bios flash.bin \
-initrd initrd \
-device virtio-gpu-pci,bus=pcie.0

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
