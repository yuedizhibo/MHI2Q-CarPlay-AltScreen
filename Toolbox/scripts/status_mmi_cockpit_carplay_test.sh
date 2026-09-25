#!/bin/sh
set -u
BASE="$0"
RESOLVED=$(command -v -- "$BASE" 2>/dev/null)
[ -n "$RESOLVED" ] || RESOLVED="$BASE"
SCRIPTDIR=$(cd -P -- "$(dirname -- "$RESOLVED")" 2>/dev/null && pwd -P)
[ -n "$SCRIPTDIR" ] || { echo "FAIL: cannot resolve STATUS directory"; exit 126; }

TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
DEVICE_ROOT=""
if [ "$TESTING" = 1 ]; then DEVICE_ROOT=${ALTSCREEN_CHAIN_ROOT:-}; fi
APP_BIN="$DEVICE_ROOT/mnt/app/root/carplay-altscreen/bin"
APP_SELF="$APP_BIN/status_mmi_cockpit_carplay_test.sh"
if [ "$SCRIPTDIR" != "$APP_BIN" ] && [ -f "$APP_SELF" ] && [ -f "$APP_BIN/altscreen_chain_test.sh" ]; then
    echo "APP_RUNTIME_FORWARD action=STATUS from=$SCRIPTDIR to=/mnt/app/root/carplay-altscreen/bin"
    if [ "$#" -gt 0 ]; then exec /bin/sh "$APP_SELF" "$@"; else exec /bin/sh "$APP_SELF"; fi
fi

CONTROLLER="$SCRIPTDIR/altscreen_chain_test.sh"
[ -f "$CONTROLLER" ] || { echo "FAIL: installed chain controller missing"; exit 127; }
/bin/sh "$CONTROLLER" status
STATUS_RC=$?

RUNTIME="$DEVICE_ROOT/mnt/app/root/carplay-altscreen"
JAR="$DEVICE_ROOT/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar"
ENABLED="$RUNTIME/state/basevideo3.enabled"
ACTIVE="$DEVICE_ROOT/tmp/mmi-mirror-active"
DEST_READY="$DEVICE_ROOT/tmp/mmi-mirror-basevideo.ready"
STARTED="$DEVICE_ROOT/tmp/mmi-mirror-controller.started"
JAVA_LOG="$DEVICE_ROOT/tmp/mmi-mirror-controller.log"
MIRROR="$RUNTIME/bin/mirror"
MIRROR_PID="$DEVICE_ROOT/tmp/MMI-Cockpit-Carplay.mirror.pid"
MIRROR_LOG="$DEVICE_ROOT/tmp/MMI-Cockpit-Carplay.mirror.log"
EXPECTED_SIZE=143072
EXPECTED_CKSUM=1515795662

file_size(){ n=$(wc -c < "$1" 2>/dev/null) || { echo 0; return; }; set -- $n; echo "${1:-0}"; }
file_cksum(){ if command -v cksum >/dev/null 2>&1; then cksum < "$1" 2>/dev/null | awk '{print $1}'; else echo unavailable; fi; }

echo "=== CarPlay private111 Direct Display V2 ==="
echo "SOURCE_PATH=private111_ScreenStreamProcessData"
echo "H264_TAP=/carplay111_h264"
echo "DECODER_BACKEND=stock_omx_screen_linearized_shm decoded_shm=/carplay111_decoded"
echo "DESTINATION=displayable3_gles"
echo "CONTEXT_POLICY=JAVA_ONLY context=80 composite=98,101,102,3 native_dmdt=0"
echo "WINDOW58_ENUMERATION=DISABLED sidecar_screen_read_window=0 hook_exact_stock_window_readback=1"

if [ -s "$JAR" ]; then
    SIZE=$(file_size "$JAR"); SUM=$(file_cksum "$JAR")
    if [ "$SIZE" = "$EXPECTED_SIZE" ] && { [ "$SUM" = unavailable ] || [ "$SUM" = "$EXPECTED_CKSUM" ]; }; then
        echo "HMI_CONTROL_PLANE=PASS size=$SIZE cksum=$SUM"
    else
        echo "HMI_CONTROL_PLANE=FAIL reason=identity_mismatch size=$SIZE cksum=$SUM"
        [ "$STATUS_RC" -ne 0 ] || STATUS_RC=1
    fi
else
    echo "HMI_CONTROL_PLANE=FAIL reason=jar_missing"
    [ "$STATUS_RC" -ne 0 ] || STATUS_RC=1
fi

[ -f "$ENABLED" ] && echo "DIRECT_DISPLAY_ENABLE=ENABLED" || echo "DIRECT_DISPLAY_ENABLE=DISABLED"
[ -f "$ACTIVE" ] && echo "JAVA80_DEMAND=YES" || echo "JAVA80_DEMAND=NO"

MIRROR_RUNNING=0
PID=""
if [ -f "$MIRROR_PID" ]; then
    PID=$(cat "$MIRROR_PID" 2>/dev/null || true)
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then MIRROR_RUNNING=1; fi
fi
[ "$MIRROR_RUNNING" = 1 ] && echo "DIRECT_DISPLAY_SIDECAR=RUNNING pid=$PID" || echo "DIRECT_DISPLAY_SIDECAR=NOT_RUNNING"
[ -x "$MIRROR/carplay-alt111-mirror-display" ] && echo "DIRECT_DISPLAY_BINARY=INSTALLED" || echo "DIRECT_DISPLAY_BINARY=MISSING"
if [ -s "$MIRROR/logo.rgba" ]; then
    echo "SECOND_SCREEN_LOGO=INSTALLED"
else
    echo "SECOND_SCREEN_LOGO=MISSING"
    [ "$STATUS_RC" -ne 0 ] || STATUS_RC=1
fi
if [ -s "$MIRROR/watermark.rgba" ]; then
    echo "SECOND_SCREEN_WATERMARK=INSTALLED text=FREE_OPEN_SOURCE_NO_RESALE motion=BOUNCING_DRIFT"
else
    echo "SECOND_SCREEN_WATERMARK=MISSING"
    [ "$STATUS_RC" -ne 0 ] || STATUS_RC=1
fi

HOOK_LOG=""
for candidate in "$DEVICE_ROOT/tmp/MMI-Cockpit-Carplay.altscreen_hook.log" "$DEVICE_ROOT/tmp/altscreen_hook.log"; do
    [ -f "$candidate" ] && { HOOK_LOG=$candidate; break; }
done

H264_SHM=0
FRAME_SHM=0
SHM_RECOVERED=0
AVCC_PROPERTY=0
AVCC_CONFIG=0
H264_DATA=0
H264_SPS=0
H264_PPS=0
H264_IDR=0
FRAME_LAYOUT=0
FRAME_LAYOUT_UNSUPPORTED=0
DECODER_FRAME=0
MAP_FAILURE=0
if [ -n "$HOOK_LOG" ]; then
    grep -q 'PHASE=H264_TAP_SHM_READY' "$HOOK_LOG" 2>/dev/null && H264_SHM=1
    grep -q 'PHASE=FRAME_TAP_SHM_READY' "$HOOK_LOG" 2>/dev/null && FRAME_SHM=1
    grep -q 'PHASE=H264_TAP_SHM_RECOVERED\|PHASE=FRAME_TAP_SHM_RECOVERED' "$HOOK_LOG" 2>/dev/null && SHM_RECOVERED=1
    grep -q 'PHASE=H264_AVCC_PROPERTY' "$HOOK_LOG" 2>/dev/null && AVCC_PROPERTY=1
    grep -q 'PHASE=H264_AVCC_CONFIG' "$HOOK_LOG" 2>/dev/null && AVCC_CONFIG=1
    grep -q 'ERROR PHASE=H264_TAP_SHM_MAP\|ERROR PHASE=FRAME_TAP_SHM_MAP' "$HOOK_LOG" 2>/dev/null && MAP_FAILURE=1
    grep -q 'PHASE=H264_TAP_FIRST_DATA' "$HOOK_LOG" 2>/dev/null && H264_DATA=1
    grep -q 'PHASE=H264_TAP_FIRST_SPS' "$HOOK_LOG" 2>/dev/null && H264_SPS=1
    grep -q 'PHASE=H264_TAP_FIRST_PPS' "$HOOK_LOG" 2>/dev/null && H264_PPS=1
    grep -q 'PHASE=H264_TAP_FIRST_IDR' "$HOOK_LOG" 2>/dev/null && H264_IDR=1
    grep -q 'PHASE=FRAME_TAP_LAYOUT' "$HOOK_LOG" 2>/dev/null && FRAME_LAYOUT=1
    grep -q 'ERROR PHASE=FRAME_TAP_UNSUPPORTED_LAYOUT' "$HOOK_LOG" 2>/dev/null && FRAME_LAYOUT_UNSUPPORTED=1
    grep -q 'PHASE=DECODER_FIRST_FRAME backend=stock-omx-tap' "$HOOK_LOG" 2>/dev/null && DECODER_FRAME=1

    echo "SHM_WRITER_READY=h264:$H264_SHM decoded:$FRAME_SHM map_failure:$MAP_FAILURE recovered:$SHM_RECOVERED"
    echo "H264_AVCC_PROPERTY=$AVCC_PROPERTY H264_CODEC_CONFIG_EMITTED=$AVCC_CONFIG"
    echo "H264_TAP_DATA=$H264_DATA SPS=$H264_SPS PPS=$H264_PPS IDR=$H264_IDR"
    echo "FRAME_TAP_LAYOUT=$FRAME_LAYOUT unsupported:$FRAME_LAYOUT_UNSUPPORTED DECODER_FIRST_FRAME=$DECODER_FRAME backend=stock-omx-screen-linearized-shm"
    LINEARIZER_LAST="$(grep 'PHASE=FRAME_LINEARIZER_PROGRESS' "$HOOK_LOG" 2>/dev/null | tail -n 1 || true)"
    [ -z "$LINEARIZER_LAST" ] || echo "FRAME_LINEARIZER_LAST='$LINEARIZER_LAST'"
    SLOW_READBACKS="$(grep -c 'PHASE=FRAME_LINEARIZER_SLOW' "$HOOK_LOG" 2>/dev/null || true)"
    case "$SLOW_READBACKS" in ''|*[!0-9]*) SLOW_READBACKS=0 ;; esac
    echo "FRAME_LINEARIZER_SLOW_EVENTS=$SLOW_READBACKS threshold_us=20000"
    echo "HOOK_DIRECT111_LOG_TAIL_BEGIN"
    grep -E 'PHASE=(STREAM_111_|VIDEO_111_|H264_TAP_|H264_AVCC_|DIRECT111_TAP_|FRAME_TAP_|FRAME_LINEARIZER_|DECODER_)|ERROR PHASE=(H264_TAP_SHM_|FRAME_TAP_SHM_|FRAME_TAP_UNSUPPORTED_LAYOUT|FRAME_LINEARIZER_)' "$HOOK_LOG" 2>/dev/null | tail -n 120 || true
    echo "HOOK_DIRECT111_LOG_TAIL_END"
else
    echo "H264_TAP_DATA=UNKNOWN hook_log_missing=1"
    echo "DECODER_FIRST_FRAME=UNKNOWN hook_log_missing=1"
fi

H264_VALID=0
SIDECAR_DECODE=0
DISPLAY3=0
DIRECT_ACTIVE=0
if [ -f "$MIRROR_LOG" ]; then
    grep -q 'PHASE=H264_STREAM_VALID' "$MIRROR_LOG" 2>/dev/null && H264_VALID=1
    grep -q 'PHASE=DECODER_FIRST_FRAME' "$MIRROR_LOG" 2>/dev/null && SIDECAR_DECODE=1
    grep -q 'PHASE=DISPLAYABLE3_FIRST_PRESENT result=OK' "$MIRROR_LOG" 2>/dev/null && DISPLAY3=1
    grep -q 'PHASE=DIRECT111_ACTIVE' "$MIRROR_LOG" 2>/dev/null && DIRECT_ACTIVE=1

    echo "H264_STREAM_VALID=$H264_VALID"
    echo "SIDECAR_DECODER_FRAME=$SIDECAR_DECODE"
    echo "DISPLAYABLE3_FIRST_PRESENT=$DISPLAY3"
    echo "DIRECT111_ACTIVE=$DIRECT_ACTIVE"
    echo "DIRECT111_LOG_TAIL_BEGIN"
    tail -n 80 "$MIRROR_LOG" 2>/dev/null || true
    echo "DIRECT111_LOG_TAIL_END"
else
    echo "H264_STREAM_VALID=UNKNOWN mirror_log_missing=1"
    echo "SIDECAR_DECODER_FRAME=UNKNOWN mirror_log_missing=1"
    echo "DISPLAYABLE3_FIRST_PRESENT=UNKNOWN mirror_log_missing=1"
    echo "DIRECT111_ACTIVE=UNKNOWN mirror_log_missing=1"
fi

DEST=0
if [ -f "$DEST_READY" ]; then
    DEST=1
    echo "DEST_FRAME_READY=YES"
    cat "$DEST_READY" 2>/dev/null || true
else
    echo "DEST_FRAME_READY=NO"
fi

[ -f "$STARTED" ] && echo "JAVA_CONTROLLER=STARTED" || echo "JAVA_CONTROLLER=NOT_STARTED"
CTXREQ=0
CTXACT=0
if [ -f "$JAVA_LOG" ]; then
    if grep -q 'ownership acquire requested' "$JAVA_LOG" 2>/dev/null; then
        CTXREQ=1
        echo "JAVA_CTX80_REQUEST=YES"
    else
        echo "JAVA_CTX80_REQUEST=NO"
    fi
    if grep -q 'CTX80_OBSERVED actual=80' "$JAVA_LOG" 2>/dev/null; then
        CTXACT=1
        CTX_LINE=$(grep 'CTX80_OBSERVED actual=80' "$JAVA_LOG" 2>/dev/null | tail -n 1)
        echo "JAVA_CTX80_ACTUAL=80 proof='$CTX_LINE'"
    else
        echo "JAVA_CTX80_ACTUAL=NOT_OBSERVED desired=80"
    fi
    echo "JAVA80_LOG_TAIL_BEGIN"
    tail -n 40 "$JAVA_LOG" 2>/dev/null || true
    echo "JAVA80_LOG_TAIL_END"
else
    echo "JAVA_CTX80_REQUEST=UNKNOWN log_missing=1"
    echo "JAVA_CTX80_ACTUAL=UNKNOWN log_missing=1"
fi

# V2 vehicle display readiness is driven by the Screen-linearized decoded path.
# H264 SPS/PPS/IDR evidence is reported independently for the future standalone
# decoder and must not falsely mark a working displayable3/Context80 route bad.
if [ "$DECODER_FRAME" = 1 ] && [ "$DISPLAY3" = 1 ] &&
   [ "$DEST" = 1 ] && [ "$MIRROR_RUNNING" = 1 ] &&
   [ "$CTXACT" = 1 ]; then
    echo "PHYSICAL_ROUTE_READY=SOFTWARE_CHAIN_COMPLETE decoder=1 displayable3=1 ctx80=1 human_vc_confirmation_required=YES"
    echo "COMPRESSED_PATH_EVIDENCE=h264_data:$H264_DATA avcc_property:$AVCC_PROPERTY config:$AVCC_CONFIG sps:$H264_SPS pps:$H264_PPS idr:$H264_IDR"
else
    echo "PHYSICAL_ROUTE_READY=NO decoded_shm=$FRAME_SHM frame_layout=$FRAME_LAYOUT layout_unsupported=$FRAME_LAYOUT_UNSUPPORTED decoder=$DECODER_FRAME displayable3=$DISPLAY3 destination=$DEST sidecar=$MIRROR_RUNNING ctx80_actual=$CTXACT"
    echo "COMPRESSED_PATH_EVIDENCE=h264_shm:$H264_SHM h264_data:$H264_DATA avcc_property:$AVCC_PROPERTY config:$AVCC_CONFIG sps:$H264_SPS pps:$H264_PPS idr:$H264_IDR"
fi

exit "$STATUS_RC"
