#!/usr/bin/env bash
set -euo pipefail

MP4_INPUT="${MP4_INPUT:-./assets/sample.mp4}"
BASE_URL="http://127.0.0.1:8090"

APP_BIN="${APP_BIN:-../build/NVRLite}"
APP_ARGS="${APP_ARGS:---config ./config_ci.json}"

RECORD_DIR="./recordings_ci"
STREAM_IDS=("cam01" "cam02" "cam03")

RTSP_PORT=8554
MEDIAMTX_BIN="./mediamtx"

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing $1"; exit 1; }; }
need ffmpeg
need curl
need jq


urlencode() {
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import sys,urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$1"
  else
    jq -rn --arg s "$1" '$s|@uri'
  fi
}


cleanup_pids=()
cleanup() {
  set +e
  echo "==> Cleaning up..."
  for pid in "${cleanup_pids[@]:-}"; do
    kill "$pid" >/dev/null 2>&1 || true
  done
  sleep 0.3
}
trap cleanup EXIT

mkdir -p "$RECORD_DIR"

[ -f "$MP4_INPUT" ] || { echo "MP4 not found: $MP4_INPUT" >&2; exit 1; }

# 1) Download MediaMTX if needed
if [ ! -x "$MEDIAMTX_BIN" ]; then
  echo "==> Downloading MediaMTX..."
  arch="$(uname -m)"
  case "$arch" in
    x86_64) pkg="mediamtx_v1.8.4_linux_amd64.tar.gz" ;;
    aarch64|arm64) pkg="mediamtx_v1.8.4_linux_arm64v8.tar.gz" ;;
    *) echo "Unsupported arch for auto-download: $arch" >&2; exit 1 ;;
  esac
  curl -sSL -o /tmp/mediamtx.tgz "https://github.com/bluenviron/mediamtx/releases/download/v1.8.4/${pkg}"
  tar -xzf /tmp/mediamtx.tgz -C . mediamtx
  chmod +x ./mediamtx
fi

# 2) Start MediaMTX
echo "==> Starting MediaMTX..."
./mediamtx ./mediamtx.yml >/tmp/mediamtx.log 2>&1 &
cleanup_pids+=("$!")
sleep 0.8

# 3) Publish streams (video only, copy)
echo "==> Publishing RTSP streams with ffmpeg..."
for sid in "${STREAM_IDS[@]}"; do
  out="rtsp://127.0.0.1:${RTSP_PORT}/${sid}"
  echo "   $sid -> $out"
  ffmpeg -hide_banner -loglevel error \
    -stream_loop -1 -re -i "$MP4_INPUT" \
    -map 0:v:0 -c:v copy \
    -f rtsp -rtsp_transport tcp -rtsp_flags prefer_tcp \
    "$out" &
  cleanup_pids+=("$!")
done

# Verify each RTSP path is readable
echo "==> Verifying RTSP streams are readable..."
for sid in "${STREAM_IDS[@]}"; do
  url="rtsp://127.0.0.1:${RTSP_PORT}/${sid}"
  ok=0
  for _ in {1..30}; do
    if ffmpeg -hide_banner -loglevel error -rtsp_transport tcp -i "$url" -t 0.5 -f null - >/dev/null 2>&1; then
      ok=1
      break
    fi
    sleep 0.2
  done
  [ "$ok" = "1" ] || {
    echo "RTSP stream not readable: $url" >&2
    echo "MediaMTX log tail:" >&2
    tail -n 120 /tmp/mediamtx.log >&2 || true
    exit 1
  }
done

# 4) Start NVRLite
echo "==> Starting NVRLite..."
"$APP_BIN" $APP_ARGS &
cleanup_pids+=("$!")

# Wait for HTTP
echo "==> Waiting for HTTP..."
for _ in {1..80}; do
  if curl -s "$BASE_URL/stream/status" >/dev/null 2>&1; then
    break
  fi
  sleep 0.2
done
curl -s "$BASE_URL/stream/status" >/dev/null 2>&1 || { echo "HTTP did not come up at $BASE_URL" >&2; exit 1; }

# 5) Start streams
echo "==> Starting streams..."
for sid in "${STREAM_IDS[@]}"; do
  curl -sS -X POST "$BASE_URL/stream/start" \
    -H "Content-Type: application/json" \
    -d "{\"stream_id\":\"$sid\"}" | jq -e '.status=="ok"' >/dev/null
done

# Wait until streaming==true for all
echo "==> Waiting for streams streaming=true..."
for _ in {1..80}; do
  st="$(curl -sS "$BASE_URL/stream/status" || true)"
  if echo "$st" | jq . >/dev/null 2>&1; then
    all_ok=1
    for sid in "${STREAM_IDS[@]}"; do
      if ! echo "$st" | jq -e --arg sid "$sid" '.streams[] | select(.stream_id==$sid) | .streaming==true' >/dev/null; then
        all_ok=0; break
      fi
    done
    [ "$all_ok" = "1" ] && break
  fi
  sleep 0.25
done

# 6) Recording test
echo "==> Recording test..."
FILES=()
for sid in "${STREAM_IDS[@]}"; do
  resp="$(curl -sS -w "\n%{http_code}" -X POST "$BASE_URL/record/start" \
    -H "Content-Type: application/json" \
    -d "{\"stream_id\":\"$sid\"}")"
  body="$(echo "$resp" | sed '$d')"
  code="$(echo "$resp" | tail -n1)"

  if [ "$code" != "200" ]; then
    echo "record/start failed for $sid (HTTP $code): $body" >&2
    exit 1
  fi

  echo "$body" | jq . >/dev/null
  file="$(echo "$body" | jq -r '.file')"
  [ -n "$file" ] && [ "$file" != "null" ] || { echo "No file returned for $sid: $body" >&2; exit 1; }

  # Normalize to basename so it matches /files/list and ?file=...
  file_base="$(basename "$file")"
  FILES+=("$file_base")

  sleep 1

  curl -sS -X POST "$BASE_URL/record/stop" \
    -H "Content-Type: application/json" \
    -d "{\"stream_id\":\"$sid\"}" | jq -e '.status=="ok"' >/dev/null

  # allow post_buffering finalize to close the file (your config uses 1.0s)
  sleep 2
done

echo "==> Files list..."
LIST=$(curl -sS "$BASE_URL/files/list")
echo "$LIST" | jq . >/dev/null

for f in "${FILES[@]}"; do
  echo "$LIST" | jq -e --arg f "$f" '.files[] | select(.name==$f) | .name' >/dev/null || {
    echo "File $f not found in /files/list" >&2
    echo "List was:" >&2
    echo "$LIST" | jq . >&2
    exit 1
  }
done

echo "==> File status + remove..."
for f in "${FILES[@]}"; do
  ef="$(urlencode "$f")"

  # /files/status
  resp="$(curl -sS -w "\n%{http_code}" "$BASE_URL/files/status?file=$ef")"
  body="$(echo "$resp" | sed '$d')"
  code="$(echo "$resp" | tail -n1)"

  if [ "$code" != "200" ]; then
    echo "/files/status failed (HTTP $code) for file=$f: $body" >&2
    exit 1
  fi
  echo "$body" | jq -e '.status=="ok"' >/dev/null

  # /files/remove
  resp="$(curl -sS -w "\n%{http_code}" -X POST "$BASE_URL/files/remove" \
    -H "Content-Type: application/json" \
    -d "{\"file\":\"$f\"}")"
  body="$(echo "$resp" | sed '$d')"
  code="$(echo "$resp" | tail -n1)"

  if [ "$code" != "200" ]; then
    echo "/files/remove failed (HTTP $code) for file=$f: $body" >&2
    exit 1
  fi

  echo "$body" | jq -e '.status=="ok"' >/dev/null
done

echo "==> CI E2E PASSED ✅"

