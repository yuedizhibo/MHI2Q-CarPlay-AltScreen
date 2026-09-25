#!/bin/sh
# Continuous append-only recorder, independent of potentially slow pidin probes.
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
PATH=${PATH:-/bin:/usr/bin}:/proc/boot:/armle/bin:/armle/scripts:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin:/eso/bin:/eso/bin/apps
LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}:/proc/boot:/usr/lib:/armle/lib:/armle/lib/dll:/lib:/mnt/app/root/carplay-altscreen/lib:/eso/lib:/mnt/app/usr/lib:/mnt/app/armle/lib:/mnt/app/armle/lib/dll:/mnt/app/armle/usr/lib:/lib/dll
export PATH LD_LIBRARY_PATH
ROOT=""; INTERVAL=1
if [ "${ALTSCREEN_CHAIN_TESTING:-0}" = 1 ]; then
    ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    case "$ROOT" in /tmp/*|/var/tmp/*) ;; *) exit 2 ;; esac
    INTERVAL=${ALTS_DIAG_INTERVAL:-0.1}
fi
SPOOL=${1:-}; OWNER=${2:-}
case "$SPOOL" in
    "$ROOT/tmp/"*) exit 2 ;;
    */MMI-Cockpit-Carplay/logs/boots/boot_*) ;;
    *) exit 2 ;;
esac
case "$OWNER" in ''|*[!0-9]*) exit 2 ;; esac
ENABLED="$ROOT/mnt/app/root/carplay-altscreen/state/diagnostics.enabled"
LIVE="$SPOOL/streams"
ensure_dirs "$LIVE" || exit 1
exec >> "$SPOOL/live_console.log" 2>&1
SLOG_PID=""; VOLUME=""
event() { printf '%s %s\n' "$(date +%Y%m%d_%H%M%S)" "$*" >> "$LIVE/recorder.log"; }
plain_append() {
    cat "$1" >> "$2"
}
start_slog() {
    if command -v sloginfo >/dev/null 2>&1; then
        (exec sloginfo -w -t) >> "$LIVE/system_source.raw" 2>&1 &
        SLOG_PID=$!
        event "SYSTEM_FOLLOW_START pid=$SLOG_PID command=sloginfo_-w_-t"
    else event "MISSING_COMMAND sloginfo continuous_system_log_unavailable=1"; fi
}
capture() (
    source=$1; name=$2
    [ -f "$source" ] || exit 0
    offset=0; seq=0; old_inode=""
    if [ -f "$LIVE/$name.cursor" ]; then read -r offset seq old_inode < "$LIVE/$name.cursor"; fi
    size=$(wc -c < "$source")
    inode=$(ls -i "$source" | awk '{print $1}')
    if [ "$offset" -gt "$size" ] || { [ -n "$old_inode" ] && [ "$inode" != "$old_inode" ]; }; then
        event "SOURCE_RESET stream=$name previous_offset=$offset size=$size"
        offset=0
    fi
    if [ "$size" -gt "$offset" ]; then
        tail -c "+$((offset + 1))" "$source" > "$LIVE/$name.chunk" || exit 0
        bytes=$(wc -c < "$LIVE/$name.chunk")
        cat "$LIVE/$name.chunk" >> "$LIVE/${name}_${seq}.log" || exit 0
        offset=$((offset + bytes))
        rm -f "$LIVE/$name.chunk"
    fi
    # Roll local storage by segment; SD receives each segment under its own name.
    if [ -f "$LIVE/${name}_${seq}.log" ] && [ "$(wc -c < "$LIVE/${name}_${seq}.log")" -ge 4194304 ]; then
        seq=$((seq + 1))
        if [ "$seq" -ge 3 ]; then
            old=$((seq - 3))
            if [ -f "$LIVE/${name}_${old}.log" ]; then
                event "LOCAL_RETENTION_DROP stream=$name segment=$old check_SD_for_archived_segment=1"
                rm -f "$LIVE/${name}_${old}.log"
            fi
        fi
    fi
    printf '%s %s %s\n' "$offset" "$seq" "$inode" > "$LIVE/$name.cursor"
)

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

# Keep one small, deliberately filtered plaintext diagnostic on SD. This file is
# the guaranteed field-test breadcrumb when the full-stream recorder is
# unavailable. It never copies protocol payloads: only routing/resolver/gate
# verdicts emitted by our own code are eligible.
persist_adaptive_status() (
    source=$(select_hook_source)
    [ -f "$source" ] || exit 0
    VOLUME=$(cat "$SPOOL/volume.path" 2>/dev/null || true)
    [ -n "$VOLUME" ] && [ -d "$VOLUME/Toolbox" ] || exit 0
    dest="$VOLUME/MMI-Cockpit-Carplay/logs/boots/${SPOOL##*/}"
    ensure_dirs "$dest" 2>/dev/null || exit 0
    target="$dest/adaptive_status.log"
    cursor="$LIVE/adaptive_status.cursor"
    offset=0; old_inode=""
    if [ -f "$cursor" ]; then read -r offset old_inode < "$cursor"; fi
    size=$(wc -c < "$source")
    inode=$(ls -i "$source" | awk '{print $1}')
    if [ "$offset" -gt "$size" ] || { [ -n "$old_inode" ] && [ "$inode" != "$old_inode" ]; }; then
        offset=0
        printf '%s ADAPTIVE_SOURCE_RESET previous_inode=%s inode=%s\n' "$(date +%Y%m%d_%H%M%S)" "$old_inode" "$inode" >> "$target"
    fi
    if [ ! -f "$target" ]; then
        profile=$(cat "$VOLUME/MMI-Cockpit-Carplay/state/firmware_profile.txt" 2>/dev/null || echo UNKNOWN)
        printf '%s ADAPTIVE_LOG_BEGIN profile=%s source=altscreen_hook.log full_stream_plaintext=YES\n' \
            "$(date +%Y%m%d_%H%M%S)" "$profile" > "$target" || exit 0
    fi
    if [ "$size" -gt "$offset" ]; then
        chunk="$LIVE/adaptive_status.chunk"
        tail -c "+$((offset + 1))" "$source" > "$chunk" 2>/dev/null || exit 0
        awk '
          /PHASE=AUG22_DYNAMIC_RESOLVER/ ||
          /PHASE=STOCK_INTERNAL_REDIRECT/ ||
          /PHASE=RUNTIME_PROCESS_IDENTITY/ ||
          /PHASE=RUNTIME_SYMBOL_RESOLUTION/ ||
          /PHASE=RUNTIME_PREREQUISITES/ ||
          /PHASE=RUNTIME_ABI_IDENTITY/ ||
          /PHASE=RUNTIME_FORWARDING_GATE/ ||
          /PHASE=PRIVATE111_BACKEND_INSTALL_RESULT/ ||
          /PHASE=RUNTIME_GEOMETRY_GATE/ ||
          /RUNTIME ready / {
              print
          }
        ' "$chunk" >> "$target" 2>/dev/null || true
        rm -f "$chunk"
        offset=$size
    fi
    printf '%s %s\n' "$offset" "$inode" > "$cursor"
)

flush() {
    VOLUME=$(cat "$SPOOL/volume.path" 2>/dev/null || true)
    [ -n "$VOLUME" ] && [ -d "$VOLUME/Toolbox" ] || return 0
    dest="$VOLUME/MMI-Cockpit-Carplay/logs/boots/${SPOOL##*/}/streams"
    ensure_dirs "$dest" 2>/dev/null || return 0
    for source in "$LIVE"/*.log; do
        [ -f "$source" ] || continue
        base=${source##*/}
        target="$dest/$base"
        cursor="$LIVE/.${base}.sd_cursor"
        copied=0
        [ ! -f "$cursor" ] || copied=$(cat "$cursor" 2>/dev/null || echo 0)
        case "$copied" in ''|*[!0-9]*) copied=0 ;; esac
        size=$(wc -c < "$source")
        [ "$size" -ge "$copied" ] || copied=0
        if [ "$size" -gt "$copied" ]; then
            chunk="$LIVE/.${base}.sd_chunk"
            tail -c "+$((copied + 1))" "$source" > "$chunk" 2>/dev/null || continue
            bytes=$(wc -c < "$chunk")
            if plain_append "$chunk" "$target" 2>/dev/null; then
                copied=$((copied + bytes))
                printf '%s\n' "$copied" > "$cursor"
            else
                event "SD_APPEND_FAILED file=$base adaptive_status_still_enabled=1"
            fi
            rm -f "$chunk"
        fi
    done
}
cycle() {
    capture "$(select_hook_source)" hook_tmp
    capture "$ROOT/tmp/CinemoDioManager.log" dio_tmp
    capture "$(select_boot_entry_source)" boot_entry
    capture "$SPOOL/boot.log" boot_events
    capture "$SPOOL/state_history.log" state_history
    capture "$LIVE/system_source.raw" system
    persist_adaptive_status
    flush
    # The native reader is append-only. Bound its temporary capture; any race at
    # this emergency trim is explicitly recorded, never advertised as lossless.
    if [ -f "$LIVE/system_source.raw" ] && [ "$(wc -c < "$LIVE/system_source.raw")" -ge 8388608 ]; then
        event "SYSTEM_RAW_TRIM possible_boundary_loss=1 limit_bytes=8388608"
        : > "$LIVE/system_source.raw"
    fi
}
finish() {
    trap - 0
    trap '' 1 2 15
    if [ -n "$SLOG_PID" ]; then
        kill -KILL "$SLOG_PID" 2>/dev/null || true
        wait "$SLOG_PID" 2>/dev/null || true
    fi
    event "LIVE_RECORDER_STOP"
    cycle
}
trap finish 0
trap 'exit 0' 1 2 15
event "LIVE_RECORDER_BEGIN interval=$INTERVAL owner=$OWNER"
start_slog
while [ -f "$ENABLED" ] && kill -0 "$OWNER" 2>/dev/null; do
    cycle
    if [ -n "$SLOG_PID" ] && ! kill -0 "$SLOG_PID" 2>/dev/null; then
        wait "$SLOG_PID"; result=$?
        event "SYSTEM_FOLLOW_EXIT status=$result"
        SLOG_PID=""
        start_slog
    fi
    sleep "$INTERVAL"
done
