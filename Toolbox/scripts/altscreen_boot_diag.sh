#!/bin/sh
# Independent boot observer: never loads, restarts, or arms CarPlay.
set -u

# QNX compatibility: some vehicle mkdir implementations return EEXIST for
# `mkdir -p` when the final directory already exists. Idempotent directory
# creation must therefore test first; lock acquisition still uses bare mkdir.
ensure_dirs() {
    for dir in "$@"; do
        [ -d "$dir" ] && continue
        mkdir -p "$dir" || return 1
    done
    return 0
}
# The boot block runs before OEM startup exports its application environment.
# A background child cannot inherit exports performed later by its parent.
PATH=${PATH:-/bin:/usr/bin}:/proc/boot:/armle/bin:/armle/scripts:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin:/eso/bin:/eso/bin/apps
LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}:/proc/boot:/usr/lib:/armle/lib:/armle/lib/dll:/lib:/mnt/app/root/carplay-altscreen/lib:/eso/lib:/mnt/app/usr/lib:/mnt/app/armle/lib:/mnt/app/armle/lib/dll:/mnt/app/armle/usr/lib:/lib/dll
export PATH LD_LIBRARY_PATH
ROOT=""; INTERVAL=2; ITERATIONS=0; PROBE_SECONDS=5
if [ "${ALTSCREEN_CHAIN_TESTING:-0}" = 1 ]; then
    ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    case "$ROOT" in /tmp/*|/var/tmp/*) ;; *) exit 2 ;; esac
    INTERVAL=${ALTS_DIAG_INTERVAL:-0.1}
    ITERATIONS=${ALTS_DIAG_ITERATIONS:-5}
    PROBE_SECONDS=${ALTS_DIAG_PROBE_SECONDS:-1}
fi
ENABLED="$ROOT/mnt/app/root/carplay-altscreen/state/diagnostics.enabled"
[ -f "$ENABLED" ] || exit 0
# Probe execution, not just command presence: mounted commands can still lack
# their shared libraries early in boot. This observer is already asynchronous.
ready_wait=0
while ! (date +%Y >/dev/null && printf '%s' ready | wc -c >/dev/null &&
         printf '%s' ready | tail -c 1 >/dev/null &&
         printf '%s' ready | cksum >/dev/null) 2>/dev/null; do
    echo "BOOT_ENV_WAIT step=$ready_wait"
    ready_wait=$((ready_wait + 1))
    [ "$ready_wait" -lt 60 ] || { echo "BOOT_ENV_UNAVAILABLE"; exit 1; }
    sleep 2 || exit 1
done
select_hook_source() {
    for candidate in \
        "$ROOT/tmp/MMI-Cockpit-Carplay.altscreen_hook.log" \
        "$ROOT/tmp/altscreen_hook.log"; do
        [ -f "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    printf '%s\n' "$ROOT/tmp/MMI-Cockpit-Carplay.altscreen_hook.log"
}
select_boot_entry_source() {
    for candidate in \
        "$ROOT/tmp/MMI-Cockpit-Carplay.boot_entry.log"; do
        [ -f "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    printf '%s\n' "$ROOT/tmp/MMI-Cockpit-Carplay.boot_entry.log"
}
select_mirror_log_source() {
    for candidate in \
        "$ROOT/tmp/MMI-Cockpit-Carplay.mirror.log"; do
        [ -f "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    printf '%s\n' "$ROOT/tmp/MMI-Cockpit-Carplay.mirror.log"
}
select_mirror_autostart_source() {
    for candidate in \
        "$ROOT/tmp/MMI-Cockpit-Carplay.mirror.autostart.log"; do
        [ -f "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    printf '%s\n' "$ROOT/tmp/MMI-Cockpit-Carplay.mirror.autostart.log"
}
flat_plain_append() {
    [ -f "$1" ] || return 1
    current=0
    [ ! -f "$2" ] || current=$(wc -c < "$2" 2>/dev/null || echo 0)
    [ "$current" -lt 8388608 ] || return 0
    tail -c 1048576 "$1" >> "$2"
}
flat_capture_delta() {
    flat_source=$1; flat_offset=$2; flat_target=$3; flat_temp=$4
    [ -f "$flat_source" ] || { echo "$flat_offset"; return 0; }
    flat_size=$(wc -c < "$flat_source")
    [ "$flat_size" -ge "$flat_offset" ] || flat_offset=0
    if [ "$flat_size" -gt "$flat_offset" ]; then
        tail -c "+$((flat_offset + 1))" "$flat_source" > "$flat_temp" 2>/dev/null || { echo "$flat_offset"; return 0; }
        if flat_plain_append "$flat_temp" "$flat_target" 2>/dev/null; then flat_offset=$flat_size; fi
        rm -f "$flat_temp"
    fi
    echo "$flat_offset"
}
flat_probe() {
    flat_name=$1; shift
    flat_raw="${FLAT_PREFIX}_${flat_name}.raw"
    flat_out="${FLAT_PREFIX}_${flat_name}.out"
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "MISSING_COMMAND $1" > "$flat_out"
    else
        (exec "$@") > "$flat_raw" 2>&1 &
        flat_child=$!
        (sleep "$PROBE_SECONDS"; kill -KILL "$flat_child" 2>/dev/null || true) &
        flat_timer=$!
        wait "$flat_child"; flat_rc=$?
        kill "$flat_timer" 2>/dev/null || true; wait "$flat_timer" 2>/dev/null || true
        tail -c 1048576 "$flat_raw" > "$flat_out"
        echo "PROBE_RESULT status=$flat_rc command=$*" >> "$flat_out"
    fi
    flat_plain_append "$flat_out" "$FLAT_DEST/$flat_name.log" 2>/dev/null || true
    rm -f "$flat_raw" "$flat_out"
}
flat_log_event() {
    flat_event="${FLAT_PREFIX}_event"
    printf '%s %s\n' "$(date +%Y%m%d_%H%M%S)" "$*" > "$flat_event"
    flat_plain_append "$flat_event" "$FLAT_DEST/boot.log" 2>/dev/null || true
    rm -f "$flat_event"
}
run_flat_plaintext() {
    VOLUME=$1
    BOOT_ID="boot_$(date +%Y%m%d_%H%M%S)_$$"
    FLAT_DEST="$VOLUME/MMI-Cockpit-Carplay/logs/boots/$BOOT_ID"
    FLAT_PREFIX="$ROOT/tmp/altscreen_diag_$$"
    ensure_dirs "$FLAT_DEST/streams" 2>/dev/null || return 0
    set -- "$VOLUME/MMI-Cockpit-Carplay/logs/boots"/boot_*
    while [ "$#" -gt 8 ]; do
        [ -d "$1" ] && [ ! -L "$1" ] && rm -rf "$1" 2>/dev/null || true
        shift
    done
    flat_log_event "BOOT_BEGIN storage=FLAT_TMP_PLAINTEXT_SD volume=$VOLUME"
    flat_log_event "SD_READY volume=$VOLUME"
    flat_system="${FLAT_PREFIX}_system.raw"; flat_slog_pid=""
    if command -v sloginfo >/dev/null 2>&1; then
        (exec sloginfo -w -t) > "$flat_system" 2>&1 & flat_slog_pid=$!
    fi
    flat_hook_offset=0; flat_dio_offset=0; flat_entry_offset=0; flat_mirror_offset=0; flat_mirror_autostart_offset=0; flat_system_offset=0; flat_tick=0
    while [ -f "$ENABLED" ]; do
        flat_hook_offset=$(flat_capture_delta "$(select_hook_source)" "$flat_hook_offset" "$FLAT_DEST/streams/hook_tmp.log" "${FLAT_PREFIX}_hook.chunk")
        flat_dio_offset=$(flat_capture_delta "$ROOT/tmp/CinemoDioManager.log" "$flat_dio_offset" "$FLAT_DEST/streams/dio_tmp.log" "${FLAT_PREFIX}_dio.chunk")
        flat_entry_offset=$(flat_capture_delta "$(select_boot_entry_source)" "$flat_entry_offset" "$FLAT_DEST/streams/boot_entry.log" "${FLAT_PREFIX}_entry.chunk")
        flat_mirror_offset=$(flat_capture_delta "$(select_mirror_log_source)" "$flat_mirror_offset" "$FLAT_DEST/streams/mirror.log" "${FLAT_PREFIX}_mirror.chunk")
        flat_mirror_autostart_offset=$(flat_capture_delta "$(select_mirror_autostart_source)" "$flat_mirror_autostart_offset" "$FLAT_DEST/streams/mirror_autostart.log" "${FLAT_PREFIX}_mirror_autostart.chunk")
        flat_system_offset=$(flat_capture_delta "$flat_system" "$flat_system_offset" "$FLAT_DEST/streams/system.log" "${FLAT_PREFIX}_system.chunk")
        if [ -f "$flat_system" ] && [ "$(wc -c < "$flat_system")" -ge 8388608 ]; then
            flat_log_event "SYSTEM_RAW_TRIM possible_boundary_loss=1 limit_bytes=8388608"
            : > "$flat_system"
            flat_system_offset=0
        fi
        if [ "$flat_tick" = 0 ] || [ $((flat_tick % 15)) = 0 ]; then
            flat_probe processes.txt pidin arguments
            flat_probe dio_libraries.txt pidin -p dio_manager libs
            flat_probe dio_mappings.txt pidin -p dio_manager mapinfo
            flat_probe integrator_libraries.txt pidin -p smartphone_integrator libs
            flat_probe mounts.txt mount
            flat_probe network.txt netstat -an
            if [ -x "$ROOT/armle/sbin/pfctl" ]; then
                flat_probe pf_rules.txt "$ROOT/armle/sbin/pfctl" -sr
            else
                flat_probe pf_rules.txt pfctl -sr
            fi
            flat_state="${FLAT_PREFIX}_state.out"
            {
                date
                ls -la "$ROOT/mnt/app/root/carplay-altscreen/lib"
                ls -la "$ROOT/mnt/app/root/carplay-altscreen/bin/mirror"
                ls -la "$ROOT/mnt/app/root/carplay-altscreen/state"
                ls -la "$VOLUME/MMI-Cockpit-Carplay/state"
            } > "$flat_state" 2>&1
            flat_plain_append "$flat_state" "$FLAT_DEST/file_state.txt.log" 2>/dev/null || true
            rm -f "$flat_state"
        fi
        flat_tick=$((flat_tick + 1))
        if [ "$ITERATIONS" -gt 0 ] && [ "$flat_tick" -ge "$ITERATIONS" ]; then break; fi
        sleep "$INTERVAL"
    done
    if [ -n "$flat_slog_pid" ]; then kill -KILL "$flat_slog_pid" 2>/dev/null || true; wait "$flat_slog_pid" 2>/dev/null || true; fi
    flat_log_event "DIAGNOSTICS_STOP"
    rm -f "${FLAT_PREFIX}"_* 2>/dev/null || true
    return 0
}

storage_wait=0
while [ -f "$ENABLED" ]; do
    storage_volume=""
    if [ "${ALTSCREEN_CHAIN_TESTING:-0}" = 1 ]; then
        storage_candidate=${ALTSCREEN_CHAIN_VOLUME:-}
        [ ! -d "$storage_candidate/Toolbox" ] || storage_volume=$storage_candidate
    else
        for storage_candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
            if [ -d "$storage_candidate/Toolbox" ] &&
               [ -d "$storage_candidate/MMI-Cockpit-Carplay/backup/original" ]; then
                storage_volume=$storage_candidate
                break
            fi
        done
    fi
    if [ -n "$storage_volume" ]; then
        if [ "${ALTSCREEN_CHAIN_TESTING:-0}" != 1 ]; then
            mount -uw "$storage_volume" >/dev/null 2>&1 || true
        fi
        if ensure_dirs "$storage_volume/MMI-Cockpit-Carplay/logs/boots" 2>/dev/null; then
            run_flat_plaintext "$storage_volume"
            exit 0
        fi
    fi
    storage_wait=$((storage_wait + 1))
    if [ "$ITERATIONS" -gt 0 ] && [ "$storage_wait" -ge "$ITERATIONS" ]; then exit 0; fi
    sleep "$INTERVAL"
done
exit 0
