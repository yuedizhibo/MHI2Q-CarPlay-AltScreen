#!/bin/sh
# MMI-Cockpit-Carplay GEM START alias.
# The alias intentionally uses the same integrated AltScreen+Mirror START path.
BASE="$0"
RESOLVED=$(command -v -- "$BASE" 2>/dev/null)
[ -n "$RESOLVED" ] || RESOLVED="$BASE"
SCRIPTDIR=$(cd -P -- "$(dirname -- "$RESOLVED")" 2>/dev/null && pwd -P)
[ -n "$SCRIPTDIR" ] || { echo "FAIL: cannot resolve installed launcher directory"; exit 126; }
TARGET="$SCRIPTDIR/start_mmi_cockpit_carplay_rx_test.sh"
[ -f "$TARGET" ] || { echo "FAIL: integrated START launcher is missing: $TARGET"; exit 127; }
if [ "$#" -gt 0 ]; then
    exec /bin/sh "$TARGET" "$@"
else
    exec /bin/sh "$TARGET"
fi
