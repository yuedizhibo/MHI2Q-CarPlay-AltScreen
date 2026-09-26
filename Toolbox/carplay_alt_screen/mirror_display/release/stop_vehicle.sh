#!/bin/sh
set -eu
TMP_ROOT="${ALT111_MIRROR_TMP_ROOT:-/tmp}"
FLAT="$TMP_ROOT/MMI-Cockpit-Carplay.mirror"
PIDFILE="$FLAT.pid"
WATCH_PIDFILE="$FLAT.lifecycle.pid"
STOP_GUARD="$FLAT.stop.requested"
RECOVERY_LOCK="$FLAT.recovery.lock"

# Publish the guard before killing anything so a concurrent private111 teardown
# watcher cannot relaunch a fresh sidecar while an explicit RESTORE/STOP is in
# progress.
: > "$STOP_GUARD" 2>/dev/null || true

if [ -f "$WATCH_PIDFILE" ]; then
  WATCH_PID="$(cat "$WATCH_PIDFILE" 2>/dev/null || true)"
  if [ -n "$WATCH_PID" ] && kill -0 "$WATCH_PID" 2>/dev/null; then
    kill -TERM "$WATCH_PID" 2>/dev/null || true
    N=0
    while kill -0 "$WATCH_PID" 2>/dev/null && [ "$N" -lt 3 ]; do
      sleep 1
      N=$((N + 1))
    done
    if kill -0 "$WATCH_PID" 2>/dev/null; then
      kill -KILL "$WATCH_PID" 2>/dev/null || true
    fi
  fi
fi

if [ -f "$PIDFILE" ]; then
  PID="$(cat "$PIDFILE" 2>/dev/null || true)"
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null || true
    N=0
    while kill -0 "$PID" 2>/dev/null && [ "$N" -lt 30 ]; do
      sleep 1
      N=$((N + 1))
    done
    if kill -0 "$PID" 2>/dev/null; then kill -KILL "$PID" 2>/dev/null || true; fi
  fi
fi

# Keep STOP_GUARD published after STOP. Any already-scheduled delayed recovery
# child must continue to see the explicit-stop decision. A future manual START
# (RESTART_REASON empty) is the only path that clears this guard.
rm -f "$FLAT.pid" "$FLAT.lifecycle.pid" "$FLAT.ready" "$FLAT.basevideo.ready" \
      /tmp/mmi-mirror-basevideo.ready
rm -f "$RECOVERY_LOCK" 2>/dev/null || true

echo "MIRROR_DISPLAY=STOPPED lifecycle_watch=STOPPED context_writer=JAVA80 native_dmdt=DISABLED stop_guard=RETAINED"
