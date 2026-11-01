#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TC_DIR="/home/sh1nr4i/project_andro/toolchain/clang-r416183b"

if [ ! -f "$TC_DIR/bin/clang" ]; then
    echo "[-] Error: Toolchain belum ada di $TC_DIR"
    exit 1
fi

export PATH="$TC_DIR/bin:$PATH"
export LD_LIBRARY_PATH="$TC_DIR/lib64:$LD_LIBRARY_PATH"

export ARCH=arm64
export CC=clang
export LD=ld.lld
export AR=llvm-ar
export NM=llvm-nm
export OBJCOPY=llvm-objcopy
export OBJDUMP=llvm-objdump
export READELF=llvm-readelf
export STRIP=llvm-strip
export LLVM=1
export LLVM_IAS=1
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_COMPAT=arm-linux-gnueabi-

cd "$SCRIPT_DIR"

echo "=============================================="
echo " Toolchain: $(clang --version | head -1)"
echo " Optimization: -O2 (Standard AOSP)"
echo " Cores: $(nproc)"
echo "=============================================="

mkdir -p out
echo "[*] Step 1: Generating defconfig..."
make O=out gki_defconfig

echo "[*] Step 2: Compiling Kernel (Image.gz)..."
make -j$(nproc) O=out Image.gz

echo "=============================================="
echo "[+] SUKSES! Kernel Image.gz siap di:"
ls -lh out/arch/arm64/boot/Image.gz
echo "=============================================="

echo "[*] Step 3: Packaging AnyKernel3 flashable zip..."
cp out/arch/arm64/boot/Image.gz /home/sh1nr4i/project_andro/AnyKernel3/Image.gz
cd /home/sh1nr4i/project_andro/AnyKernel3
rm -f /home/sh1nr4i/project_andro/Chihiro-Kernel-flashable.zip
zip -r9 /home/sh1nr4i/project_andro/Chihiro-Kernel-flashable.zip * -x .git README.md *placeholder > /dev/null
cd "$SCRIPT_DIR"

echo "=============================================="
echo "[+] Flashable ZIP siap disideload / diinstall:"
ls -lh /home/sh1nr4i/project_andro/Chihiro-Kernel-flashable.zip
echo "=============================================="
