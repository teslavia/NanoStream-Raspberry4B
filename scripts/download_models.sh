#!/bin/bash
set -e

# Define paths
SCRIPT_DIR=$(dirname "$0")
PROJECT_ROOT="$SCRIPT_DIR/.."
MODEL_DIR="$PROJECT_ROOT/models"

mkdir -p "$MODEL_DIR"

echo "[NanoStream] Checking for NCNN models..."

# Files we need (Using nanodet_m 320x320 for stability and availability)
PARAM_FILE="$MODEL_DIR/nanodet_m.param"
BIN_FILE="$MODEL_DIR/nanodet_m.bin"

# Check if they exist
if [ -f "$PARAM_FILE" ] && [ -f "$BIN_FILE" ]; then
    echo "[NanoStream] Models already exist in $MODEL_DIR."
    exit 0
fi

echo "[NanoStream] Downloading NanoDet-m models (320x320)..."
echo "[NanoStream] Source: nihui/ncnn-assets (Reliable Mirror)"

# Base URL for Nihui's assets
BASE_URL="https://github.com/nihui/ncnn-assets/raw/master/models"

# Download Param
echo "Downloading .param..."
curl -L "$BASE_URL/nanodet_m.param" -o "$PARAM_FILE"

# Download Bin
echo "Downloading .bin..."
curl -L "$BASE_URL/nanodet_m.bin" -o "$BIN_FILE"

echo "[NanoStream] Download complete! Models ready in $MODEL_DIR"
ls -l $MODEL_DIR
