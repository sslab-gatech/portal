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

