#!/bin/sh
# V3 RESTORE ORIGINAL entry point.
# All persistent mutations are delegated to the SD-resident transactional
# orchestrator so recovery still works when /mnt/app runtime is partial/missing.
set -u

ensure_dirs() {
    for dir in "$@"; do
        [ -d "$dir" ] && continue
        mkdir -p "$dir" || return 1
    done
    return 0
}

# GEM entry: stream the RESTORE ORIGINAL transaction to the SD operation log as
# it runs, show short progress on the screen, and finish with one RESULT block.
# STORE LOGS + RESTORE and the one-step INSTALL set ALTS_OPLOG_CAPTURED=1 and
# own the log/screen themselves, so they enter the transaction directly below.
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
        # Older/partial package: still restore, just without the new screen.
        echo "WARN: altscreen_console.sh missing; running RESTORE without progress view"
        ALTS_OPLOG_CAPTURED=1; export ALTS_OPLOG_CAPTURED
        if [ "$#" -gt 0 ]; then exec /bin/sh "$CAPTURE_ENTRY" "$@"; else exec /bin/sh "$CAPTURE_ENTRY"; fi
    fi
    . "$CONSOLE"
    SD_RW_HELPER="$journal_volume/Toolbox/scripts/altscreen_sd_writable.sh"
    sd_ok=0
    if [ -f "$SD_RW_HELPER" ]; then
        . "$SD_RW_HELPER"
        altscreen_sd_ensure_writable "$journal_volume" RESTORE_JOURNAL >/dev/null 2>&1 && sd_ok=1
    fi
    alts_ui_open_log "$journal_volume" restore "$journal_root/tmp" || true
    alts_ui_header \
        "MMI-Cockpit-Carplay - RESTORE ORIGINAL" \
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
    alts_ui ""
    alts_ui "[1/1] Restoring stock configuration..."
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

TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
VOLUME=""
if [ "$TESTING" = 1 ]; then
    VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
    case "$VOLUME" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_VOLUME" >&2; exit 2 ;; esac
else
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -d "$candidate/Toolbox" ]; then
            VOLUME=$candidate
            break
        fi
    done
fi

[ -n "$VOLUME" ] || {
    echo "RESTORE=REFUSED reason=SD_WITH_TOOLBOX_NOT_FOUND production_changed=NO"
    exit 1
}

TXN="$VOLUME/Toolbox/scripts/altscreen_restore_transaction.sh"
[ -f "$TXN" ] || {
    echo "RESTORE=REFUSED reason=TRANSACTIONAL_RESTORE_SCRIPT_MISSING production_changed=NO"
    exit 127
}

echo "RESTORE_ENTRY=TRANSACTIONAL_V3 source=$TXN"
if [ "$#" -gt 0 ]; then
    exec /bin/sh "$TXN" "$@"
else
    exec /bin/sh "$TXN"
fi
