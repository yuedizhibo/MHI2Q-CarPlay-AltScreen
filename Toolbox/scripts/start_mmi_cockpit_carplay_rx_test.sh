#!/bin/sh
# private111 direct-display START.
# Arms type111 + H264/decoded SHM taps. Java/HMI remains the sole terminal1/ctx80
# owner; /tmp/mmi-mirror-basevideo.ready is published only after displayable3
# has successfully presented its first decoded frame.
set -u

# QNX compatibility: some target mkdir implementations return EEXIST for
# "mkdir -p" when the final directory already exists. Never treat that as a
# failed idempotent directory creation.
ensure_dirs() {
    for dir in "$@"; do
        [ -d "$dir" ] && continue
        mkdir -p "$dir" || return 1
    done
    return 0
}

# Capture the whole START transaction before any forwarding or mount mutation.
# The volatile copy survives long enough for boot diagnostics to flush it to SD;
# when the SD is already writable we also publish it immediately.
if [ "${ALTS_START_CAPTURED:-0}" != 1 ]; then
    CAPTURE_ENTRY="$0"
    RESOLVED_CAPTURE=$(command -v -- "$CAPTURE_ENTRY" 2>/dev/null)
    [ -n "$RESOLVED_CAPTURE" ] && CAPTURE_ENTRY="$RESOLVED_CAPTURE"

    journal_root=""
    if [ "${ALTSCREEN_CHAIN_TESTING:-0}" = 1 ]; then
        journal_root=${ALTSCREEN_CHAIN_ROOT:-}
        case "$journal_root" in /tmp/*|/var/tmp/*) ;; *) exit 2 ;; esac
    fi
    journal_stamp=$(date +%Y%m%d_%H%M%S 2>/dev/null || echo unknown)
    journal_name="start_${journal_stamp}_$$.log"
    journal_volume=""
    if [ "${ALTSCREEN_CHAIN_TESTING:-0}" = 1 ]; then
        journal_volume=${ALTSCREEN_CHAIN_VOLUME:-}
    else
        for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
            if [ -d "$candidate/Toolbox" ]; then journal_volume=$candidate; break; fi
        done
    fi
    journal_storage=TMP
    if [ -n "$journal_volume" ] && [ -d "$journal_volume/Toolbox" ]; then
        SD_RW_HELPER="$journal_volume/Toolbox/scripts/altscreen_sd_writable.sh"
        [ -f "$SD_RW_HELPER" ] || { echo "START_FAIL_STAGE=SD_WRITABLE rc=127 reason=SD_WRITABLE_HELPER_MISSING"; exit 127; }
        . "$SD_RW_HELPER"
        altscreen_sd_ensure_writable "$journal_volume" START_JOURNAL || {
            echo "START_FAIL_STAGE=SD_WRITABLE rc=1 reason=SD_NOT_WRITABLE"
            exit 1
        }
        journal_dir="$journal_volume/MMI-Cockpit-Carplay/logs/operations"
        if ensure_dirs "$journal_dir" 2>/dev/null; then
            journal="$journal_dir/$journal_name"
            journal_storage=SD
        fi
    fi
    if [ "$journal_storage" = TMP ]; then
        journal="$journal_root/tmp/altscreen_$journal_name"
    fi

    journal_opened=0
    if (printf 'OP_BEGIN action=START script=%s storage=%s\n' "$CAPTURE_ENTRY" "$journal_storage" > "$journal") 2>/dev/null; then
        journal_opened=1
    elif [ "$journal_storage" = SD ]; then
        # A card may be present but temporarily read-only/unwritable. Logging
        # must degrade to volatile storage instead of running unjournaled.
        journal_storage=TMP
        journal="$journal_root/tmp/altscreen_$journal_name"
        if (printf 'OP_BEGIN action=START script=%s storage=%s\n' "$CAPTURE_ENTRY" "$journal_storage" > "$journal") 2>/dev/null; then
            printf 'START_JOURNAL_FALLBACK=TMP reason=sd_write_failed\n' >> "$journal" 2>/dev/null || true
            journal_opened=1
        fi
    fi
    if [ "$journal_opened" != 1 ]; then
        echo "WARN: START diagnostic journal unavailable on SD and /tmp; continuing operation"
        ALTS_START_CAPTURED=1; export ALTS_START_CAPTURED
        if [ "$#" -gt 0 ]; then exec /bin/sh "$CAPTURE_ENTRY" "$@"; else exec /bin/sh "$CAPTURE_ENTRY"; fi
    fi

    if [ "$#" -gt 0 ]; then
        ALTS_START_CAPTURED=1 /bin/sh "$CAPTURE_ENTRY" "$@" >> "$journal" 2>&1
    else
        ALTS_START_CAPTURED=1 /bin/sh "$CAPTURE_ENTRY" >> "$journal" 2>&1
    fi
    journal_rc=$?
    printf 'OP_END action=START rc=%s\n' "$journal_rc" >> "$journal"

    if [ -n "$journal_volume" ] && [ -d "$journal_volume/Toolbox" ]; then
        printf 'DIAGNOSTICS_VOLUME=%s\n' "$journal_volume" >> "$journal"
        if [ "$journal_storage" = TMP ]; then
            journal_target_dir="$journal_volume/MMI-Cockpit-Carplay/logs/operations"
            if ensure_dirs "$journal_target_dir" 2>/dev/null; then
                cp "$journal" "$journal_target_dir/$journal_name.new" 2>/dev/null &&
                    mv "$journal_target_dir/$journal_name.new" "$journal_target_dir/$journal_name" 2>/dev/null ||
                    rm -f "$journal_target_dir/$journal_name.new" 2>/dev/null || true
            fi
        fi
    fi
    cat "$journal"
    exit "$journal_rc"
fi

BASE="$0"
RESOLVED=$(command -v -- "$BASE" 2>/dev/null)
[ -n "$RESOLVED" ] || RESOLVED="$BASE"
SCRIPTDIR=$(cd -P -- "$(dirname -- "$RESOLVED")" 2>/dev/null && pwd -P)
[ -n "$SCRIPTDIR" ] || { echo "FAIL: cannot resolve START directory"; exit 126; }

TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
DEVICE_ROOT=""
VOLUME=""
if [ "$TESTING" = 1 ]; then
    DEVICE_ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
    case "$DEVICE_ROOT" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_ROOT"; exit 2 ;; esac
    case "$VOLUME" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_VOLUME"; exit 2 ;; esac
else
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -d "$candidate/Toolbox" ]; then VOLUME=$candidate; break; fi
    done
fi
[ -n "$VOLUME" ] || { echo "START_FAIL_STAGE=DISCOVER_SD rc=1 reason=no_Toolbox_SD"; echo "FAIL: no Toolbox SD card discovered"; exit 1; }
SD_RW_HELPER="$VOLUME/Toolbox/scripts/altscreen_sd_writable.sh"
[ -f "$SD_RW_HELPER" ] || { echo "START_FAIL_STAGE=SD_WRITABLE rc=127 reason=SD_WRITABLE_HELPER_MISSING"; exit 127; }
. "$SD_RW_HELPER"
altscreen_sd_ensure_writable "$VOLUME" START_RUNTIME || { echo "START_FAIL_STAGE=SD_WRITABLE rc=1 reason=SD_NOT_WRITABLE"; exit 1; }
echo "DIAGNOSTICS_VOLUME=$VOLUME"
CHAIN_STATE="$VOLUME/MMI-Cockpit-Carplay/state"

APP_BIN="$DEVICE_ROOT/mnt/app/root/carplay-altscreen/bin"
APP_SELF="$APP_BIN/start_mmi_cockpit_carplay_rx_test.sh"
if [ "$SCRIPTDIR" != "$APP_BIN" ] && [ -f "$APP_SELF" ] && [ -f "$APP_BIN/altscreen_chain_test.sh" ]; then
    echo "APP_RUNTIME_FORWARD action=START from=$SCRIPTDIR to=/mnt/app/root/carplay-altscreen/bin"
    if [ "$#" -gt 0 ]; then
        exec /bin/sh "$APP_SELF" "$@"
    else
        exec /bin/sh "$APP_SELF"
    fi
fi

CONTROLLER="$SCRIPTDIR/altscreen_chain_test.sh"
[ -f "$CONTROLLER" ] || { echo "FAIL: installed chain controller missing"; exit 127; }

RUNTIME="$DEVICE_ROOT/mnt/app/root/carplay-altscreen"
STATE="$RUNTIME/state"
ENABLED="$STATE/basevideo3.enabled"
JAR="$DEVICE_ROOT/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar"
HMI_INFO="$STATE/hmi-build-info.txt"
EXPECTED_SIZE=$(sed -n 's/^jar_size=//p' "$HMI_INFO" 2>/dev/null || true)
EXPECTED_CKSUM=$(sed -n 's/^jar_cksum=//p' "$HMI_INFO" 2>/dev/null || true)
case "$EXPECTED_SIZE:$EXPECTED_CKSUM" in *[!0-9:]*|:*|*:) echo "FAIL: invalid installed HMI identity metadata; run INSTALL" >&2; exit 1 ;; esac
[ "$EXPECTED_SIZE" -gt 0 ] || exit 1
ACTIVE="$DEVICE_ROOT/tmp/mmi-mirror-active"
READY="$DEVICE_ROOT/tmp/mmi-mirror-basevideo.ready"
STARTED="$DEVICE_ROOT/tmp/mmi-mirror-controller.started"
MIRROR="$RUNTIME/bin/mirror"
MIRROR_START="$MIRROR/start_vehicle.sh"
MIRROR_STOP="$MIRROR/stop_vehicle.sh"
MIRROR_SUPERVISOR="$MIRROR/stream_supervisor.sh"
MIRROR_PID="$DEVICE_ROOT/tmp/altscreen_mirror.pid"
MIRROR_LOG="$DEVICE_ROOT/tmp/altscreen_mirror.log"
SUPERVISOR_PID="$DEVICE_ROOT/tmp/altscreen_stream_supervisor.pid"
SUPERVISOR_LOG="$DEVICE_ROOT/tmp/altscreen_stream_supervisor.log"
STREAM_READY="$DEVICE_ROOT/tmp/altscreen-private111.stream-ready"

file_size(){ n=$(wc -c < "$1" 2>/dev/null) || { echo 0; return; }; set -- $n; echo "${1:-0}"; }
file_cksum(){ if command -v cksum >/dev/null 2>&1; then cksum < "$1" 2>/dev/null | awk '{print $1}'; else echo unavailable; fi; }
jar_valid(){
    [ -s "$JAR" ] || return 1
    [ "$(file_size "$JAR")" = "$EXPECTED_SIZE" ] || return 1
    sum=$(file_cksum "$JAR")
    [ "$sum" = unavailable ] || [ "$sum" = "$EXPECTED_CKSUM" ]
}
probe_writable_dir(){
    probe_dir=$1
    [ -d "$probe_dir" ] || return 1
    probe_file="$probe_dir/.altscreen-start-write.$"
    if ( : > "$probe_file" ) 2>/dev/null; then
        rm -f "$probe_file" 2>/dev/null || true
        return 0
    fi
    return 1
}
mount_app_rw(){
    [ "$TESTING" = 1 ] && return 0
    probe_writable_dir "$DEVICE_ROOT/mnt/app/root" && return 0
    mount -uw /mnt/app >/dev/null 2>&1 || true
    probe_writable_dir "$DEVICE_ROOT/mnt/app/root"
}
mount_app_ro(){
    [ "$TESTING" = 1 ] && return 0
    mount -ur /mnt/app >/dev/null 2>&1 || true
    if probe_writable_dir "$DEVICE_ROOT/mnt/app/root"; then return 1; fi
    return 0
}
SYSTEM_PROBE_DIR=""
mount_system_rw(){
    [ "$TESTING" = 1 ] && return 0
    [ -n "$SYSTEM_PROBE_DIR" ] && probe_writable_dir "$SYSTEM_PROBE_DIR" && return 0
    mount -uw /mnt/system >/dev/null 2>&1 || true
    [ -n "$SYSTEM_PROBE_DIR" ] && probe_writable_dir "$SYSTEM_PROBE_DIR"
}
mount_system_ro(){
    [ "$TESTING" = 1 ] && return 0
    mount -ur /mnt/system >/dev/null 2>&1 || true
    if [ -n "$SYSTEM_PROBE_DIR" ] && probe_writable_dir "$SYSTEM_PROBE_DIR"; then return 1; fi
    return 0
}

CURRENT_STAGE=INIT
stage(){
    CURRENT_STAGE=$1
    echo "START_STAGE=$CURRENT_STAGE"
}
pre_fail(){
    echo "START_FAIL_STAGE=$CURRENT_STAGE rc=1 reason=$1"
    echo "FAIL: $1" >&2
    exit 1
}

stage PRECHECK_JAR
if ! jar_valid; then
    echo "expected_size=$EXPECTED_SIZE expected_cksum=$EXPECTED_CKSUM"
    [ -f "$JAR" ] && echo "actual_size=$(file_size "$JAR") actual_cksum=$(file_cksum "$JAR")"
    pre_fail "Java80 HMI JAR missing or mismatched"
fi

stage PRECHECK_SIDECAR
[ -x "$MIRROR/carplay-alt111-mirror-display" ] || pre_fail "direct-display sidecar binary missing"
[ -x "$MIRROR_START" ] || pre_fail "direct-display sidecar launcher missing"
[ -x "$MIRROR_SUPERVISOR" ] || pre_fail "V3.5 stream supervisor missing"

stage LOCATE_STARTUP
STARTUP=""
for candidate in "$DEVICE_ROOT/mnt/system/etc/boot/startup.sh" "$DEVICE_ROOT/etc/boot/startup.sh"; do
    if [ -f "$candidate" ]; then STARTUP=$candidate; break; fi
done
[ -n "$STARTUP" ] || pre_fail "startup.sh not found"
SYSTEM_PROBE_DIR=${STARTUP%/*}

strip_blocks(){
    awk '
      {
        key=$0
        sub(/\r$/, "", key)
        trimmed=key
        gsub(/^[ \t]+/, "", trimmed)
        gsub(/[ \t]+$/, "", trimmed)

        if (trimmed == "# BEGIN ALT111 MIRROR AUTOSTART") {
            if (block != "") bad=8
            block="old"
            next
        }
        if (trimmed == "# END ALT111 MIRROR AUTOSTART") {
            if (block != "old") bad=8
            block=""
            next
        }
        if (trimmed == "# BEGIN ALT111 BASEVIDEO3 AUTOSTART") {
            if (block != "") bad=8
            block="new"
            next
        }
        if (trimmed == "# END ALT111 BASEVIDEO3 AUTOSTART") {
            if (block != "new") bad=8
            block=""
            next
        }
        if (block == "") print
      }
      END {
        if (bad) exit bad
        if (block != "") exit 9
      }
    ' "$1"
}

verify_autostart_contract(){
    awk '
      $0 == "# BEGIN ALT111 BASEVIDEO3 AUTOSTART" { begin_new++ }
      $0 == "# END ALT111 BASEVIDEO3 AUTOSTART" { end_new++ }
      $0 == "# BEGIN ALT111 MIRROR AUTOSTART" { begin_old++ }
      $0 == "# END ALT111 MIRROR AUTOSTART" { end_old++ }
      /\/mnt\/app\/root\/carplay-altscreen\/state\/basevideo3.enabled/ { enabled++ }
      /\/mnt\/app\/root\/carplay-altscreen\/bin\/mirror\/stream_supervisor.sh/ { launcher++ }
      /\/tmp\/altscreen_autostart.log/ { autolog++ }
      END {
        if (begin_new != 1 || end_new != 1 || begin_old != 0 || end_old != 0 ||
            enabled < 1 || launcher < 1 || autolog < 1) exit 1
      }
    ' "$1"
}

CLEAN="$DEVICE_ROOT/tmp/altscreen_start_$$.clean"
BLOCK="$DEVICE_ROOT/tmp/altscreen_start_$$.block"
NEW="$DEVICE_ROOT/tmp/altscreen_start_$$.new"
ORIGINAL="$DEVICE_ROOT/tmp/altscreen_start_$$.original"

system_space_snapshot(){
    label=$1
    target="$DEVICE_ROOT/mnt/system"
    echo "SYSTEM_SPACE_BEGIN label=$label path=$target"
    df -k "$target" 2>/dev/null || df "$target" 2>/dev/null || true
    echo "SYSTEM_SPACE_END label=$label"
}
cleanup_legacy_system_staging(){
    rm -f "$STARTUP.basevideo3.clean."* "$STARTUP.basevideo3.block."*           "$STARTUP.basevideo3.new."* "$STARTUP.basevideo3.original."*           "$STARTUP.basevideo3.restore."*           "$SYSTEM_PROBE_DIR/.${STARTUP##*/}.altscreen.new."* 2>/dev/null || true
}
publish_system_file(){
    src=$1; dst=$2; mode=$3; dir=${dst%/*}; base=${dst##*/}
    tmp="$dir/.$base.altscreen.new.$$"
    if cmp -s "$src" "$dst" 2>/dev/null; then
        chmod "$mode" "$dst" 2>/dev/null || return 1
        echo "SYSTEM_PUBLISH=SKIP_IDENTICAL target=$dst"
        return 0
    fi
    rm -f "$tmp" 2>/dev/null || true
    if cp "$src" "$tmp"; then
        :
    else
        rc=$?
        rm -f "$tmp" 2>/dev/null || true
        echo "SYSTEM_WRITE_FAILED stage=copy target=$dst"
        system_space_snapshot publish_copy_failed
        return "$rc"
    fi
    chmod "$mode" "$tmp" || { rc=$?; rm -f "$tmp" 2>/dev/null || true; return "$rc"; }
    cmp -s "$src" "$tmp" || { rm -f "$tmp" 2>/dev/null || true; return 1; }
    mv "$tmp" "$dst" || { rc=$?; rm -f "$tmp" 2>/dev/null || true; return "$rc"; }
    return 0
}

APP_RW=0
SYSTEM_RW=0
STARTUP_CHANGED=0
ENABLED_CREATED=0
ACTIVE_CREATED=0
READY_CLEARED=0
CONTROLLER_ATTEMPTED=0
CHAIN_WAS_ACTIVE=0
CURRENT_ACTIVE_WAS_PRESENT=0
READY_WAS_PRESENT=0
MIRROR_WAS_RUNNING=0

[ -f "$CHAIN_STATE/ACTIVE" ] && CHAIN_WAS_ACTIVE=1
[ -f "$ACTIVE" ] && CURRENT_ACTIVE_WAS_PRESENT=1
[ -f "$READY" ] && READY_WAS_PRESENT=1
if [ -f "$MIRROR_PID" ]; then
    old_mirror_pid=$(cat "$MIRROR_PID" 2>/dev/null || true)
    if [ -n "$old_mirror_pid" ] && kill -0 "$old_mirror_pid" 2>/dev/null; then MIRROR_WAS_RUNNING=1; fi
fi

rollback_controller_start(){
    [ "$CONTROLLER_ATTEMPTED" = 1 ] || return 0
    [ "$CHAIN_WAS_ACTIVE" != 1 ] || { echo "START_ROLLBACK_CONTROLLER=PRESERVED_PREEXISTING"; return 0; }

    rm -f "$CHAIN_STATE/ARMED" "$CHAIN_STATE/ARMED_MUTATE" "$CHAIN_STATE/ARMED_INFO"           "$CHAIN_STATE/ARMED_FEATURE" "$CHAIN_STATE/ARMED_CREATE111"           "$CHAIN_STATE/ACTIVE" "$CHAIN_STATE/FORCE_START"           "$CHAIN_STATE/run_id" "$CHAIN_STATE/session_path" 2>/dev/null || true

    if mount_app_rw >/dev/null 2>&1; then
        APP_RW=1
        rm -f "$RUNTIME/state/fullchain_probe" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        APP_RW=0
    fi
    echo "START_ROLLBACK_CONTROLLER=DISARMED_NEW_TRANSACTION"
}

cleanup(){ rm -f "$CLEAN" "$BLOCK" "$NEW" "$ORIGINAL" 2>/dev/null || true; }

rollback(){
    echo "START_ROLLBACK_BEGIN stage=$CURRENT_STAGE"

    if [ "$MIRROR_WAS_RUNNING" != 1 ] && [ -x "$MIRROR_STOP" ]; then
        /bin/sh "$MIRROR_STOP" >/dev/null 2>&1 || true
    fi
    rollback_controller_start

    if [ "$STARTUP_CHANGED" = 1 ] && [ -f "$ORIGINAL" ]; then
        [ "$SYSTEM_RW" = 1 ] || { mount_system_rw >/dev/null 2>&1 && SYSTEM_RW=1; }
        if [ "$SYSTEM_RW" = 1 ] &&
           publish_system_file "$ORIGINAL" "$STARTUP" 755 >/dev/null 2>&1 &&
           cmp -s "$ORIGINAL" "$STARTUP" 2>/dev/null; then
            echo "START_ROLLBACK_AUTOSTART=RESTORED"
        else
            echo "START_ROLLBACK_AUTOSTART=RESTORE_FAILED"
        fi
    fi
    [ "$SYSTEM_RW" != 1 ] || { mount_system_ro >/dev/null 2>&1 || true; SYSTEM_RW=0; }

    if [ "$ENABLED_CREATED" = 1 ]; then
        [ "$APP_RW" = 1 ] || { mount_app_rw >/dev/null 2>&1 && APP_RW=1; }
        [ "$APP_RW" != 1 ] || rm -f "$ENABLED" >/dev/null 2>&1 || true
    fi
    [ "$APP_RW" != 1 ] || { mount_app_ro >/dev/null 2>&1 || true; APP_RW=0; }

    if [ "$ACTIVE_CREATED" = 1 ]; then
        rm -f "$ACTIVE" 2>/dev/null || true
    elif [ "$CURRENT_ACTIVE_WAS_PRESENT" = 1 ]; then
        touch "$ACTIVE" 2>/dev/null || true
    fi

    if [ "$READY_CLEARED" = 1 ]; then
        if [ "$READY_WAS_PRESENT" = 1 ]; then touch "$READY" 2>/dev/null || true
        else rm -f "$READY" 2>/dev/null || true
        fi
    fi

    cleanup
    echo "START_ROLLBACK_END stage=$CURRENT_STAGE"
}

fail(){
    msg=$1
    echo "START_FAIL_STAGE=$CURRENT_STAGE rc=1 reason=$msg"
    rollback
    echo "FAIL: $msg" >&2
    exit 1
}
fail_rc(){
    rc=$1
    shift
    msg=$*
    echo "START_FAIL_STAGE=$CURRENT_STAGE rc=$rc reason=$msg"
    rollback
    echo "FAIL: $msg" >&2
    exit "$rc"
}
on_signal(){
    trap - 0 1 2 15
    echo "START_FAIL_STAGE=$CURRENT_STAGE rc=130 reason=signal_interrupt"
    rollback
    exit 130
}
trap cleanup 0
trap on_signal 1 2 15

stage APP_STATE_RW
mount_app_rw || fail "cannot mount /mnt/app writable"
APP_RW=1

stage ENABLE_BOOT_DEMAND
ensure_dirs "$STATE" || fail "cannot create runtime state directory"
if [ ! -f "$ENABLED" ]; then
    touch "$ENABLED" || fail "cannot enable BaseVideo3 boot demand"
    ENABLED_CREATED=1
else
    echo "START_COMPAT=BOOT_DEMAND_ALREADY_ENABLED"
fi
sync >/dev/null 2>&1 || true
mount_app_ro || fail "cannot remount /mnt/app read-only"
APP_RW=0

stage SNAPSHOT_STARTUP
system_space_snapshot start_begin
cp "$STARTUP" "$ORIGINAL" || fail "cannot snapshot startup.sh into volatile transaction storage"

stage STRIP_AUTOSTART
strip_blocks "$STARTUP" > "$CLEAN" || fail "invalid existing BaseVideo3/Mirror autostart block"

stage COMPOSE_AUTOSTART
cat > "$BLOCK" <<'BASEVIDEO3_BOOT'
# BEGIN ALT111 BASEVIDEO3 AUTOSTART
if [ -f /mnt/app/root/carplay-altscreen/state/basevideo3.enabled ]; then
    (
        AUTOLOG=/tmp/altscreen_autostart.log
        echo "AUTOSTART_BEGIN component=private111_stream_supervisor policy=stream_driven_no_fixed_delay" >>"$AUTOLOG" 2>&1 || true
        rm -f /tmp/mmi-mirror-basevideo.ready /tmp/mmi-mirror-active >/dev/null 2>&1 || true
        if [ -x /mnt/app/root/carplay-altscreen/bin/mirror/stream_supervisor.sh ]; then
            /bin/sh /mnt/app/root/carplay-altscreen/bin/mirror/stream_supervisor.sh >>"$AUTOLOG" 2>&1
            SUPERVISOR_RC=$?
            echo "STREAM_SUPERVISOR_RC=$SUPERVISOR_RC" >>"$AUTOLOG" 2>&1 || true
        else
            echo "STREAM_SUPERVISOR_RC=127 reason=launcher_missing" >>"$AUTOLOG" 2>&1 || true
        fi
    ) &
fi
# END ALT111 BASEVIDEO3 AUTOSTART
BASEVIDEO3_BOOT
awk 'FNR==NR {b=b $0 "\n"; next} FNR==1 {if ($0 ~ /^#!/) {print; printf "%s",b; next} printf "%s",b} {print}' "$BLOCK" "$CLEAN" > "$NEW" ||
    fail "cannot compose BaseVideo3 autostart"

stage VALIDATE_AUTOSTART
sh -n "$NEW" || fail "BaseVideo3 autostart makes startup.sh invalid"
verify_autostart_contract "$NEW" || fail "BaseVideo3 autostart contract verification failed"

stage PUBLISH_AUTOSTART
if cmp -s "$NEW" "$STARTUP" 2>/dev/null; then
    echo "AUTOSTART_PUBLISH=UNCHANGED"
else
    mount_system_rw || fail "cannot mount startup filesystem writable"
    SYSTEM_RW=1
    cleanup_legacy_system_staging
    STARTUP_CHANGED=1
    publish_system_file "$NEW" "$STARTUP" 755 || fail "SYSTEM_WRITE_FAILED publishing BaseVideo3 autostart"
    cmp -s "$NEW" "$STARTUP" || fail "published startup.sh byte verification failed"
    echo "AUTOSTART_PUBLISH=UPDATED"
    sync >/dev/null 2>&1 || true
    stage SYSTEM_RO
    mount_system_ro || fail "cannot remount startup filesystem read-only"
    SYSTEM_RW=0
fi
system_space_snapshot start_after_publish

rm -f "$DEVICE_ROOT/tmp/mmi-rgi.stopped" "$DEVICE_ROOT/tmp/altscreen_mirror.stop.requested" 2>/dev/null || fail "cannot reset display lifecycle"
if [ -f "$STATE/rgi.disabled" ]; then
    : > "$DEVICE_ROOT/tmp/mmi-rgi.disabled" || fail "cannot disable RGI lifecycle"
    echo "INSTALL_RGI_MODE=NO"
else
    rm -f "$DEVICE_ROOT/tmp/mmi-rgi.disabled" || fail "cannot enable RGI lifecycle"
    echo "INSTALL_RGI_MODE=WITH"
fi
stage CURRENT_BOOT_DISPLAY_RESET
rm -f "$READY" "$ACTIVE" 2>/dev/null || true
READY_CLEARED=1
echo "DISPLAY_DEMAND_POLICY=STREAM_DRIVEN active_marker_owner=stream_supervisor fixed_delay=NONE"

stage CONTROLLER_START
CONTROLLER_ATTEMPTED=1
ALTSCREEN_INTEGRATED_START=1 /bin/sh "$CONTROLLER" start
RC=$?
[ "$RC" -eq 0 ] || fail_rc "$RC" "integrated AltScreen controller START failed"

stage STREAM_SUPERVISOR_START
if [ "${ALTS_START_DEFER_SUPERVISOR:-0}" = 1 ]; then
    # One-step INSTALL: the head unit is rebooted right after this START and
    # the boot autostart block launches the supervisor. Launching it now would
    # only run against the pre-reboot CarPlay process without the new preload.
    SUP_PID=deferred
    echo "STREAM_SUPERVISOR=DEFERRED_TO_REBOOT launcher=boot_autostart"
else
    /bin/sh "$MIRROR_SUPERVISOR" >/dev/null 2>&1 &
    sleep 1
    SUP_PID=$(cat "$SUPERVISOR_PID" 2>/dev/null || true)
    case "$SUP_PID" in ''|*[!0-9]*) fail "stream supervisor pid unavailable" ;; esac
    kill -0 "$SUP_PID" 2>/dev/null || fail "stream supervisor exited during startup"
    echo "STREAM_SUPERVISOR=RUNNING pid=$SUP_PID marker=$STREAM_READY"
fi

stage COMPLETE

cleanup
trap - 0 1 2 15
echo "DISPLAY_PATH=PRIVATE111_DIRECT source=ScreenStreamProcessData h264_shm=/carplay111_h264 decoder_backend=stock_omx_screen_linearized_shm decoded_shm=/carplay111_decoded sink=displayable3_gles window58_readback=0"
echo "HMI_CONTROL_PLANE=JAVA80 context=80 composite=98,101,102,3"
echo "CONTEXT_POLICY=JAVA_ONLY native_dmdt=0 sidecar_dmdt=0"
echo "PRIVATE111_NEGOTIATION_POLICY=V35_EARLY_PROTOCOL_READY sd_runtime_gate=DISABLED geometry_gate=ASYNC"
echo "DISPLAY_START_POLICY=STREAM_DRIVEN marker=/tmp/altscreen-private111.stream-ready stable_decoded_frames=2 fixed_delay=NONE"
echo "DYNAMIC_JAVA80_DEMAND=/tmp/mmi-mirror-active owner=stream_supervisor"
echo "READY_MARKER=/tmp/mmi-mirror-basevideo.ready meaning=destination_first_successful_gles_present"
if [ -f "$STARTED" ]; then echo "JAVA_CONTROLLER=OBSERVED current_boot=YES"; else echo "JAVA_CONTROLLER=NOT_YET_OBSERVED current_boot=NO_or_reboot_pending"; fi
[ "$SUP_PID" = deferred ] || echo "STREAM_SUPERVISOR=RUNNING pidfile=$SUPERVISOR_PID log=$SUPERVISOR_LOG"
echo "DIRECT_DISPLAY_SIDECAR=STARTS_ONLY_AFTER_PRIVATE111_STREAM_READY pidfile=$MIRROR_PID log=$MIRROR_LOG"
echo "START=PASS integrated=AltScreen+H264Tap+DecoderTap+Displayable3+Java80 reboot_required=YES"
exit 0
