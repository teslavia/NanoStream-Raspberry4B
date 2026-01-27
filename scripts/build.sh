#!/bin/bash
set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

echo "[Build] Project Root: $PROJECT_ROOT"

# Create build directory
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# Check for ARM architecture to give a hint
ARCH=$(uname -m)
# Use single brackets for sh/dash compatibility
if [ "$ARCH" != "aarch64" ] && [ "$ARCH" != "armv7l" ]; then
    echo "[Warning] You are building on $ARCH (likely Host Mac)."
    echo "          Dependencies (libgstreamer, libncnn, libgst-rtsp-server) must be installed locally."
    echo "          If they are missing, CMake will fail."
fi

# Run CMake
echo "[Build] Running CMake..."
cmake ..

# Run Make
echo "[Build] Compiling..."
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

echo "[Build] Success! Executable is at: $BUILD_DIR/NanoStream"
