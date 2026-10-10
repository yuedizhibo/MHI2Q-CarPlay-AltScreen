#!/bin/sh
# CarPlay private111 Direct Display V3 wheel-zoom INSTALL.
# The proven V2 display chain remains unchanged; V3 adds the true CarPlay
# changeMapZoomLevel control plane and refuses to install an unbuilt V2 HMI JAR.
# Installs the type111 control/data plane, H264/decoded SHM bridge,
# displayable3 GLES sidecar, and Java80 HMI control plane.
# Window58 readback and RGI98 native renderer are not used by the sidecar.
set -u

ALTS_INSTALL_RGI_MODE=${ALTS_INSTALL_RGI_MODE:-WITH}
case "$ALTS_INSTALL_RGI_MODE" in NO|WITH) ;; *) echo "INSTALL=REFUSED reason=INVALID_RGI_MODE" >&2; exit 2 ;; esac
export ALTS_INSTALL_RGI_MODE

# QNX compatibility: treat already-existing directories as success instead of
# relying on target mkdir -p return semantics.
ensure_dirs() {
    for dir in "$@"; do
        [ -d "$dir" ] && continue
        mkdir -p "$dir" || return 1
    done
    return 0
}

# Persist the complete INSTALL transaction on the currently inserted SD card.
# This wrapper runs before any production mutation.  The child re-enters the
# same script with ALTS_OPLOG_CAPTURED=1 so every stdout/stderr line from this
# installer and its nested controllers is captured in one operation log.
if [ "${ALTS_OPLOG_CAPTURED:-0}" != 1 ]; then
    CAPTURE_ENTRY="$0"
    RESOLVED_CAPTURE=$(command -v -- "$CAPTURE_ENTRY" 2>/dev/null)
    [ -n "$RESOLVED_CAPTURE" ] && CAPTURE_ENTRY="$RESOLVED_CAPTURE"

    journal_volume=""
    if [ "${ALTSCREEN_CHAIN_TESTING:-0}" = 1 ]; then
        journal_volume=${ALTSCREEN_CHAIN_VOLUME:-}
        case "$journal_volume" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_VOLUME"; exit 2 ;; esac
    else
        for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
            if [ -d "$candidate/Toolbox" ]; then journal_volume=$candidate; break; fi
        done
    fi
    [ -n "$journal_volume" ] && [ -d "$journal_volume/Toolbox" ] || {
        echo "FAIL: no Toolbox SD card discovered; INSTALL not started and no production files changed"
        exit 1
    }
    SD_RW_HELPER="$journal_volume/Toolbox/scripts/altscreen_sd_writable.sh"
    [ -f "$SD_RW_HELPER" ] || { echo "INSTALL=REFUSED reason=SD_WRITABLE_HELPER_MISSING production_changed=NO"; exit 127; }
    . "$SD_RW_HELPER"
    altscreen_sd_ensure_writable "$journal_volume" INSTALL_JOURNAL || {
        echo "INSTALL=REFUSED reason=SD_NOT_WRITABLE production_changed=NO"
        exit 1
    }

    journal_dir="$journal_volume/MMI-Cockpit-Carplay/logs/operations"
    ensure_dirs "$journal_dir" 2>/dev/null || {
        echo "FAIL: cannot create persistent INSTALL log directory on SD: $journal_dir"
        exit 1
    }
    journal_stamp=$(date +%Y%m%d_%H%M%S 2>/dev/null || echo unknown)
    journal_base="$journal_dir/install_${journal_stamp}"
    journal="$journal_base.log"
    journal_n=0
    while [ -e "$journal" ]; do
        journal_n=$((journal_n + 1))
        journal="${journal_base}_${journal_n}.log"
    done
    if ! (printf 'OP_BEGIN action=INSTALL script=%s storage=SD\n' "$CAPTURE_ENTRY" > "$journal") 2>/dev/null; then
        echo "FAIL: cannot create persistent INSTALL log on SD: $journal"
        exit 1
    fi
    printf 'DIAGNOSTICS_VOLUME=%s\n' "$journal_volume" >> "$journal"

    if [ "$#" -gt 0 ]; then
        ALTS_OPLOG_CAPTURED=1 /bin/sh "$CAPTURE_ENTRY" "$@" >> "$journal" 2>&1
    else
        ALTS_OPLOG_CAPTURED=1 /bin/sh "$CAPTURE_ENTRY" >> "$journal" 2>&1
    fi
    journal_rc=$?
    printf 'OP_END action=INSTALL rc=%s\n' "$journal_rc" >> "$journal"
    printf 'OPERATION_LOG=%s\n' "$journal" >> "$journal"
    sync >/dev/null 2>&1 || true
    cat "$journal"
    exit "$journal_rc"
fi

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
        if [ -d "$candidate/Toolbox" ]; then VOLUME=$candidate; break; fi
    done
fi

[ -n "$VOLUME" ] || { echo "FAIL: no Toolbox SD card discovered"; exit 1; }

# All production mutations are owned by the persistent INSTALL transaction.
# The outer operation-log wrapper re-enters this script once; that child then
# hands control to the transaction wrapper.  The transaction calls back with
# ALTS_INSTALL_TXN_ACTIVE=1 for the actual APPLY step.
if [ "${ALTS_INSTALL_TXN_ACTIVE:-0}" != 1 ] && [ "${ALTS_PACKAGE_PREFLIGHT:-0}" != 1 ]; then
    INSTALL_TXN="$VOLUME/Toolbox/scripts/altscreen_install_transaction.sh"
    [ -f "$INSTALL_TXN" ] || {
        echo "FAIL: transactional INSTALL wrapper missing: $INSTALL_TXN"
        exit 127
    }
    if [ "$#" -gt 0 ]; then
        exec /bin/sh "$INSTALL_TXN" install "$@"
    else
        exec /bin/sh "$INSTALL_TXN" install
    fi
fi

CONTROLLER="$VOLUME/Toolbox/scripts/altscreen_chain_test.sh"
MIRROR_RELEASE="$VOLUME/Toolbox/carplay_alt_screen/mirror_display/release"
MIRROR_INFO="$MIRROR_RELEASE/BUILD_INFO.txt"
JAR_SOURCE="$VOLUME/Toolbox/carplay_alt_screen/hmi/carplay_hook-basevideo3.jar"
HMI_INFO="$VOLUME/Toolbox/carplay_alt_screen/hmi/BUILD_INFO.txt"
JAR_TARGET="$DEVICE_ROOT/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar"
JAR_TARGET_DIR=$(dirname -- "$JAR_TARGET")
EXPECTED_SIZE=$(sed -n 's/^jar_size=//p' "$HMI_INFO")
EXPECTED_CKSUM=$(sed -n 's/^jar_cksum=//p' "$HMI_INFO")
case "$EXPECTED_SIZE:$EXPECTED_CKSUM" in *[!0-9:]*|:*|*:) echo "FAIL: invalid HMI identity metadata" >&2; exit 1 ;; esac
[ "$EXPECTED_SIZE" -gt 0 ] || exit 1

[ -f "$CONTROLLER" ] || { echo "FAIL: chain controller missing: $CONTROLLER"; exit 127; }
[ -s "$JAR_SOURCE" ] || { echo "FAIL: Java80 HMI JAR missing: $JAR_SOURCE"; exit 1; }
[ -s "$HMI_INFO" ] || { echo "FAIL: HMI BUILD_INFO missing: $HMI_INFO"; exit 1; }
grep -Fq 'oem_geometry_build_status=COMPILED_OBSERVER_READY' "$HMI_INFO" 2>/dev/null || {
    echo "FAIL: OEM observer source/JAR is not a compiled matched pair"
    grep -E '^(oem_geometry_build_status|jar_size|jar_cksum|jar_sha256)=' "$HMI_INFO" 2>/dev/null || true
    echo "ACTION=RUN_OEM_LAYOUT_OBSERVER_BUILD_BEFORE_INSTALL"
    exit 1
}
grep -Eq '^mode=(PRIVATE111_DIRECT_DISPLAY_V3_WHEEL_ZOOM|PRIVATE111_DIRECT_DISPLAY_V3_3_OEM_LOWER_BAR|PRIVATE111_DIRECT_DISPLAY_V3_4_STREAM_DRIVEN|PRIVATE111_DIRECT_DISPLAY_V3_5_COLD_START)$' "$HMI_INFO" 2>/dev/null &&
grep -Fq 'wheel_zoom_build_status=COMPILED_READY_FOR_VEHICLE_TEST' "$HMI_INFO" 2>/dev/null || {
    echo "FAIL: V3/V3.3/V3.4/V3.5 HMI artifact is not the compiled vehicle-test build"
    grep -E '^(mode|wheel_zoom_build_status|jar_size|jar_cksum|jar_sha256)=' "$HMI_INFO" 2>/dev/null || true
    echo "ACTION=RUN_WHEEL_ZOOM_V3_BUILD"
    exit 1
}
[ -s "$MIRROR_INFO" ] || { echo "FAIL: V2 Mirror BUILD_INFO missing: $MIRROR_INFO"; exit 1; }
grep -Fq 'release_binary_status=PRIVATE111_DIRECT_DISPLAY_V3_5' "$MIRROR_INFO" 2>/dev/null &&
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

# One-step upgrade must validate the same package identity and complete runtime
# requirements before removing the old installation. This branch is read-only.
if [ "${ALTS_PACKAGE_PREFLIGHT:-0}" = 1 ]; then
    /bin/sh "$CONTROLLER" package-precheck || exit 1
    echo "INSTALL_PACKAGE_PREFLIGHT=PASS production_changed=NO"
    exit 0
fi

echo "PACKAGE_MODE=CARPLAY_PRIVATE111_DIRECT_DISPLAY_V3_5_COLD_START"
echo "NATIVE_SOURCE=private111_ScreenStreamProcessData h264_shm=/carplay111_h264"
echo "DECODER_BACKEND=stock_omx_screen_linearized_shm decoded_shm=/carplay111_decoded"
echo "PIXEL_BRIDGE=Screen_linearized_NV12_to_existing_MMI_GLES"
echo "PIXEL_TARGET=displayable3"
echo "HMI_CONTEXT=ctx80"
echo "WINDOW58_READBACK=DISABLED"
echo "DIRECT_DISPLAY_SIDECAR=INCLUDED"
echo "INSTALL_RGI_MODE=$ALTS_INSTALL_RGI_MODE reboot_required=YES"

echo "HMI_JAR_POLICY=PROJECT_OWNED install=CREATE_OR_REPLACE uninstall=DELETE permanent_oem_backup=NO rollback=INSTALL_TRANSACTION_SNAPSHOT"

/bin/sh "$CONTROLLER" install "${1:-}"
CHAIN_RC=$?
[ "$CHAIN_RC" -eq 0 ] || exit "$CHAIN_RC"
MIRROR_RUNTIME="$DEVICE_ROOT/mnt/app/root/carplay-altscreen/bin/mirror"
[ -x "$MIRROR_RUNTIME/carplay-alt111-mirror-display" ] || { echo "FAIL: integrated direct-display binary was not staged"; exit 1; }
[ -x "$MIRROR_RUNTIME/start_vehicle.sh" ] || { echo "FAIL: integrated direct-display launcher was not staged"; exit 1; }
[ -x "$MIRROR_RUNTIME/stream_supervisor.sh" ] || { echo "FAIL: V3.5 stream supervisor was not staged"; exit 1; }

fail(){
    msg=$1
    mount_app_ro >/dev/null 2>&1 || true
    echo "FAIL: $msg; INSTALL transaction will roll back" >&2
    exit 1
}

mount_app_rw || fail "cannot mount /mnt/app writable"
ensure_dirs "$JAR_TARGET_DIR" || fail "cannot create HMI JAR directory"
TMP="$JAR_TARGET.basevideo3.tmp"
rm -f "$TMP" 2>/dev/null || true
cp "$JAR_SOURCE" "$TMP" || fail "cannot stage Java80 HMI JAR"
chmod 644 "$TMP" || fail "cannot chmod Java80 HMI JAR"
jar_valid "$TMP" || fail "staged Java80 HMI JAR identity check failed"
mv "$TMP" "$JAR_TARGET" || fail "cannot publish Java80 HMI JAR"
jar_valid "$JAR_TARGET" || fail "installed Java80 HMI JAR identity check failed"
sync || fail "sync failed after Java80 HMI install"
mount_app_ro || fail "cannot remount /mnt/app read-only"

echo "HMI_CONTROL_PLANE=INSTALLED target=/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar size=$EXPECTED_SIZE cksum=$EXPECTED_CKSUM"
echo "HMI_CONTRACT=JAVA80 ctx80=98,101,102,3 basevideo=3"
echo "V35_STARTUP_POLICY=early_negotiation_ready display_stream_driven fixed_delay=NONE sd_runtime_gate=DISABLED geometry_gate=ASYNC"
echo "INSTALL=PASS integrated=AltScreen+H264Tap+DecoderTap+StreamSupervisor+Displayable3+Java80 reboot_required=YES"
exit 0
