git clone https://github.com/OP-TEE/build.git optee-build
cd optee-build
git reset --hard 064b4b0584a1ce50e533f3174d3e18d273febf90
cd ../
cp *.mk ./optee-build
mv optee-build ../
