#!/bin/sh
# MMI-Cockpit-Carplay GEM STORE LOGS + RESTORE action.
# Log collection is best effort; integrated restore (AltScreen + Mirror) always
# follows and its exit status is the visible GEM result.

ensure_dirs() {
    for dir in "$@"; do
        [ -d "$dir" ] && continue
        mkdir -p "$dir" || return 1
    done
    return 0
}

# GEM entry: stream log collection + restore to one SD operation log as it
# runs, show short progress on the screen, and finish with one RESULT block.
# ALTS_OPLOG_CAPTURED is inherited by RESTORE ORIGINAL so the nested launcher
# enters its transaction directly instead of opening a second log.
if [ "${ALTS_OPLOG_CAPTURED:-0}" != 1 ]; then
    CAPTURE_ENTRY="$0"
    RESOLVED_CAPTURE=$(command -v -- "$CAPTURE_ENTRY" 2>/dev/null)
    [ -n "$RESOLVED_CAPTURE" ] && CAPTURE_ENTRY="$RESOLVED_CAPTURE"

    journal_volume=""
    journal_root=""
    if [ "${ALTSCREEN_CHAIN_TESTING:-0}" = 1 ]; then
        journal_volume=${ALTSCREEN_CHAIN_VOLUME:-}
        journal_root=${ALTSCREEN_CHAIN_ROOT:-}
        case "$journal_volume" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_VOLUME"; exit 2 ;; esac
    else
        for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
            if [ -d "$candidate/Toolbox" ]; then journal_volume=$candidate; break; fi
        done
    fi
    if [ -z "$journal_volume" ] || [ ! -d "$journal_volume/Toolbox" ]; then
        echo "=============================================="
        echo " RESULT: FAILED"
        echo " No SD card with a Toolbox folder was found."
        echo " Nothing was changed. Insert the SD card that"
        echo " holds MMI-Cockpit-Carplay/backup and try again."
        echo "=============================================="
        exit 1
    fi
    CONSOLE="$journal_volume/Toolbox/scripts/altscreen_console.sh"
    if [ ! -f "$CONSOLE" ]; then
        # Older/partial package: still collect + restore, just without the new screen.
        echo "WARN: altscreen_console.sh missing; running without progress view"
        ALTS_OPLOG_CAPTURED=1; export ALTS_OPLOG_CAPTURED
        if [ "$#" -gt 0 ]; then exec /bin/sh "$CAPTURE_ENTRY" "$@"; else exec /bin/sh "$CAPTURE_ENTRY"; fi
    fi
    . "$CONSOLE"
    SD_RW_HELPER="$journal_volume/Toolbox/scripts/altscreen_sd_writable.sh"
    sd_ok=0
    if [ -f "$SD_RW_HELPER" ]; then
        . "$SD_RW_HELPER"
        altscreen_sd_ensure_writable "$journal_volume" STORE_RESTORE_JOURNAL >/dev/null 2>&1 && sd_ok=1
    fi
    alts_ui_open_log "$journal_volume" store_restore "$journal_root/tmp" || true
    alts_ui_header \
        "MMI-Cockpit-Carplay - STORE LOGS + RESTORE" \
        "Saves diagnostic logs, then restores stock." \
        "Keep the SD card inserted and the power on." \
        "Do not reboot until RESULT is shown."
    if [ "$sd_ok" != 1 ]; then
        alts_ui_result "FAILED" \
            "The SD card is not writable." \
            "Nothing was changed. Check the card (FAT32," \
            "not write-protected) and try again."
        alts_ui_close_log 1
        exit 1
    fi
    if ! alts_ui_lock "$journal_root/tmp"; then
        alts_ui_result "FAILED" \
            "Another INSTALL / RESTORE is still running." \
            "Nothing was changed. Wait for its RESULT, then" \
            "try again."
        alts_ui_close_log 1
        exit 1
    fi
    alts_detect_previous "$journal_root" "$journal_volume"
    if [ "$#" -gt 0 ]; then
        ALTS_OPLOG_CAPTURED=1 alts_ui_run /bin/sh "$CAPTURE_ENTRY" "$@"
    else
        ALTS_OPLOG_CAPTURED=1 alts_ui_run /bin/sh "$CAPTURE_ENTRY"
    fi
    journal_rc=$?
    [ "$journal_rc" -ne 0 ] || alts_ui "      OK"
    alts_ui_restore_result "$journal_rc" "$ALTS_PREVIOUS"
    alts_ui_close_log "$journal_rc"
    alts_ui_unlock
    exit "$journal_rc"
fi

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

echo "@@UI"
echo "@@UI [1/2] Collecting logs..."
/bin/sh "$CONTROLLER" collect
COLLECT_RC=$?
if [ "$COLLECT_RC" -eq 0 ]; then
    echo "log collection complete"
else
    echo "WARN: log collection incomplete (status $COLLECT_RC); restoring originals anyway"
    echo "@@UI       Some logs could not be collected; continuing"
fi
echo "@@UI"
echo "@@UI [2/2] Restoring stock configuration..."

exec /bin/sh "$RESTORE"
