#!/bin/sh
# Shared GEM console + operation-log helper for MMI-Cockpit-Carplay.
#
# The GEM screen gets short English progress lines, error lines, and one clear
# RESULT block. Every line of every nested script still goes to the SD
# operation log, written as it happens instead of being replayed at the end.
#
# Sourced by GEM entry scripts; POSIX sh only (QNX 6.5 sh, no tee/cut needed).
# Nested scripts may print "@@UI <text>" to show a line on the GEM screen.

ALTS_UI_LOG=""
ALTS_UI_LOG_REL=""
ALTS_UI_MAX_ERRORS=8

alts_ui_ensure_dirs() {
    for alts_ui_d in "$@"; do
        [ -d "$alts_ui_d" ] && continue
        mkdir -p "$alts_ui_d" || return 1
    done
    return 0
}

# alts_ui_open_log VOLUME ACTION FALLBACK_TMP_DIR
# Opens a new operation log on the SD card, or under FALLBACK_TMP_DIR when the
# card cannot take it. Sets ALTS_UI_LOG / ALTS_UI_LOG_REL.
alts_ui_open_log() {
    alts_ui_vol=$1; alts_ui_action=$2; alts_ui_tmp=${3:-/tmp}
    alts_ui_stamp=$(date +%Y%m%d_%H%M%S 2>/dev/null || echo unknown)
    alts_ui_dir="$alts_ui_vol/MMI-Cockpit-Carplay/logs/operations"
    if alts_ui_ensure_dirs "$alts_ui_dir" 2>/dev/null; then
        alts_ui_base="$alts_ui_dir/${alts_ui_action}_${alts_ui_stamp}"
        ALTS_UI_LOG="$alts_ui_base.log"
        alts_ui_n=0
        while [ -e "$ALTS_UI_LOG" ]; do
            alts_ui_n=$((alts_ui_n + 1))
            ALTS_UI_LOG="${alts_ui_base}_${alts_ui_n}.log"
        done
        if (printf 'OP_BEGIN action=%s storage=SD\n' "$alts_ui_action" > "$ALTS_UI_LOG") 2>/dev/null; then
            ALTS_UI_LOG_REL=${ALTS_UI_LOG#"$alts_ui_vol"/}
            return 0
        fi
    fi
    ALTS_UI_LOG="$alts_ui_tmp/altscreen_${alts_ui_action}_${alts_ui_stamp}.log"
    if (printf 'OP_BEGIN action=%s storage=TMP\n' "$alts_ui_action" > "$ALTS_UI_LOG") 2>/dev/null; then
        ALTS_UI_LOG_REL=$ALTS_UI_LOG
        return 0
    fi
    ALTS_UI_LOG=""
    ALTS_UI_LOG_REL="(not available)"
    return 1
}

alts_ui_close_log() {
    alts_ui_log "OP_END rc=$1"
    sync >/dev/null 2>&1 || true
}

# Write one line to the operation log only.
alts_ui_log() {
    [ -n "$ALTS_UI_LOG" ] || return 0
    printf '%s\n' "$*" >> "$ALTS_UI_LOG" 2>/dev/null || true
}

# Show one line on the GEM screen and record it in the log.
alts_ui() {
    printf '%s\n' "$*"
    alts_ui_log "@@UI $*"
}

# Decide whether a line from a nested script belongs on the GEM screen.
# Prints the screen form of the line, or nothing.
alts_ui_screen_line() {
    case "$1" in
        INSTALL_TRANSACTION=PREPARED*) echo "      Pre-install snapshot saved" ;;
        INSTALL_VERIFY=PASS*)          echo "      Installed files verified" ;;
        INSTALL_ROLLBACK=STARTED*)     echo "      Error - rolling back all changes..." ;;
        INSTALL_ROLLBACK=PASS*)        echo "      Rollback complete" ;;
        RESTORE_PREFLIGHT=PASS*)       echo "      Pre-checks passed" ;;
        RESTORE_VERIFY=PASS*)          echo "      Stock files verified" ;;
        ROLLBACK=PASS*)                echo "      Rollback complete" ;;
        AUTOSTART_PUBLISH=*)           echo "      Boot autostart written" ;;
        log\ collection\ complete*)    echo "      Logs saved to SD card" ;;
        *FAIL*|*ERROR*|*REFUSED*|*recovery_required=YES*)
            printf '    ! %.110s\n' "$1" ;;
    esac
}

# alts_ui_run CMD [ARGS...]
# Runs CMD with stdin from /dev/null, stdout+stderr merged. Each output line is
# written to the operation log immediately and, when relevant, to the screen.
# Returns the exit status of CMD.
alts_ui_run() {
    alts_ui_rcf="/tmp/altscreen_ui_rc.$$"
    rm -f "$alts_ui_rcf" 2>/dev/null || true
    { "$@" < /dev/null 2>&1; echo "$?" > "$alts_ui_rcf"; } |
    {
        alts_ui_errs=0
        while IFS= read -r alts_ui_line || [ -n "$alts_ui_line" ]; do
            alts_ui_log "$alts_ui_line"
            case "$alts_ui_line" in
                "@@UI"*)
                    alts_ui_t=${alts_ui_line#@@UI}
                    printf '%s\n' "${alts_ui_t# }"
                    continue
                    ;;
            esac
            alts_ui_out=$(alts_ui_screen_line "$alts_ui_line")
            [ -n "$alts_ui_out" ] || continue
            case "$alts_ui_out" in
                "    ! "*)
                    alts_ui_errs=$((alts_ui_errs + 1))
                    [ "$alts_ui_errs" -le "$ALTS_UI_MAX_ERRORS" ] || continue
                    ;;
            esac
            printf '%s\n' "$alts_ui_out"
        done
    }
    alts_ui_rc=$(cat "$alts_ui_rcf" 2>/dev/null || true)
    rm -f "$alts_ui_rcf" 2>/dev/null || true
    case "$alts_ui_rc" in ''|*[!0-9]*) alts_ui_rc=1 ;; esac
    return "$alts_ui_rc"
}

# True when the operation log contains a line starting with PATTERN text.
alts_ui_log_has() {
    [ -n "$ALTS_UI_LOG" ] || return 1
    grep -q "$1" "$ALTS_UI_LOG" 2>/dev/null
}

alts_ui_rule() {
    alts_ui "=============================================="
}

alts_ui_header() {
    alts_ui ""
    alts_ui_rule
    for alts_ui_h in "$@"; do alts_ui " $alts_ui_h"; done
    alts_ui_rule
}

# alts_ui_result STATUS LINE...
alts_ui_result() {
    alts_ui_status=$1; shift
    alts_ui ""
    alts_ui_rule
    alts_ui " RESULT: $alts_ui_status"
    for alts_ui_r in "$@"; do alts_ui " $alts_ui_r"; done
    alts_ui " Log: $ALTS_UI_LOG_REL"
    alts_ui_rule
}

# alts_detect_previous ROOT VOLUME
# Sets ALTS_PREVIOUS to a space-separated list of traces of an existing (or
# half-finished) installation; empty when the head unit looks stock.
alts_detect_previous() {
    alts_dp_root=$1; alts_dp_sd="$2/MMI-Cockpit-Carplay"
    ALTS_PREVIOUS=""
    [ -f "$alts_dp_sd/state/INSTALLED" ] && ALTS_PREVIOUS="$ALTS_PREVIOUS installed"
    [ -f "$2/Log/MMI-Cockpit-Carplay/current/INSTALLED" ] && ALTS_PREVIOUS="$ALTS_PREVIOUS legacy-installed"
    [ -d "$alts_dp_sd/install-transaction/active" ] && ALTS_PREVIOUS="$ALTS_PREVIOUS unfinished-install"
    [ -d "$alts_dp_sd/restore-transaction/active" ] && ALTS_PREVIOUS="$ALTS_PREVIOUS unfinished-restore"
    [ -e "$alts_dp_root/mnt/app/root/carplay-altscreen" ] && ALTS_PREVIOUS="$ALTS_PREVIOUS runtime"
    grep -q 'libcarplay_altscreen.so' "$alts_dp_root/mnt/system/etc/eso/production/smartphone_integrator.json" 2>/dev/null &&
        ALTS_PREVIOUS="$ALTS_PREVIOUS preload"
    for alts_dp_s in "$alts_dp_root/mnt/system/etc/boot/startup.sh" "$alts_dp_root/etc/boot/startup.sh"; do
        [ -f "$alts_dp_s" ] || continue
        grep -Eq 'BEGIN ALT111 (MIRROR|BASEVIDEO3) AUTOSTART|BEGIN ALTSCREEN DIAGNOSTICS' "$alts_dp_s" 2>/dev/null &&
            ALTS_PREVIOUS="$ALTS_PREVIOUS autostart"
        break
    done
    alts_ui_log "PREVIOUS_INSTALLATION=${ALTS_PREVIOUS:- none}"
}

# alts_ui_restore_result RC [PREVIOUS]  - RESULT block for RESTORE-style actions.
# PREVIOUS is the ALTS_PREVIOUS value detected before the restore ran.
alts_ui_restore_result() {
    if [ "$1" -eq 0 ]; then
        alts_ui_result "SUCCESS" \
            "The head unit is back to the stock configuration." \
            "Next: fully reboot the head unit." \
            "Keep this SD card: it holds the stock backup."
    elif [ "$#" -ge 2 ] && [ -z "$2" ] && alts_ui_log_has 'RESTORE=REFUSED production_changed=NO'; then
        alts_ui_result "NOTHING TO RESTORE" \
            "No installation was found; the head unit already" \
            "looks stock. Nothing was changed."
    elif alts_ui_log_has 'recovery_required=YES'; then
        alts_ui_result "FAILED" \
            "Restore stopped and its rollback was incomplete." \
            "Do NOT reboot. Run RESTORE ORIGINAL again; if it" \
            "still fails, keep this SD card and the log."
    elif alts_ui_log_has 'no trusted restore route' || alts_ui_log_has 'backup unavailable' ||
         alts_ui_log_has 'backup damaged'; then
        alts_ui_result "FAILED" \
            "No usable stock backup was found on this SD card." \
            "Nothing was changed. Insert the SD card used for" \
            "the original install and try again."
    else
        alts_ui_result "FAILED" \
            "Restore did not complete; the head unit was left" \
            "as it was before RESTORE. Check the log, then" \
            "run RESTORE ORIGINAL again."
    fi
}

# alts_ui_lock TMPDIR  - one GEM install/restore action at a time.
# Returns 1 when another live action holds the lock.
ALTS_UI_LOCK=""
alts_ui_lock() {
    alts_ui_lk="$1/altscreen_gem_action.lock"
    if ! mkdir "$alts_ui_lk" 2>/dev/null; then
        alts_ui_owner=$(cat "$alts_ui_lk/pid" 2>/dev/null || true)
        case "$alts_ui_owner" in
            ''|*[!0-9]*) ;;
            *) kill -0 "$alts_ui_owner" 2>/dev/null && return 1 ;;
        esac
        rm -rf "$alts_ui_lk" 2>/dev/null || true
        mkdir "$alts_ui_lk" 2>/dev/null || return 1
    fi
    echo "$$" > "$alts_ui_lk/pid" 2>/dev/null || true
    ALTS_UI_LOCK=$alts_ui_lk
    return 0
}
alts_ui_unlock() {
    [ -n "$ALTS_UI_LOCK" ] || return 0
    rm -rf "$ALTS_UI_LOCK" 2>/dev/null || true
    ALTS_UI_LOCK=""
}
