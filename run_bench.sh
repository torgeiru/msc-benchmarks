#!/bin/bash

read -p "Benchmark target (host|linux|includeos): " BENCH_TARGET
if [ "$BENCH_TARGET" = "" ]; then
    BENCH_TARGET="includeos"
fi

ls -l | grep benchmark
read -p "What benchmark do you want to run: " BENCHMARK

# Removing and creating a results directory
rm -rf "./results"
mkdir "results"

# Setting up tmpfs
TMP_DIR=$(mktemp -d -p /tmp virtiofs_share.XXXXXX)
SHARED_DIR=$TMP_DIR/shared
mkdir $SHARED_DIR
echo "Sharing directory ${SHARED_DIR} with guest"

sudo mount -t tmpfs -o mode=777,size=4G tmpfs "$SHARED_DIR"

# Copying material folder contents if exists
if [ -d "$BENCHMARK/material" ]; then
    echo "Copying material contents"
    cp -vvv $BENCHMARK/material/* ${SHARED_DIR}
fi

# Start system
if [ "$BENCH_TARGET" = "host" ]; then
    echo "Building benchmark program ${BENCHMARK}/linux_drv.nix"
    nix-build $BENCHMARK/linux_drv.nix
    ln -sf "$SHARED_DIR" VirtioFS0
    ./result/bin/virtiofs_bench
    rm -r VirtioFS0
elif [ "$BENCH_TARGET" = "linux" ]; then
    echo "Building benchmark program ${BENCHMARK}/linux_drv.nix"
    nix-build $BENCHMARK/linux_drv.nix
    cp ./result/bin/virtiofs_bench $SHARED_DIR
    nix-shell --run "./run_linux.py $SHARED_DIR"
else
    read -p "Use VirtioFS interrupts? (y/N): " USE_INTERRUPTS
    if [ "$USE_INTERRUPTS" = "y" ] || [ "$USE_INTERRUPTS" = "Y" ]; then
        USE_INTERRUPTS="true"
        VIRTIOFS_DRIVER="interrupts"
    else
        USE_INTERRUPTS="false"
        VIRTIOFS_DRIVER="polling"
    fi

    read -p "Use patched VirtioFSD? (y/N): " USE_PATCHED_VIRTIOFSD
    if [ "$USE_PATCHED_VIRTIOFSD" = "y" ] || [ "$USE_PATCHED_VIRTIOFSD" = "Y" ]; then
        USE_PATCHED_VIRTIOFSD="true"
    else
        USE_PATCHED_VIRTIOFSD="false"
    fi

    echo "Building unikernel ${BENCHMARK}/includeos_drv.nix with ${VIRTIOFS_DRIVER}"
    nix-build $BENCHMARK/includeos_drv.nix --arg use_interrupts $USE_INTERRUPTS
    cp ./result/bin/virtiofs_bench $SHARED_DIR
    nix-shell --arg use_patched_virtiofsd $USE_PATCHED_VIRTIOFSD --run "./run_includeos.py $SHARED_DIR"
fi

rm -rf ./result
rm -rf ./results
mkdir results
cp -v -r $SHARED_DIR/*.csv results
cp -v -r $SHARED_DIR/*.yuv results
cp -v -r $SHARED_DIR/*_copy.bin results

# Cleanup
sudo umount "$SHARED_DIR"
rm -rf "$TMP_DIR"
