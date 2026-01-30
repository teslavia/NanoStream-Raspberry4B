#!/bin/sh
#参数说明  
#1) RTSP 地址（默认 rtsp://127.0.0.1:8554/live）  
#2) 输出目录（默认 calib）  
#3) 抓取张数（默认 300）  
#4) 抓取帧率（默认 1 fps）
#example: ./scripts/capture_calib.sh rtsp://127.0.0.1:8554/live calib 300 1
set -eu

RTSP_URL=${1:-"rtsp://127.0.0.1:8554/live"}
OUT_DIR=${2:-"calib"}
COUNT=${3:-300}
FPS=${4:-1}

mkdir -p "$OUT_DIR"

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "[Error] ffmpeg not found. Please install ffmpeg." >&2
  exit 1
fi

echo "[Capture] RTSP: $RTSP_URL"
echo "[Capture] Output: $OUT_DIR"
echo "[Capture] Count: $COUNT, FPS: $FPS"

ffmpeg -hide_banner -loglevel error -rtsp_transport tcp \
  -i "$RTSP_URL" \
  -vf "fps=${FPS},scale=320:320" \
  -frames:v "$COUNT" \
  "$OUT_DIR/frame_%04d.jpg"

echo "[Capture] Done. Saved $COUNT frames to $OUT_DIR"
