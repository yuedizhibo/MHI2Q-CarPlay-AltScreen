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

set -eu
TMP_ROOT="${ALT111_MIRROR_TMP_ROOT:-/tmp}"

PIDFILE="$TMP_ROOT/altscreen_mirror.pid"
WATCH_PIDFILE="$TMP_ROOT/altscreen_mirror.lifecycle.pid"
SUPERVISOR_PIDFILE="$TMP_ROOT/altscreen_stream_supervisor.pid"
STOP_GUARD="$TMP_ROOT/altscreen_mirror.stop.requested"
RECOVERY_DIR=$(alts_posix_tmp_dir "$TMP_ROOT")
RECOVERY_LOCK="$RECOVERY_DIR/altscreen_mirror.recovery.lock"

stop_pidfile() {
  pf=$1
  wait_limit=$2
  [ -f "$pf" ] || return 0
  pid="$(cat "$pf" 2>/dev/null || true)"
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
    n=0
    while kill -0 "$pid" 2>/dev/null && [ "$n" -lt "$wait_limit" ]; do
      sleep 1
      n=$((n + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then kill -KILL "$pid" 2>/dev/null || true; fi
  fi
  rm -f "$pf" 2>/dev/null || true
}

# Only explicit STOP/RESTORE publishes the persistent guard. A supervisor
# cleanup ends one session and must allow the next session to start. Preserve
# any existing explicit guard so concurrent STOP/RESTORE still wins.
if [ "${ALT111_SUPERVISOR_CHILD:-0}" != "1" ]; then
  : > "$STOP_GUARD" 2>/dev/null || true
  stop_pidfile "$SUPERVISOR_PIDFILE" 5
  stop_pidfile "$TMP_ROOT/altscreen_rgi_supervisor.pid" 3
  stop_pidfile "$TMP_ROOT/altscreen_rgi_renderer.pid" 2
fi
stop_pidfile "$WATCH_PIDFILE" 3
stop_pidfile "$PIDFILE" 30

rm -f "$TMP_ROOT/altscreen_mirror.ready" \
      "${ALT111_JAVA_BASE_READY_FILE:-$TMP_ROOT/mmi-altscreen-basevideo.ready}" \
      "${ALT111_MIRROR_ACTIVE_FILE:-$TMP_ROOT/mmi-altscreen-active}" \
      "${ALT111_DISPLAYABLE_STATE_FILE:-$TMP_ROOT/mmi-altscreen-displayable3.state}" \
      "$TMP_ROOT/altscreen_stream_supervisor.active" 2>/dev/null || true
rmdir "$RECOVERY_LOCK" 2>/dev/null || true

# Backward-compatible cleanup for builds that used /tmp/MMI-Cockpit-Carplay.
LEGACY_NS="$TMP_ROOT/MMI-Cockpit-Carplay/mirror"
stop_pidfile "$LEGACY_NS/lifecycle.pid" 3
stop_pidfile "$LEGACY_NS/pid" 30
rm -f "$LEGACY_NS/ready" "$LEGACY_NS/basevideo.ready" 2>/dev/null || true
rmdir "$LEGACY_NS/recovery.lock" 2>/dev/null || true

echo "MIRROR_DISPLAY=STOPPED lifecycle_watch=STOPPED context_writer=JAVA80 native_dmdt=DISABLED stop_guard=$([ -f "$STOP_GUARD" ] && echo RETAINED || echo ABSENT) volatile_mode=FLAT_TMP"
