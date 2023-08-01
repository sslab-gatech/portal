export GCC5_AARCH64_PREFIX="aarch64-linux-gnu-"
git submodule update --init
source edksetup.sh
make -C BaseTools
#build -a AARCH64 -t GCC5 -p ArmVirtPkg/ArmVirtQemu.dsc -b RELEASE
build -a AARCH64 -t GCC5 -p ArmVirtPkg/ArmVirtQemuKernel.dsc -b RELEASE
#cp ./Build/ArmVirtQemu-AARCH64/RELEASE_GCC5/FV/QEMU_EFI.fd ../ 
cp ./Build/ArmVirtQemuKernel-AARCH64/RELEASE_GCC5/FV/QEMU_EFI.fd ../ 
