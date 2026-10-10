#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
QNX_HOST="${QNX_HOST:-/usr/qnx650/host/qnx6/x86}"
QNX_TARGET="${QNX_TARGET:-/usr/qnx650/target/qnx6}"
export QNX_HOST QNX_TARGET
CC="${CC:-$QNX_HOST/usr/bin/ntoarmv7-gcc}"
if [ ! -x "$CC" ] && [ -x "$QNX_HOST/usr/bin/arm-unknown-nto-qnx6.5.0eabi-gcc" ]; then
  CC="$QNX_HOST/usr/bin/arm-unknown-nto-qnx6.5.0eabi-gcc"
fi
export CC
[ -x "$CC" ] || { echo "ERROR: QNX ARMv7 gcc driver not found: $CC" >&2; exit 1; }
cd "$ROOT"
[ -s assets/logo_frames.z ] || {
  echo "ERROR: embedded logo missing; run python3 tools/build_startup_logo.py ../../../Logo.mp4 assets/logo_frames.z" >&2
  exit 1
}
make clean
make
BIN="build/carplay-alt111-mirror-display"
file "$BIN" 2>/dev/null || true
ls -lh "$BIN"
READELF="$QNX_HOST/usr/bin/ntoarmv7-readelf"
if [ -x "$READELF" ]; then
  if "$READELF" -d "$BIN" | grep -q 'libstdc++'; then
    echo "ERROR: unexpected dynamic libstdc++ dependency" >&2
    exit 1
  fi
fi

STRINGS="${STRINGS:-}"
if [ -z "$STRINGS" ]; then
  if [ -x "$QNX_HOST/usr/bin/ntoarmv7-strings" ]; then
    STRINGS="$QNX_HOST/usr/bin/ntoarmv7-strings"
  elif command -v strings >/dev/null 2>&1; then
    STRINGS="$(command -v strings)"
  fi
fi
[ -n "$STRINGS" ] || {
  echo "ERROR: no strings tool found for sidecar verification" >&2
  exit 1
}

check_marker() {
  marker=$1
  "$STRINGS" "$BIN" | grep -Fq "$marker"
}

for marker in \
  'carplay-private111-direct-display-v3.1-oem-map-1to1-clip' \
  'present_policy=source-driven' \
  'no_success_sleep=1' \
  'PHASE=OEM_MAP_PLACEMENT' \
  'PHASE=OEM_MAP_PLACEMENT_STATE_GAP' \
  'action=retain_previous' \
  'PHASE=OEM_MAP_RERENDER' \
  'live_switch=1' \
  'renderer_scale=0' \
  'natural_clip=1' \
  'PHASE=OEM_GEOMETRY_V31' \
  'geometry_policy=OEM_MAP_PLANE_1TO1_CLIP_V31' \
  'stall_report_after_ms=' \
  'PHASE=H264_SHM_ATTACHED' \
  'PHASE=DECODED_SHM_WAIT_SIZE' \
  'PHASE=SOURCE_SESSION' \
  'PHASE=GATE_RECOVER_CURRENT_SESSION' \
  'matching_identity_plus_frame_progress' \
  'packed_tight_required=1' \
  'stream111_request_or_phone_marker' \
  'STREAM_111_REQUESTED=YES' \
  'PHASE=PIPELINE_SOURCE_PRIMED' \
  'startup_frame_progress_required=2' \
  'PHASE=H264_STREAM_VALID' \
  'PHASE=DECODER_FIRST_FRAME' \
  'PHASE=NV12_CSC_READY' \
  'PHASE=DISPLAYABLE3_FIRST_PRESENT' \
  '/tmp/mmi-altscreen-displayable3.state' \
  'DISPLAYABLE3_OWNERSHIP_V1' \
  'display_observer_revision=V32_READABLE_STATE_V1' \
  'PHASE=DISPLAYABLE3_OWNERSHIP' \
  'PHASE=DIRECT111_ACTIVE' \
  'PHASE=STARTUP_LOGO_BEGIN asset=EMBEDDED' \
  'PHASE=STARTUP_LOGO_END' \
  'PHASE=STARTUP_LOGO_SKIP' \
  'window58_readback=0'
do
  check_marker "$marker" || {
    echo "ERROR: built sidecar is missing direct111 marker: $marker" >&2
    exit 1
  }
done

for marker in \
  'screen_read_window' \
  'WINDOW_MANAGER_CONTEXT event observer ready'
do
  if "$STRINGS" "$BIN" | grep -Fq "$marker"; then
    echo "ERROR: forbidden direct-display marker present: $marker" >&2
    exit 1
  fi
done

echo "MIRROR_BUILD_ID=carplay-private111-direct-display-v3.1-oem-map-1to1-clip"
echo "MIRROR_BUILD=PASS output=$ROOT/$BIN"
