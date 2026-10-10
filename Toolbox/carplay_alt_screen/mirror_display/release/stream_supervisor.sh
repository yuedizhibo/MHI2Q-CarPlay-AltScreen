#!/bin/sh
# V3.4 stream-driven display supervisor.
# private111 negotiation/production is always owned by the preloaded native hook.
# This process only starts the display consumer after the current private111
# producer has published a stable decoded stream marker.
set -eu

PATH=${PATH:+$PATH:}/proc/boot:/armle/bin:/armle/scripts:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin:/eso/bin:/eso/bin/apps
export PATH

case "$0" in
  */*) ROOT=${0%/*} ;;
  *) ROOT=. ;;
esac
ROOT=$(CDPATH= cd "$ROOT" 2>/dev/null && pwd) || exit 2


# Independent Mirror and AltScreen own the same physical cluster display.
# Check live process arguments, never a lone stale marker. Do not kill or
# uninstall another package; refuse before changing our production state.
altscreen_display_precheck() {
    if ! command -v pidin >/dev/null 2>&1; then
        echo "DISPLAY_PRECHECK=UNAVAILABLE reason=pidin_missing" >&2
        return 0
    fi
    alts_display_processes=$(pidin ar 2>/dev/null) || {
        echo "DISPLAY_PRECHECK=FAILED reason=process_inventory_unavailable" >&2
        return 1
    }
    alts_display_conflicts=$(printf '%s\n' "$alts_display_processes" | awk '
      /(^|[\/[:space:]])mmi_mirror_supervisor\.sh([[:space:]]|$)/ ||
      /(^|[\/[:space:]])stop_mmi_mirror_toolbox\.sh([[:space:]]|$)/ {
        print
      }')
    if [ -n "$alts_display_conflicts" ]; then
        echo "DISPLAY_PRECHECK=REFUSED reason=EXTERNAL_MIRROR_RUNNING production_changed=NO" >&2
        printf '%s\n' "$alts_display_conflicts" >&2
        echo "Stop the separate Mirror package before starting or installing AltScreen." >&2
        return 1
    fi
    return 0
}

altscreen_display_precheck || exit 5

TMP_ROOT="${ALT111_MIRROR_TMP_ROOT:-/tmp}"
PIDFILE="$TMP_ROOT/altscreen_stream_supervisor.pid"
STATEFILE="$TMP_ROOT/altscreen_stream_supervisor.active"
LOGFILE="$TMP_ROOT/altscreen_stream_supervisor.log"
STREAM_READY="$TMP_ROOT/altscreen-private111.stream-ready"
ACTIVE="${ALT111_MIRROR_ACTIVE_FILE:-$TMP_ROOT/mmi-altscreen-active}"
MIRROR_PID="$TMP_ROOT/altscreen_mirror.pid"
START="$ROOT/start_vehicle.sh"
STOP="$ROOT/stop_vehicle.sh"

[ -x "$START" ] || { echo "ERROR missing start_vehicle.sh" >&2; exit 2; }
[ -x "$STOP" ] || { echo "ERROR missing stop_vehicle.sh" >&2; exit 2; }

if [ -f "$PIDFILE" ]; then
  OLD=$(cat "$PIDFILE" 2>/dev/null || true)
  if [ -n "$OLD" ] && kill -0 "$OLD" 2>/dev/null; then
    # Explicit repeated START may rearm a renderer which reached its bounded
    # restart ceiling. Its own atomic lock prevents duplicate instances.
    if [ -x "$ROOT/rgi_supervisor.sh" ]; then
      /bin/sh "$ROOT/rgi_supervisor.sh" >> "$LOGFILE" 2>&1 &
    fi
    echo "STREAM_SUPERVISOR=ALREADY_RUNNING pid=$OLD"
    exit 0
  fi
  rm -f "$PIDFILE"
fi

echo "$$" > "$PIDFILE"
: > "$LOGFILE"
echo "STREAM_SUPERVISOR=STARTED pid=$$ policy=private111_stable_frames_no_fixed_delay marker=$STREAM_READY" >> "$LOGFILE"

read_stream_key() {
  [ -s "$STREAM_READY" ] || return 1
  producer_pid=$(sed -n 's/^pid=//p' "$STREAM_READY" 2>/dev/null | head -n 1)
  generation=$(sed -n 's/^generation=//p' "$STREAM_READY" 2>/dev/null | head -n 1)
  cookie=$(sed -n 's/^cookie=//p' "$STREAM_READY" 2>/dev/null | head -n 1)
  frames=$(sed -n 's/^frames=//p' "$STREAM_READY" 2>/dev/null | head -n 1)
  complete=$(sed -n 's/^ready=//p' "$STREAM_READY" 2>/dev/null | head -n 1)
  case "$producer_pid" in ''|*[!0-9]*) return 1 ;; esac
  case "$generation" in ''|*[!0-9]*) return 1 ;; esac
  case "$frames" in ''|*[!0-9]*) return 1 ;; esac
  [ "$generation" -gt 0 ] || return 1
  [ "$frames" -ge 2 ] || return 1
  [ "$complete" = "1" ] || return 1
  [ -n "$cookie" ] || return 1
  kill -0 "$producer_pid" 2>/dev/null || return 1
  printf '%s:%s:%s\n' "$producer_pid" "$generation" "$cookie"
}

mirror_running() {
  [ -s "$MIRROR_PID" ] || return 1
  mpid=$(cat "$MIRROR_PID" 2>/dev/null || true)
  case "$mpid" in ''|*[!0-9]*) return 1 ;; esac
  kill -0 "$mpid" 2>/dev/null
}

stop_display() {
  rm -f "$ACTIVE" 2>/dev/null || true
  ALT111_SUPERVISOR_CHILD=1 /bin/sh "$STOP" >> "$LOGFILE" 2>&1 || true
  rm -f "$STATEFILE" 2>/dev/null || true
}

cleanup() {
  trap - 0 1 2 15
  if [ -n "$RGI_PID" ]; then
    # A completed child may have been reaped automatically. Never signal its
    # historical PID if the renderer supervisor no longer owns that marker.
    if [ "$(cat "$TMP_ROOT/altscreen_rgi_supervisor.pid" 2>/dev/null || true)" = "$RGI_PID" ]; then
      kill -TERM "$RGI_PID" 2>/dev/null || true
    fi
    wait "$RGI_PID" 2>/dev/null || true
  fi
  stop_display
  rm -f "$PIDFILE" 2>/dev/null || true
  echo "STREAM_SUPERVISOR=STOPPED pid=$$" >> "$LOGFILE" 2>/dev/null || true
}
RGI_PID=""
if [ -x "$ROOT/rgi_supervisor.sh" ]; then
  /bin/sh "$ROOT/rgi_supervisor.sh" >> "$LOGFILE" 2>&1 &
  RGI_PID=$!
fi
trap cleanup 0
trap 'exit 0' 1 2 15

while :; do
  KEY=$(read_stream_key 2>/dev/null || true)
  OLD_KEY=$(cat "$STATEFILE" 2>/dev/null || true)

  if [ -n "$KEY" ]; then
    if [ -n "$OLD_KEY" ] && [ "$OLD_KEY" != "$KEY" ]; then
      echo "STREAM_SESSION_CHANGE old=$OLD_KEY new=$KEY action=RESTART_DISPLAY" >> "$LOGFILE"
      stop_display
      OLD_KEY=""
    fi

    if ! mirror_running; then
      RESTART_REASON=stream_ready
      if [ -n "$OLD_KEY" ] && [ "$OLD_KEY" = "$KEY" ]; then
        RESTART_REASON=sidecar_abnormal
      fi
      echo "STREAM_READY key=$KEY reason=$RESTART_REASON action=START_DISPLAY recover_current_session=1" >> "$LOGFILE"
      touch "$ACTIVE"
      start_rc=0
      if ALT111_MIRROR_RESTART_REASON="$RESTART_REASON" ALT111_STREAM_SUPERVISED=1 ALT111_RECOVER_CURRENT_SESSION=1 /bin/sh "$START" >> "$LOGFILE" 2>&1; then
        if ! mirror_running; then
          start_rc=3
        fi
      else
        start_rc=$?
      fi
      if [ "$start_rc" -eq 0 ]; then
        printf '%s\n' "$KEY" > "$STATEFILE"
      else
        rm -f "$ACTIVE" "$STATEFILE" 2>/dev/null || true
        echo "STREAM_DISPLAY_START_FAILED key=$KEY rc=$start_rc fail_open=YES" >> "$LOGFILE"
      fi
    else
      [ -f "$ACTIVE" ] || touch "$ACTIVE"
      [ "$OLD_KEY" = "$KEY" ] || printf '%s\n' "$KEY" > "$STATEFILE"
    fi
  else
    if mirror_running || [ -f "$ACTIVE" ] || [ -s "$STATEFILE" ]; then
      echo "STREAM_NOT_READY action=STOP_DISPLAY" >> "$LOGFILE"
      stop_display
    fi
  fi

  sleep 1
done
