#!/bin/sh
# QNX /tmp can be a process-manager link to /dev/shmem. That namespace
# supports flat files, but neither mkdir nor atomic rename. Use the boot-local
# QNX4 ramdisk for directory locks and atomically replaced metadata. Never fall
# back to a second lock namespace: all contenders must use the same path.
alts_posix_tmp_dir() (
    alts_storage_input=$1
    alts_storage_physical=$(CDPATH= cd "$alts_storage_input" 2>/dev/null && pwd -P) || alts_storage_physical=""
    case "$alts_storage_physical" in
        */dev/shmem) printf '%s/ramdisk/var/run\n' "${alts_storage_physical%/dev/shmem}"; return 0 ;;
    esac
    case "$alts_storage_input" in /tmp|/dev/shmem)
        if [ "$(uname -s 2>/dev/null)" = QNX ]; then
            printf '%s\n' /ramdisk/var/run; return 0
        fi ;;
    esac
    printf '%s\n' "$alts_storage_input"
)

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
        "$ROOT/tmp/altscreen_hook.log" \
        "$ROOT/tmp/MMI-Cockpit-Carplay/altscreen_hook.log" \
        "$ROOT/tmp/MMI-Cockpit-Carplay.altscreen_hook.log"; do
        [ -f "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    printf '%s\n' "$ROOT/tmp/altscreen_hook.log"
}
select_boot_entry_source() {
    for candidate in \
        "$ROOT/tmp/altscreen_boot_entry.log" \
        "$ROOT/tmp/MMI-Cockpit-Carplay/boot_entry.log" \
        "$ROOT/tmp/MMI-Cockpit-Carplay.boot_entry.log"; do
        [ -f "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    printf '%s\n' "$ROOT/tmp/altscreen_boot_entry.log"
}
select_mirror_log_source() {
    for candidate in \
        "$ROOT/tmp/altscreen_mirror.log" \
        "$ROOT/tmp/MMI-Cockpit-Carplay/mirror/mirror.log" \
        "$ROOT/tmp/MMI-Cockpit-Carplay.mirror.log"; do
        [ -f "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    printf '%s\n' "$ROOT/tmp/altscreen_mirror.log"
}
select_mirror_autostart_source() {
    for candidate in \
        "$ROOT/tmp/altscreen_autostart.log" \
        "$ROOT/tmp/MMI-Cockpit-Carplay/mirror/autostart.log" \
        "$ROOT/tmp/MMI-Cockpit-Carplay.mirror.autostart.log"; do
        [ -f "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    printf '%s\n' "$ROOT/tmp/altscreen_autostart.log"
}
select_controller_log_source() {
    for candidate in \
        "$ROOT/tmp/mmi-mirror-controller.log" \
        "$ROOT/tmp/MMI-Cockpit-Carplay/mmi-mirror-controller.log"; do
        [ -f "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    # ClusterStateController writes this exact path on the vehicle.
    printf '%s\n' "$ROOT/tmp/mmi-mirror-controller.log"
}
select_wheel_log_source() {
    printf '%s\n' "$ROOT/tmp/mmi-mirror-wheel-zoom.log"
}
select_wheel_events_source() {
    printf '%s\n' "$ROOT/tmp/mmi-mirror-wheel-zoom.events"
}
select_carplay_hook_log_source() {
    printf '%s\n' "$ROOT/tmp/carplay_hook.log"
}
select_oem_geometry_history_source() {
    printf '%s\n' "$ROOT/tmp/carplay-oem-geometry.log"
}
select_oem_displaymanager_api_source() {
    printf '%s\n' "$ROOT/tmp/carplay-oem-displaymanager-read-api.log"
}
flat_plain_append() {
    cat "$1" >> "$2"
}
flat_read_cursor() {
    flat_cursor_path=$1
    [ -f "$flat_cursor_path" ] || { echo 0; return 0; }
    flat_cursor_value=$(cat "$flat_cursor_path" 2>/dev/null || true)
    case "$flat_cursor_value" in
        ''|*[!0-9]*) echo 0 ;;
        *) echo "$flat_cursor_value" ;;
    esac
}
flat_file_signature() {
    flat_sig_path=$1
    [ -f "$flat_sig_path" ] || { echo absent; return 0; }
    flat_sig_line=$(cksum "$flat_sig_path" 2>/dev/null) || { echo unreadable; return 0; }
    set -- $flat_sig_line
    [ "$#" -ge 2 ] || { echo unreadable; return 0; }
    printf '%s:%s\n' "$1" "$2"
}
flat_capture_delta() {
    flat_source=$1; flat_offset=$2; flat_target=$3; flat_temp=$4
    flat_cursor_path=${5:-}
    flat_initial_offset=$flat_offset
    [ -f "$flat_source" ] || { echo "$flat_offset"; return 0; }
    flat_size=$(wc -c < "$flat_source")
    [ "$flat_size" -ge "$flat_offset" ] || flat_offset=0
    if [ "$flat_size" -gt "$flat_offset" ]; then
        tail -c "+$((flat_offset + 1))" "$flat_source" > "$flat_temp" 2>/dev/null || { echo "$flat_offset"; return 0; }
        if flat_plain_append "$flat_temp" "$flat_target" 2>/dev/null; then flat_offset=$flat_size; fi
        rm -f "$flat_temp"
    fi
    if [ -n "$flat_cursor_path" ] && [ "$flat_offset" != "$flat_initial_offset" ]; then
        flat_cursor_tmp="${flat_cursor_path}.new.$$"
        if printf '%s\n' "$flat_offset" > "$flat_cursor_tmp" 2>/dev/null; then
            mv "$flat_cursor_tmp" "$flat_cursor_path" 2>/dev/null || rm -f "$flat_cursor_tmp"
        fi
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

    # Cursor state is volatile and published on a filesystem with rename. It survives only an
    # observer-process restart in the current boot, so a real reboot starts at
    # offset zero even when the vehicle clock is still 1970 and PIDs repeat.
    # This also avoids periodic cursor metadata writes to the SD card.
    cursor_dir=$(alts_posix_tmp_dir "$ROOT/tmp")
    CURSOR_PREFIX="$cursor_dir/altscreen_diag_cursor"
    if ! ensure_dirs "$cursor_dir"; then
        echo "WARN: diagnostic cursor storage unavailable: $cursor_dir; collection continues"
    fi

    # START/controller wrappers may have emitted a flat /tmp journal before the
    # SD card became writable. Promote those breadcrumbs now, without requiring
    # any volatile directory tree.
    operation_dest="$VOLUME/MMI-Cockpit-Carplay/logs/operations"
    if ensure_dirs "$operation_dest" 2>/dev/null; then
        for operation in "$ROOT/tmp"/altscreen_start_*.log "$ROOT/tmp"/altscreen_operation_*.log; do
            [ -f "$operation" ] || continue
            operation_name=${operation##*/}
            if cp "$operation" "$operation_dest/$operation_name.new" 2>/dev/null &&
               mv "$operation_dest/$operation_name.new" "$operation_dest/$operation_name" 2>/dev/null; then
                rm -f "$operation" 2>/dev/null || true
            else
                rm -f "$operation_dest/$operation_name.new" 2>/dev/null || true
            fi
        done
    fi
    flat_log_event "BOOT_BEGIN storage=FLAT_TMP_PLAINTEXT_SD volume=$VOLUME cursor_scope=VOLATILE_BOOT_POSIX cursor_resume=1 cursor_dir=$cursor_dir"
    flat_log_event "SD_READY volume=$VOLUME"
    flat_system="${FLAT_PREFIX}_system.raw"; flat_slog_pid=""
    if command -v sloginfo >/dev/null 2>&1; then
        (exec sloginfo -w -t) > "$flat_system" 2>&1 & flat_slog_pid=$!
    fi
    flat_hook_offset=$(flat_read_cursor "${CURSOR_PREFIX}_hook.offset")
    flat_dio_offset=$(flat_read_cursor "${CURSOR_PREFIX}_dio.offset")
    flat_entry_offset=$(flat_read_cursor "${CURSOR_PREFIX}_boot-entry.offset")
    flat_mirror_offset=$(flat_read_cursor "${CURSOR_PREFIX}_mirror.offset")
    flat_mirror_autostart_offset=$(flat_read_cursor "${CURSOR_PREFIX}_mirror-autostart.offset")
    flat_controller_offset=$(flat_read_cursor "${CURSOR_PREFIX}_controller.offset")
    flat_wheel_log_offset=$(flat_read_cursor "${CURSOR_PREFIX}_wheel-log.offset")
    flat_carplay_hook_offset=$(flat_read_cursor "${CURSOR_PREFIX}_carplay-hook.offset")
    flat_rgi_offset=$(flat_read_cursor "${CURSOR_PREFIX}_rgi.offset")
    flat_rgi_native_offset=$(flat_read_cursor "${CURSOR_PREFIX}_rgi-native.offset")
    flat_supervisor_offset=$(flat_read_cursor "${CURSOR_PREFIX}_supervisor.offset")
    flat_oem_geometry_offset=$(flat_read_cursor "${CURSOR_PREFIX}_oem-geometry.offset")
    flat_oem_api_offset=$(flat_read_cursor "${CURSOR_PREFIX}_oem-api.offset")
    flat_wheel_events_sig=absent
    flat_system_offset=0
    flat_tick=0
    while [ -f "$ENABLED" ]; do
        flat_hook_offset=$(flat_capture_delta "$(select_hook_source)" "$flat_hook_offset" "$FLAT_DEST/streams/hook_tmp.log" "${FLAT_PREFIX}_hook.chunk" "${CURSOR_PREFIX}_hook.offset")
        flat_dio_offset=$(flat_capture_delta "$ROOT/tmp/CinemoDioManager.log" "$flat_dio_offset" "$FLAT_DEST/streams/dio_tmp.log" "${FLAT_PREFIX}_dio.chunk" "${CURSOR_PREFIX}_dio.offset")
        flat_entry_offset=$(flat_capture_delta "$(select_boot_entry_source)" "$flat_entry_offset" "$FLAT_DEST/streams/boot_entry.log" "${FLAT_PREFIX}_entry.chunk" "${CURSOR_PREFIX}_boot-entry.offset")
        flat_mirror_offset=$(flat_capture_delta "$(select_mirror_log_source)" "$flat_mirror_offset" "$FLAT_DEST/streams/mirror.log" "${FLAT_PREFIX}_mirror.chunk" "${CURSOR_PREFIX}_mirror.offset")
        flat_mirror_autostart_offset=$(flat_capture_delta "$(select_mirror_autostart_source)" "$flat_mirror_autostart_offset" "$FLAT_DEST/streams/mirror_autostart.log" "${FLAT_PREFIX}_mirror_autostart.chunk" "${CURSOR_PREFIX}_mirror-autostart.offset")
        flat_controller_offset=$(flat_capture_delta "$(select_controller_log_source)" "$flat_controller_offset" "$FLAT_DEST/streams/mmi-mirror-controller.log" "${FLAT_PREFIX}_controller.chunk" "${CURSOR_PREFIX}_controller.offset")
        flat_wheel_log_offset=$(flat_capture_delta "$(select_wheel_log_source)" "$flat_wheel_log_offset" "$FLAT_DEST/streams/mmi-mirror-wheel-zoom.log" "${FLAT_PREFIX}_wheel_log.chunk" "${CURSOR_PREFIX}_wheel-log.offset")
        flat_carplay_hook_offset=$(flat_capture_delta "$(select_carplay_hook_log_source)" "$flat_carplay_hook_offset" "$FLAT_DEST/streams/carplay_hook.log" "${FLAT_PREFIX}_carplay_hook.chunk" "${CURSOR_PREFIX}_carplay-hook.offset")
        flat_rgi_offset=$(flat_capture_delta "$ROOT/tmp/maneuver_render.log" "$flat_rgi_offset" "$FLAT_DEST/streams/maneuver_render.log" "${FLAT_PREFIX}_rgi.chunk" "${CURSOR_PREFIX}_rgi.offset")
        flat_rgi_native_offset=$(flat_capture_delta "$ROOT/tmp/carplay_rgi_native.log" "$flat_rgi_native_offset" "$FLAT_DEST/streams/carplay_rgi_native.log" "${FLAT_PREFIX}_rgi_native.chunk" "${CURSOR_PREFIX}_rgi-native.offset")
        flat_supervisor_offset=$(flat_capture_delta "$ROOT/tmp/altscreen_stream_supervisor.log" "$flat_supervisor_offset" "$FLAT_DEST/streams/altscreen_stream_supervisor.log" "${FLAT_PREFIX}_supervisor.chunk" "${CURSOR_PREFIX}_supervisor.offset")
        flat_oem_geometry_offset=$(flat_capture_delta "$(select_oem_geometry_history_source)" "$flat_oem_geometry_offset" "$FLAT_DEST/streams/carplay-oem-geometry.log" "${FLAT_PREFIX}_oem_geometry.chunk" "${CURSOR_PREFIX}_oem-geometry.offset")
        flat_oem_api_offset=$(flat_capture_delta "$(select_oem_displaymanager_api_source)" "$flat_oem_api_offset" "$FLAT_DEST/streams/carplay-oem-displaymanager-read-api.log" "${FLAT_PREFIX}_oem_api.chunk" "${CURSOR_PREFIX}_oem-api.offset")
        wheel_events_source=$(select_wheel_events_source)
        wheel_events_sig=$(flat_file_signature "$wheel_events_source")
        if [ "$wheel_events_sig" != "$flat_wheel_events_sig" ]; then
            if [ -f "$wheel_events_source" ]; then
                if cp "$wheel_events_source" "$FLAT_DEST/streams/mmi-mirror-wheel-zoom.events.new" 2>/dev/null &&
                   mv "$FLAT_DEST/streams/mmi-mirror-wheel-zoom.events.new" "$FLAT_DEST/streams/mmi-mirror-wheel-zoom.events" 2>/dev/null; then
                    flat_wheel_events_sig=$wheel_events_sig
                else
                    rm -f "$FLAT_DEST/streams/mmi-mirror-wheel-zoom.events.new" 2>/dev/null || true
                fi
            else
                rm -f "$FLAT_DEST/streams/mmi-mirror-wheel-zoom.events" "$FLAT_DEST/streams/mmi-mirror-wheel-zoom.events.new" 2>/dev/null || true
                flat_wheel_events_sig=absent
            fi
        fi
        if [ -f "$ROOT/tmp/carplay-oem-geometry.state" ]; then
            cp "$ROOT/tmp/carplay-oem-geometry.state" "$FLAT_DEST/streams/carplay-oem-geometry.state.new" 2>/dev/null &&
                mv "$FLAT_DEST/streams/carplay-oem-geometry.state.new" "$FLAT_DEST/streams/carplay-oem-geometry.state" 2>/dev/null || true
        fi
        if [ -f "$ROOT/tmp/mmi-mirror-displayable3.state" ]; then
            cp "$ROOT/tmp/mmi-mirror-displayable3.state" "$FLAT_DEST/streams/mmi-mirror-displayable3.state.new" 2>/dev/null &&
                mv "$FLAT_DEST/streams/mmi-mirror-displayable3.state.new" "$FLAT_DEST/streams/mmi-mirror-displayable3.state" 2>/dev/null || true
        fi
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
                ls -la "$ROOT/mnt/app/root/carplay-altscreen/bin/rgi"
                ls -la "$ROOT/mnt/app/root/carplay-altscreen/state"
                printf 'DISPLAY_STATE_PATH_PERMISSIONS_BEGIN\n'
                id 2>/dev/null || true
                ls -ld "$ROOT/tmp" 2>&1 || true
                for display_state_probe in "$ROOT/tmp/mmi-mirror-displayable3.state" "$ROOT/tmp/mmi-mirror-displayable3.state.tmp" "$ROOT/tmp"/mmi-mirror-displayable3.state.tmp.*; do
                    if [ -e "$display_state_probe" ]; then
                        ls -l "$display_state_probe" 2>&1 || true
                    else
                        printf 'DISPLAY_STATE_PATH path=%s status=ABSENT\n' "$display_state_probe"
                    fi
                done
                printf 'DISPLAY_STATE_PATH_PERMISSIONS_END\n'
                for rgi_marker in altscreen_rgi_supervisor.pid altscreen_rgi_renderer.pid mmi-rgi.disabled altscreen_mirror.stop.requested; do
                    if [ -f "$ROOT/tmp/$rgi_marker" ]; then
                        printf 'RGI_STATE %s=' "$rgi_marker"
                        cat "$ROOT/tmp/$rgi_marker"
                        printf '\n'
                    else
                        printf 'RGI_STATE %s=ABSENT\n' "$rgi_marker"
                    fi
                done
                for rgi_verbose in "$ROOT/tmp/carplay_verbose" "$ROOT/mnt/app/carplay_verbose" "$ROOT/mnt/app/root/carplay-altscreen/state/rgi-debug.enabled"; do
                    if [ -f "$rgi_verbose" ]; then printf 'RGI_LOG_MARKER %s=PRESENT\n' "$rgi_verbose"; else printf 'RGI_LOG_MARKER %s=ABSENT\n' "$rgi_verbose"; fi
                done
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

# Flat /tmp contract: diagnostics persist directly to SD and use only
# process-local flat /tmp scratch (altscreen_diag_$$*). No /tmp directory tree
# is created, so early boot logging cannot be blocked by QNX mkdir behavior.
storage_wait=0
while [ -f "$ENABLED" ]; do
    storage_volume=""
    if [ "${ALTSCREEN_CHAIN_TESTING:-0}" = 1 ]; then
        storage_candidate=${ALTSCREEN_CHAIN_VOLUME:-}
        [ ! -d "$storage_candidate/Toolbox" ] || storage_volume=$storage_candidate
    else
        for storage_candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
            if [ -d "$storage_candidate/Toolbox" ] && { [ -d "$storage_candidate/MMI-Cockpit-Carplay/backup/original" ] || [ -d "$storage_candidate/Backup/AltScreenChain/original" ]; }; then
                storage_volume=$storage_candidate
                break
            fi
        done
    fi
    if [ -n "$storage_volume" ]; then
        if [ "${ALTSCREEN_CHAIN_TESTING:-0}" != 1 ]; then
            (exec mount -uw "$storage_volume") >/dev/null 2>&1 &
            storage_mount_pid=$!
            (sleep 5; kill -KILL "$storage_mount_pid" 2>/dev/null || true) &
            storage_timer=$!
            wait "$storage_mount_pid" || true
            kill "$storage_timer" 2>/dev/null || true
            wait "$storage_timer" 2>/dev/null || true
        fi
        if ensure_dirs "$storage_volume/MMI-Cockpit-Carplay/logs/boots" 2>/dev/null; then
            run_flat_plaintext "$storage_volume"
            exit 0
        fi
    fi
    echo "SD_WAIT storage=FLAT_TMP_PLAINTEXT_SD attempt=$storage_wait"
    storage_wait=$((storage_wait + 1))
    if [ "$ITERATIONS" -gt 0 ] && [ "$storage_wait" -ge "$ITERATIONS" ]; then exit 0; fi
    sleep "$INTERVAL"
done
exit 0
