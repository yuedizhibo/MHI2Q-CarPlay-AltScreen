#!/bin/sh
# CarPlay private111 Direct Display V2 INSTALL.
# Installs the type111 control/data plane, H264/decoded SHM bridge,
# displayable3 GLES sidecar, and Java80 HMI control plane.
# Window58 readback and RGI98 native renderer are not used by the sidecar.
set -u

BASE="$0"
RESOLVED=$(command -v -- "$BASE" 2>/dev/null)
[ -n "$RESOLVED" ] || RESOLVED="$BASE"
SCRIPTDIR=$(cd -P -- "$(dirname -- "$RESOLVED")" 2>/dev/null && pwd -P)
[ -n "$SCRIPTDIR" ] || { echo "FAIL: cannot resolve installer directory"; exit 126; }

TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
DEVICE_ROOT=""
VOLUME=""
if [ "$TESTING" = 1 ]; then
    VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
    DEVICE_ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    [ -n "$VOLUME" ] || { echo "FAIL: testing volume missing"; exit 1; }
    case "$DEVICE_ROOT" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_ROOT"; exit 2 ;; esac
else
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -s "$candidate/Toolbox/carplay_alt_screen/universal/libcarplay_altscreen.so" ] &&
           [ -s "$candidate/Toolbox/carplay_alt_screen/hmi/carplay_hook-basevideo3.jar" ]; then
            VOLUME=$candidate; break
        fi
    done
fi

[ -n "$VOLUME" ] || { echo "FAIL: no Toolbox SD card discovered"; exit 1; }
CONTROLLER="$VOLUME/Toolbox/scripts/altscreen_chain_test.sh"
MIRROR_RELEASE="$VOLUME/Toolbox/carplay_alt_screen/mirror_display/release"
MIRROR_INFO="$MIRROR_RELEASE/BUILD_INFO.txt"
JAR_SOURCE="$VOLUME/Toolbox/carplay_alt_screen/hmi/carplay_hook-basevideo3.jar"
JAR_TARGET="$DEVICE_ROOT/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar"
JAR_TARGET_DIR=$(dirname -- "$JAR_TARGET")
EXPECTED_SIZE=143072
EXPECTED_CKSUM=1515795662

[ -f "$CONTROLLER" ] || { echo "FAIL: chain controller missing: $CONTROLLER"; exit 127; }
[ -s "$JAR_SOURCE" ] || { echo "FAIL: Java80 HMI JAR missing: $JAR_SOURCE"; exit 1; }
[ -s "$MIRROR_INFO" ] || { echo "FAIL: V2 Mirror BUILD_INFO missing: $MIRROR_INFO"; exit 1; }
grep -Fq 'release_binary_status=PRIVATE111_DIRECT_DISPLAY_V2' "$MIRROR_INFO" 2>/dev/null &&
grep -Fq 'vehicle_zip_status=READY_FOR_VEHICLE_TEST' "$MIRROR_INFO" 2>/dev/null || {
    echo "FAIL: this package is not an approved rebuilt V2 vehicle release"
    grep -E '^(release_binary_status|vehicle_zip_status)=' "$MIRROR_INFO" 2>/dev/null || true
    echo "ACTION=REBUILD_QNX_SIDECAR_AND_PROMOTE_BEFORE_INSTALL"
    exit 1
}

file_size(){
    n=$(wc -c < "$1" 2>/dev/null) || { echo 0; return; }
    set -- $n
    echo "${1:-0}"
}
file_cksum(){
    if command -v cksum >/dev/null 2>&1; then
        cksum < "$1" 2>/dev/null | awk '{print $1}'
    else
        echo unavailable
    fi
}
jar_valid(){
    f=$1
    [ -s "$f" ] || return 1
    [ "$(file_size "$f")" = "$EXPECTED_SIZE" ] || return 1
    sum=$(file_cksum "$f")
    [ "$sum" = unavailable ] || [ "$sum" = "$EXPECTED_CKSUM" ] || return 1
}
mount_app_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/app; }
mount_app_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/app; }

jar_valid "$JAR_SOURCE" || {
    echo "FAIL: Java80 HMI JAR identity mismatch"
    echo "expected_size=$EXPECTED_SIZE expected_cksum=$EXPECTED_CKSUM"
    echo "actual_size=$(file_size "$JAR_SOURCE") actual_cksum=$(file_cksum "$JAR_SOURCE")"
    exit 1
}

echo "PACKAGE_MODE=CARPLAY_PRIVATE111_DIRECT_DISPLAY_V2"
echo "NATIVE_SOURCE=private111_ScreenStreamProcessData h264_shm=/carplay111_h264"
echo "DECODER_BACKEND=stock_omx_screen_linearized_shm decoded_shm=/carplay111_decoded"
echo "PIXEL_BRIDGE=Screen_linearized_NV12_to_existing_MMI_GLES"
echo "PIXEL_TARGET=displayable3"
echo "HMI_CONTEXT=ctx80"
echo "WINDOW58_READBACK=DISABLED"
echo "DIRECT_DISPLAY_SIDECAR=INCLUDED"
echo "RGI98_NATIVE_RENDERER=NOT_INCLUDED"

ALTSCREEN_SD_VOLUME="$VOLUME" /bin/sh "$CONTROLLER" install "${1:-}"
CHAIN_RC=$?
[ "$CHAIN_RC" -eq 0 ] || exit "$CHAIN_RC"

APP_RW=0
TMP="$JAR_TARGET.basevideo3.tmp"
rollback(){
    echo "WARN: Java80 deployment failed; removing deployed JAR and restoring native state"
    if [ "$APP_RW" != 1 ]; then
        if mount_app_rw >/dev/null 2>&1; then APP_RW=1; fi
    fi
    if [ "$APP_RW" = 1 ]; then
        rm -f "$TMP" "$JAR_TARGET" || echo "WARN: Java HMI JAR removal failed"
        sync >/dev/null 2>&1 || true
        mount_app_ro >/dev/null 2>&1 || true
        APP_RW=0
    fi
    ALTSCREEN_SD_VOLUME="$VOLUME" /bin/sh "$CONTROLLER" restore >/dev/null 2>&1 || echo "WARN: native rollback failed; runtime remains inert until RESTORE ORIGINAL succeeds"
}
fail(){ msg=$1; rollback; echo "FAIL: $msg" >&2; exit 1; }

MIRROR_RUNTIME="$DEVICE_ROOT/mnt/app/root/carplay-altscreen/bin/mirror"
[ -x "$MIRROR_RUNTIME/carplay-alt111-mirror-display" ] || fail "integrated direct-display binary was not staged"
[ -x "$MIRROR_RUNTIME/start_vehicle.sh" ] || fail "integrated direct-display launcher was not staged"

mount_app_rw || fail "cannot mount /mnt/app writable"
APP_RW=1
mkdir -p "$JAR_TARGET_DIR" || fail "cannot create HMI JAR directory"
rm -f "$TMP" 2>/dev/null || true
cp "$JAR_SOURCE" "$TMP" || fail "cannot stage Java80 HMI JAR"
chmod 644 "$TMP" || fail "cannot chmod Java80 HMI JAR"
jar_valid "$TMP" || fail "staged Java80 HMI JAR identity check failed"
mv "$TMP" "$JAR_TARGET" || fail "cannot publish Java80 HMI JAR"
jar_valid "$JAR_TARGET" || fail "installed Java80 HMI JAR identity check failed"
sync || fail "sync failed after Java80 HMI install"
mount_app_ro || fail "cannot remount /mnt/app read-only"
APP_RW=0

echo "HMI_CONTROL_PLANE=INSTALLED target=/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar size=$EXPECTED_SIZE cksum=$EXPECTED_CKSUM"
echo "HMI_CONTRACT=JAVA80 ctx80=98,101,102,3 basevideo=3"
echo "INSTALL=PASS integrated=AltScreen+H264Tap+DecoderTap+Displayable3+Java80 reboot_required=YES"
exit 0
