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
        mkdir -p "$alts_ui_d" || [ -d "$alts_ui_d" ] || return 1
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
    {
        if [ -n "$ALTS_UI_LOCK" ]; then
            # Register the actual worker shell before it can launch a writer.
            # A killed GEM parent must not release protection while its nested
            # installation/restore transaction is still changing device files.
            /bin/sh -c '
                registration=$1; parent=$2; token=$3; console=$4; shift 4
                [ -f "$console" ] || { echo "ERROR: operation lease helper missing"; exit 125; }
                . "$console"
                worker="$registration/workers/$$"
                if ! alts_ui_ensure_dirs "$registration/workers" || ! mkdir "$worker"; then
                    echo "ERROR: cannot register installation worker"; exit 125
                fi
                identity=$(alts_ui_process_identity "$$")
                if ! printf "%s\n" "$identity" > "$worker/identity"; then
                    rmdir "$worker" 2>/dev/null || true; exit 125
                fi
                parent_identity=$(alts_ui_process_identity "$parent")
                if ! kill -0 "$parent" 2>/dev/null || [ "$parent_identity" = dead ] ||
                   [ "$(cat "$registration/token" 2>/dev/null)" != "$token" ] ||
                   [ ! -s "$registration/ticket" ]; then
                    rm -f "$worker/identity"; rmdir "$worker" 2>/dev/null || true
                    echo "ERROR: installation parent exited before worker start"; exit 125
                fi
                "$@" < /dev/null
                rc=$?
                rm -f "$worker/identity"; rmdir "$worker" 2>/dev/null || true
                exit "$rc"
            ' altscreen-ui-worker "$ALTS_UI_LOCK" "$ALTS_UI_LOCK_PID" "$ALTS_UI_LOCK_TOKEN" "${CONSOLE:-}" "$@" < /dev/null 2>&1
        else
            "$@" < /dev/null 2>&1
        fi
        echo "$?" > "$alts_ui_rcf"
    } |
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
    [ -e "$alts_dp_root/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar" ] && ALTS_PREVIOUS="$ALTS_PREVIOUS hmi"
    grep -Eq 'libcarplay_(altscreen|rgi_meta)\.so' "$alts_dp_root/mnt/system/etc/eso/production/smartphone_integrator.json" 2>/dev/null &&
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

# Per-process bakery tickets: a directory alone is never proof of occupation.
# Registration names contain the PID before any metadata is written. A crashed
# publisher is therefore distinguishable from a live publisher. Tickets are
# published by rename, and stale registrations are ignored, never recursively
# deleted by a competing installer. This avoids stale-reaper/acquirer races.
ALTS_UI_LOCK=""
ALTS_UI_LOCK_MESSAGE=""
ALTS_UI_LOCK_DETAIL=""
ALTS_UI_LOCK_TOKEN=""
ALTS_UI_LOCK_PID=""

alts_ui_lock_error() {
    ALTS_UI_LOCK_MESSAGE=$2
    ALTS_UI_LOCK_DETAIL=$3
    alts_ui_log "UI_LOCK=FAILED reason=$1 path=${alts_ui_lk:-unknown} detail=$3"
    return 1
}

# Linux fixtures have a kernel start identity. QNX uses its existing pidin
# arguments interface; /tmp scopes registrations to this device boot. Unknown
# process identity is kept conservative, never interpreted as a dead process.
alts_ui_process_identity() {
    if [ -r "/proc/$1/stat" ]; then
        awk '{sub(/^.*\) /, ""); if ($1 == "Z" || $1 == "X") print "dead"; else print "linux:" $20}' "/proc/$1/stat" 2>/dev/null
    elif command -v pidin >/dev/null 2>&1; then
        pidin -p "$1" arguments 2>/dev/null | awk -v p="$1" '$1 == p {sub(/^[ \t]*[0-9]+[ \t]+/, ""); print "qnx:" $0; exit}'
    fi
}

alts_ui_process_arguments() {
    if [ -r "/proc/$1/cmdline" ]; then
        tr '\000' ' ' < "/proc/$1/cmdline" 2>/dev/null
    elif command -v pidin >/dev/null 2>&1; then
        pidin -p "$1" arguments 2>/dev/null | awk -v p="$1" '$1 == p {sub(/^[ \t]*[0-9]+[ \t]+/, ""); print; exit}'
    fi
}

alts_ui_action_arguments() {
    case "$1" in
        *altscreen_install.sh*|*install_mmi_cockpit_carplay_*|*stop_mmi_cockpit_carplay_test.sh*|*finish_mmi_cockpit_carplay_test.sh*) return 0 ;;
    esac
    return 1
}

alts_ui_pid_record_live() {
    alts_ui_peer_pid=${1##*/}
    case "$alts_ui_peer_pid" in ''|*[!0-9]*|0|1) return 1 ;; esac
    [ "$alts_ui_peer_pid" -gt 1 ] 2>/dev/null || return 1
    kill -0 "$alts_ui_peer_pid" 2>/dev/null || return 1
    alts_ui_peer_now=$(alts_ui_process_identity "$alts_ui_peer_pid")
    [ "$alts_ui_peer_now" != dead ] || return 1
    alts_ui_peer_saved=$(cat "$1/identity" 2>/dev/null || true)
    if [ -z "$alts_ui_peer_saved" ]; then
        alts_ui_peer_args=$(alts_ui_process_arguments "$alts_ui_peer_pid")
        case "$1" in */workers/*) ;; *)
            # Bare shell arguments can be incomplete during publication. Only
            # a clearly different executable is positive evidence of PID reuse.
            alts_ui_peer_exe=${alts_ui_peer_args%% *}
            case "${alts_ui_peer_exe##*/}" in ''|sh|ksh|bash) ;; *)
                alts_ui_action_arguments "$alts_ui_peer_args" || return 1 ;;
            esac ;;
        esac
    fi
    if [ -n "$alts_ui_peer_saved" ] && [ -n "$alts_ui_peer_now" ] &&
       [ "$alts_ui_peer_saved" != "$alts_ui_peer_now" ]; then
        alts_ui_log "UI_LOCK_STALE reason=process_identity_changed pid=$alts_ui_peer_pid"
        return 1
    fi
    return 0
}

alts_ui_registration_live() {
    if alts_ui_pid_record_live "$1"; then return 0; fi
    for alts_ui_worker in "$1"/workers/*; do
        [ -d "$alts_ui_worker" ] && [ ! -L "$alts_ui_worker" ] || continue
        # A worker may have only its PID-named directory during publication.
        # Its shell remains part of this operation even without metadata yet.
        alts_ui_worker_pid=${alts_ui_worker##*/}
        case "$alts_ui_worker_pid" in ''|*[!0-9]*|0|1) continue ;; esac
        kill -0 "$alts_ui_worker_pid" 2>/dev/null || continue
        alts_ui_worker_now=$(alts_ui_process_identity "$alts_ui_worker_pid")
        [ "$alts_ui_worker_now" != dead ] || continue
        alts_ui_worker_saved=$(cat "$alts_ui_worker/identity" 2>/dev/null || true)
        if [ -n "$alts_ui_worker_saved" ] && [ -n "$alts_ui_worker_now" ] &&
           [ "$alts_ui_worker_saved" != "$alts_ui_worker_now" ]; then continue; fi
        alts_ui_log "UI_LOCK_WORKER_LIVE parent=${1##*/} worker=$alts_ui_worker_pid"
        return 0
    done
    return 1
}

# Only needed when adopting an old empty/invalid lock. Check for a live legacy
# action before migrating; old publishers do not have PID-named registrations.
alts_ui_legacy_action_live() {
    if command -v pidin >/dev/null 2>&1; then
        alts_ui_legacy_processes=$(pidin arguments 2>/dev/null) || return 2
    elif command -v ps >/dev/null 2>&1; then
        alts_ui_legacy_processes=$(ps -eo pid,args 2>/dev/null) || return 2
    else
        return 2
    fi
    alts_ui_legacy_found=$(printf '%s\n' "$alts_ui_legacy_processes" | awk -v self="$ALTS_UI_LOCK_PID" '
        $1 ~ /^[0-9]+$/ && $1 != self &&
        ($2 ~ /(^|\/)(sh|ksh|bash)$/ || $2 ~ /(^|\/)(altscreen_install|install_mmi_cockpit_carplay_[^ ]*|stop_mmi_cockpit_carplay_test|finish_mmi_cockpit_carplay_test)\.sh$/) &&
        /altscreen_install\.sh|install_mmi_cockpit_carplay_|stop_mmi_cockpit_carplay_test\.sh|finish_mmi_cockpit_carplay_test\.sh/ {print $1; exit}')
    [ -n "$alts_ui_legacy_found" ]
}

alts_ui_lock() {
    ALTS_UI_LOCK_PID=$$
    alts_ui_storage=$(alts_posix_tmp_dir "$1")
    alts_ui_lk="$alts_ui_storage/altscreen_gem_action.lock"
    alts_ui_log "UI_LOCK_STORAGE requested=$1 selected=$alts_ui_storage"
    ALTS_UI_LOCK_MESSAGE=""; ALTS_UI_LOCK_DETAIL=""
    [ -z "$ALTS_UI_LOCK" ] || { alts_ui_lock_error ALREADY_HELD "This action already holds the installation lock." "Run only one action per process."; return 1; }
    if [ -L "$alts_ui_lk" ] || { [ -e "$alts_ui_lk" ] && [ ! -d "$alts_ui_lk" ]; }; then
        alts_ui_lock_error INVALID_PATH "The installation lock path is invalid." "$alts_ui_lk"; return 1
    fi
    if [ -d "$alts_ui_lk" ]; then
        alts_ui_old_pid=$(cat "$alts_ui_lk/pid" 2>/dev/null || true)
        case "$alts_ui_old_pid" in ''|*[!0-9]*) ;; *)
            [ "$alts_ui_old_pid" -gt 1 ] 2>/dev/null || alts_ui_old_pid="" ;;
        esac
        case "$alts_ui_old_pid" in
            ''|*[!0-9]*|0|1)
                if [ ! -d "$alts_ui_lk/operations" ]; then
                    sleep 1
                    alts_ui_old_pid=$(cat "$alts_ui_lk/pid" 2>/dev/null || true)
                    case "$alts_ui_old_pid" in ''|*[!0-9]*) ;; *)
                        [ "$alts_ui_old_pid" -gt 1 ] 2>/dev/null || alts_ui_old_pid="" ;;
                    esac
                    case "$alts_ui_old_pid" in ''|*[!0-9]*|0|1)
                        alts_ui_legacy_action_live; alts_ui_legacy_rc=$?
                        if [ "$alts_ui_legacy_rc" = 0 ]; then
                            alts_ui_lock_error LEGACY_PUBLISHING "Another installation action is starting." "Owner PID: $alts_ui_legacy_found. Wait for its result."; return 1
                        elif [ "$alts_ui_legacy_rc" = 2 ]; then
                            alts_ui_lock_error LEGACY_UNVERIFIED "The old lock owner could not be verified." "Process inspection is unavailable; no files were changed."; return 1
                        fi ;;
                    esac
                fi ;;
        esac
        case "$alts_ui_old_pid" in ''|*[!0-9]*|0|1) ;; *)
            alts_ui_old_registered=0
            if [ -d "$alts_ui_lk/operations/$alts_ui_old_pid" ] &&
               alts_ui_pid_record_live "$alts_ui_lk/operations/$alts_ui_old_pid" &&
               [ -s "$alts_ui_lk/operations/$alts_ui_old_pid/ticket" ]; then
                alts_ui_old_registered=1
            fi
            if kill -0 "$alts_ui_old_pid" 2>/dev/null && [ "$alts_ui_old_registered" = 0 ]; then
                alts_ui_old_args=$(alts_ui_process_arguments "$alts_ui_old_pid")
                if [ -z "$alts_ui_old_args" ] || alts_ui_action_arguments "$alts_ui_old_args"; then
                    alts_ui_lock_error LEGACY_OWNER_LIVE "An earlier installation action has not exited." "Owner PID: $alts_ui_old_pid. Wait for it to finish."; return 1
                fi
                alts_ui_log "UI_LOCK_STALE reason=legacy_pid_reused pid=$alts_ui_old_pid"
            fi ;;
        esac
    fi
    if alts_ui_dir_error=$(alts_ui_ensure_dirs "$alts_ui_storage" "$alts_ui_lk" "$alts_ui_lk/operations" 2>&1); then :; else
        alts_ui_log "UI_LOCK_DIRECTORY_ERROR path=$alts_ui_storage error=$alts_ui_dir_error"
        alts_ui_lock_error DIRECTORY_CREATE "Cannot create the installation lock directory." "Storage: $alts_ui_storage. $alts_ui_dir_error"; return 1
    fi
    [ ! -L "$alts_ui_lk/operations" ] || { alts_ui_lock_error INVALID_PATH "The operation registry is invalid." "$alts_ui_lk/operations"; return 1; }
    ALTS_UI_LOCK="$alts_ui_lk/operations/$ALTS_UI_LOCK_PID"
    # PID reuse can leave our own old registration. No other live process has
    # this PID. Remove only known records and never a foreign/symlink directory.
    if [ -L "$ALTS_UI_LOCK" ]; then
        ALTS_UI_LOCK=""; alts_ui_lock_error INVALID_PATH "The process registration is invalid." "No files were changed."; return 1
    fi
    if [ -d "$ALTS_UI_LOCK" ]; then
        for alts_ui_worker in "$ALTS_UI_LOCK"/workers/*; do
            [ -d "$alts_ui_worker" ] && [ ! -L "$alts_ui_worker" ] || continue
            if alts_ui_pid_record_live "$alts_ui_worker"; then
                ALTS_UI_LOCK=""; alts_ui_lock_error WORKER_LIVE "An earlier installation worker is still running." "Worker PID: ${alts_ui_worker##*/}. Wait for its result."; return 1
            fi
            rm -f "$alts_ui_worker/identity" 2>/dev/null || true
            rmdir "$alts_ui_worker" 2>/dev/null || true
        done
        rmdir "$ALTS_UI_LOCK/workers" 2>/dev/null || true
        rm -f "$ALTS_UI_LOCK/identity" "$ALTS_UI_LOCK/token" "$ALTS_UI_LOCK/action" "$ALTS_UI_LOCK/ticket" "$ALTS_UI_LOCK/ticket.new" "$ALTS_UI_LOCK/phase" 2>/dev/null
        rmdir "$ALTS_UI_LOCK" 2>/dev/null || { ALTS_UI_LOCK=""; alts_ui_lock_error REGISTRATION_CREATE "Cannot recover the process registration." "Check the operation log."; return 1; }
    fi
    if ! mkdir "$ALTS_UI_LOCK" 2>/dev/null; then
        ALTS_UI_LOCK=""; alts_ui_lock_error REGISTRATION_CREATE "Cannot register this installation action." "Check temporary storage permissions and free space."; return 1
    fi
    ALTS_UI_LOCK_TOKEN="${ALTS_UI_LOCK_PID}_$(date +%Y%m%d_%H%M%S 2>/dev/null || echo unknown)"
    alts_ui_self_id=$(alts_ui_process_identity "$ALTS_UI_LOCK_PID")
    [ -n "$alts_ui_self_id" ] || alts_ui_log "UI_LOCK_IDENTITY=UNAVAILABLE pid=$ALTS_UI_LOCK_PID conservative_live_process_check=1"
    if ! { printf '%s\n' "$ALTS_UI_LOCK_TOKEN" > "$ALTS_UI_LOCK/token" &&
           printf '%s\n' "$alts_ui_self_id" > "$ALTS_UI_LOCK/identity" &&
           printf '%s\n' "${ALTS_UI_LOG_REL:-unknown}" > "$ALTS_UI_LOCK/action"; } 2>/dev/null; then
        alts_ui_unlock; alts_ui_lock_error METADATA_WRITE "Cannot save the installation operation identity." "Check temporary storage permissions and free space."; return 1
    fi
    alts_ui_max_ticket=0
    for alts_ui_peer in "$alts_ui_lk"/operations/*; do
        [ -d "$alts_ui_peer" ] && [ ! -L "$alts_ui_peer" ] || continue
        [ "$alts_ui_peer" != "$ALTS_UI_LOCK" ] || continue
        alts_ui_registration_live "$alts_ui_peer" || continue
        alts_ui_ticket=$(cat "$alts_ui_peer/ticket" 2>/dev/null || true)
        case "$alts_ui_ticket" in ''|*[!0-9]*) continue ;; esac
        # Keep arithmetic within QNX sh's signed range, refuse corrupted data.
        if [ "${#alts_ui_ticket}" -gt 8 ]; then
            alts_ui_unlock; alts_ui_lock_error INVALID_TICKET "An active operation has invalid lock metadata." "Check the operation log."; return 1
        fi
        [ "$alts_ui_ticket" -le "$alts_ui_max_ticket" ] || alts_ui_max_ticket=$alts_ui_ticket
    done
    ALTS_UI_LOCK_TICKET=$((alts_ui_max_ticket + 1))
    if ! { printf '%s\n' "$ALTS_UI_LOCK_TICKET" > "$ALTS_UI_LOCK/ticket.new" &&
           mv "$ALTS_UI_LOCK/ticket.new" "$ALTS_UI_LOCK/ticket"; } 2>/dev/null; then
        alts_ui_unlock; alts_ui_lock_error TICKET_WRITE "Cannot publish the installation operation." "Check temporary storage permissions and free space."; return 1
    fi
    for alts_ui_peer in "$alts_ui_lk"/operations/*; do
        [ -d "$alts_ui_peer" ] && [ ! -L "$alts_ui_peer" ] || continue
        [ "$alts_ui_peer" != "$ALTS_UI_LOCK" ] || continue
        alts_ui_wait=0
        while alts_ui_registration_live "$alts_ui_peer"; do
            alts_ui_ticket=$(cat "$alts_ui_peer/ticket" 2>/dev/null || true)
            case "$alts_ui_ticket" in ''|*[!0-9]*|0)
                if [ "$alts_ui_wait" -ge 3 ]; then
                    alts_ui_busy_pid=${alts_ui_peer##*/}
                    alts_ui_unlock; alts_ui_lock_error PUBLISHING "Another installation action is initializing." "Owner PID: $alts_ui_busy_pid. Retry after its result."; return 1
                fi
                alts_ui_wait=$((alts_ui_wait + 1)); sleep 1; continue ;;
            esac
            if [ "${#alts_ui_ticket}" -gt 8 ]; then
                alts_ui_unlock; alts_ui_lock_error INVALID_TICKET "An active operation has invalid lock metadata." "Check the operation log."; return 1
            fi
            alts_ui_peer_pid=${alts_ui_peer##*/}
            if [ "$alts_ui_ticket" -lt "$ALTS_UI_LOCK_TICKET" ] ||
               { [ "$alts_ui_ticket" -eq "$ALTS_UI_LOCK_TICKET" ] && [ "$alts_ui_peer_pid" -lt "$ALTS_UI_LOCK_PID" ]; }; then
                alts_ui_busy_action=$(cat "$alts_ui_peer/action" 2>/dev/null || echo unknown)
                alts_ui_unlock; alts_ui_lock_error BUSY "Another INSTALL / RESTORE is still running." "Owner PID: $alts_ui_peer_pid; operation: $alts_ui_busy_action"; return 1
            fi
            break
        done
    done
    # Keep old menu scripts from bypassing the new registry. Do not remove the
    # registry directory on release; dead tickets do not block future actions.
    if ! { printf '%s\n' "$ALTS_UI_LOCK_PID" > "$ALTS_UI_LOCK/pid.new" &&
           mv "$ALTS_UI_LOCK/pid.new" "$alts_ui_lk/pid" &&
           printf '%s\n' HELD > "$ALTS_UI_LOCK/phase"; } 2>/dev/null; then
        alts_ui_unlock; alts_ui_lock_error METADATA_WRITE "Cannot finalize the installation lock." "Check temporary storage permissions and free space."; return 1
    fi
    alts_ui_log "UI_LOCK=ACQUIRED pid=$ALTS_UI_LOCK_PID ticket=$ALTS_UI_LOCK_TICKET identity=$alts_ui_self_id operation=$ALTS_UI_LOG_REL"
    return 0
}
alts_ui_unlock() {
    [ -n "$ALTS_UI_LOCK" ] || return 0
    # Called during a publication failure too; this is always our PID-named
    # registration. No contender may delete another process's registration.
    if [ "${ALTS_UI_LOCK##*/}" = "$ALTS_UI_LOCK_PID" ] && [ ! -L "$ALTS_UI_LOCK" ]; then
        for alts_ui_worker in "$ALTS_UI_LOCK"/workers/*; do
            [ -d "$alts_ui_worker" ] && [ ! -L "$alts_ui_worker" ] || continue
            if alts_ui_pid_record_live "$alts_ui_worker"; then
                alts_ui_log "UI_LOCK_RELEASE=DEFERRED reason=worker_alive worker=${alts_ui_worker##*/}"
                ALTS_UI_LOCK=""; return 0
            fi
        done
        if [ "$(cat "$alts_ui_lk/pid" 2>/dev/null || true)" = "$ALTS_UI_LOCK_PID" ]; then
            rm -f "$alts_ui_lk/pid" 2>/dev/null || true
        fi
        rm -f "$ALTS_UI_LOCK/identity" "$ALTS_UI_LOCK/token" "$ALTS_UI_LOCK/action" "$ALTS_UI_LOCK/ticket" "$ALTS_UI_LOCK/ticket.new" "$ALTS_UI_LOCK/pid.new" "$ALTS_UI_LOCK/phase" 2>/dev/null || true
        rmdir "$ALTS_UI_LOCK/workers" 2>/dev/null || true
        rmdir "$ALTS_UI_LOCK" 2>/dev/null || true
        alts_ui_log "UI_LOCK=RELEASED pid=$ALTS_UI_LOCK_PID"
    fi
    ALTS_UI_LOCK=""
}
