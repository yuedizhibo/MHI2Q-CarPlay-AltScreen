#!/bin/sh
# MMI-Cockpit-Carplay GEM STORE LOGS + RESTORE action.
# Log collection is best effort; integrated restore (AltScreen + Mirror) always
# follows and its exit status is the visible GEM result.
BASE="$0"
RESOLVED=$(command -v -- "$BASE" 2>/dev/null)
[ -n "$RESOLVED" ] || RESOLVED="$BASE"
SCRIPTDIR=$(cd -P -- "$(dirname -- "$RESOLVED")" 2>/dev/null && pwd -P)
[ -n "$SCRIPTDIR" ] || { echo "FAIL: cannot resolve installed launcher directory"; exit 126; }

TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
DEVICE_ROOT=""
if [ "$TESTING" = 1 ]; then
    DEVICE_ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    case "$DEVICE_ROOT" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_ROOT"; exit 2 ;; esac
fi
APP_BIN="$DEVICE_ROOT/mnt/app/root/carplay-altscreen/bin"
APP_SELF="$APP_BIN/finish_mmi_cockpit_carplay_test.sh"
# ALTSCREEN_FAKE_RECORD belongs only to the host dispatcher contract test.
# Production always forwards from a legacy /eso GEM bootstrap to the owned
# /mnt/app runtime when that runtime exists.
if { [ "$TESTING" != 1 ] || [ -z "${ALTSCREEN_FAKE_RECORD:-}" ]; } &&
   [ "$SCRIPTDIR" != "$APP_BIN" ] && [ -f "$APP_SELF" ] &&
   [ -f "$APP_BIN/altscreen_chain_test.sh" ]; then
    echo "APP_RUNTIME_FORWARD action=STORE_RESTORE from=$SCRIPTDIR to=/mnt/app/root/carplay-altscreen/bin"
    if [ "$#" -gt 0 ]; then
        exec /bin/sh "$APP_SELF" "$@"
    else
        exec /bin/sh "$APP_SELF"
    fi
fi

CONTROLLER="$SCRIPTDIR/altscreen_chain_test.sh"
RESTORE="$SCRIPTDIR/stop_mmi_cockpit_carplay_test.sh"
[ -f "$CONTROLLER" ] || { echo "FAIL: installed chain controller is missing: $CONTROLLER"; exit 127; }
[ -f "$RESTORE" ] || { echo "FAIL: integrated restore launcher is missing: $RESTORE"; exit 127; }

/bin/sh "$CONTROLLER" collect
COLLECT_RC=$?
if [ "$COLLECT_RC" -eq 0 ]; then
    echo "log collection complete"
else
    echo "WARN: log collection incomplete (status $COLLECT_RC); restoring originals anyway"
fi

exec /bin/sh "$RESTORE"
