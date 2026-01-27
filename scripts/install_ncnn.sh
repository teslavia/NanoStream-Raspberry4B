#!/bin/bash
set -e

# Work in a temporary directory
WORK_DIR="/tmp/ncnn_build_v2"
rm -rf $WORK_DIR
mkdir -p $WORK_DIR
cd $WORK_DIR

echo "[Install] Cloning NCNN..."
# Clone standard ncnn (shallow clone for speed)
git clone --depth 1 https://github.com/Tencent/ncnn.git
cd ncnn

echo "[Install] Configuring NCNN build..."
mkdir -p build
cd build

# Configure specifically for RPi 4 with explicit install prefix
cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DNCNN_VULKAN=OFF \
    -DNCNN_SYSTEM_GLSLANG=OFF \
    -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_PIXEL_ROTATE=OFF \
    -DNCNN_PIXEL_AFFINE=OFF \
    -DNCNN_BUILD_BENCHMARK=OFF \
    ..

echo "[Install] Compiling NCNN..."
make -j4

echo "[Install] Installing to /usr/local..."
sudo make install

echo "[Install] Updating shared library cache..."
sudo ldconfig

echo "[Install] Checking installation..."
if [ -f "/usr/local/include/ncnn/net.h" ]; then
    echo "SUCCESS: Header found at /usr/local/include/ncnn/net.h"
else
    echo "ERROR: Header NOT found. Installation might have failed."
    exit 1
fi
