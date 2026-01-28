#!/usr/bin/env bash
set -euo pipefail

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
