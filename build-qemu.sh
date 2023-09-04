NPROC=$(nproc)
mkdir -p ./qemu/build
cd ./qemu/build
../configure --target-list=aarch64-softmmu --disable-docs
make -j $(nproc)
cd ../../

