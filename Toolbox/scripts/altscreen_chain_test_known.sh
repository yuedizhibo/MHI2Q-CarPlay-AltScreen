#!/bin/sh
# AltScreen chain-test controller: install / start / status / restore / collect.
#
# Owner's simplified flow, superseding the earlier READY/hash/authorization chain:
# INSTALL backs up the real device files and deploys the three-library overlay.
# Development packages read profile files directly; hardened release packages
# authenticate and materialize only the selected profile from the secure ARM
# payload. START arms it, RESTORE puts the original bytes back. Profile selection
# uses K1004/P1404 Current train or an explicit argument. No READY gate. First
# backups are kept.
#
# QNX /bin/sh only: no bash arrays, no GNU-only utilities.
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

# Secure release packages execute this source from an authenticated in-memory
# pipe.  The tiny on-disk launcher supplies the real entry and asset directory;
# source-tree/fixture runs keep the ordinary $0 behavior.
ALTSCREEN_CONTROLLER_ENTRY=${ALTSCREEN_CONTROLLER_ENTRY:-$0}

# Capture the entire command, including discovery errors, before SD is available.
# The outer shell owns the journal and preserves the inner command's exit status.
if [ "${ALTS_DIAG_CAPTURED:-0}" != 1 ]; then
    journal_root=""
    if [ "${ALTSCREEN_CHAIN_TESTING:-0}" = 1 ]; then
        journal_root=${ALTSCREEN_CHAIN_ROOT:-}
        case "$journal_root" in /tmp/*|/var/tmp/*) ;; *) exit 2 ;; esac
    fi
    journal_id="operation_$(date +%Y%m%d_%H%M%S)_$$.log"
    journal="$journal_root/tmp/altscreen_$journal_id"
    if ! (printf 'OP_BEGIN action=%s script=%s\n' "${1:-}" "$0" > "$journal") 2>/dev/null; then
        echo "WARN: diagnostic journal unavailable; continuing requested operation"
        ALTS_DIAG_CAPTURED=1; export ALTS_DIAG_CAPTURED
        if [ "$#" -gt 0 ]; then
            exec /bin/sh "$ALTSCREEN_CONTROLLER_ENTRY" "$@"
        else
            exec /bin/sh "$ALTSCREEN_CONTROLLER_ENTRY"
        fi
    fi
    if [ "$#" -gt 0 ]; then
        ALTS_DIAG_CAPTURED=1 /bin/sh "$ALTSCREEN_CONTROLLER_ENTRY" "$@" >> "$journal" 2>&1
    else
        ALTS_DIAG_CAPTURED=1 /bin/sh "$ALTSCREEN_CONTROLLER_ENTRY" >> "$journal" 2>&1
    fi
    journal_rc=$?
    printf 'OP_END action=%s rc=%s\n' "${1:-}" "$journal_rc" >> "$journal"
    journal_volume=$(sed -n 's/^DIAGNOSTICS_VOLUME=//p' "$journal" | tail -1)
    if [ -n "$journal_volume" ] && [ -d "$journal_volume/Toolbox" ]; then
        journal_target="$journal_volume/MMI-Cockpit-Carplay/logs/operations/$journal_id.log"
        if ensure_dirs "${journal_target%/*}" 2>/dev/null; then
            cp "$journal" "$journal_target.new" 2>/dev/null &&
                mv "$journal_target.new" "$journal_target" 2>/dev/null || rm -f "$journal_target.new"
        fi
    fi
    cat "$journal"
    rm -f "$journal" 2>/dev/null || true
    exit "$journal_rc"
fi

PROG=altscreen_chain_test
echo "CHAIN_SCRIPT_VERSION=AUG22_LIVI_TYPE111_DYNAMIC_PF_REALFRAME_PLAINTEXT_LOGS_20260917"
FIXED_ROOT=""
VOLUME=""
REBOOT_REQUIRED=NO

say() { echo "$1"; }
fail() { echo "FAIL: $1" >&2; exit 1; }

# ---------------------------------------------------------------- live paths
# Production paths are fixed. In testing every live path is prefixed with the
# fixture root, so nothing outside the fixture is ever touched.
LIVE_DIO_CANDIDATES="/eso/bin/apps/dio_manager /mnt/app/eso/bin/apps/dio_manager"
LIVE_LIBAIRPLAY="/eso/lib/libairplay.so"
LIVE_LIBSTOCK_REL="/eso/lib/libNmeBaseClasses.so"
LIVE_NME_CANDIDATES="/armle/usr/lib/libNmeBaseClasses.so /mnt/app/armle/usr/lib/libNmeBaseClasses.so /eso/lib/libNmeBaseClasses.so"
LIVE_JSON_SI="/mnt/system/etc/eso/production/smartphone_integrator.json"
LIVE_JSON_DIO="/mnt/system/etc/eso/production/dio_manager.json"
LIVE_PF_CONF="/mnt/system/etc/pf.conf"
LIVE_LIBTARGET="/mnt/app/root/carplay-altscreen/lib"
LEGACY_LIBTARGET="/mnt/app/root/lib-target"
# Runtime companions are owned by the router and staged under /mnt/app/root/carplay-altscreen.
# The known controller must not write /eso/hmi/engdefs, which is not reliably writable.
ARTIFACT_DIR=""

p() { printf '%s%s\n' "$FIXED_ROOT" "$1"; }

# ------------------------------------------------------------------- testing
if [ "${ALTSCREEN_CHAIN_TESTING:-0}" = "1" ]; then
    FIXED_ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
    [ -n "$FIXED_ROOT" ] || fail "ALTSCREEN_CHAIN_TESTING=1 requires ALTSCREEN_CHAIN_ROOT"
    [ -n "$VOLUME" ] || fail "ALTSCREEN_CHAIN_TESTING=1 requires ALTSCREEN_CHAIN_VOLUME"
    case "$FIXED_ROOT" in
        /tmp/*|/var/tmp/*) : ;;
        *) fail "testing root must be under /tmp or /var/tmp" ;;
    esac
    case "$VOLUME" in
        /tmp/*|/var/tmp/*) : ;;
        *) fail "testing volume must be under /tmp or /var/tmp" ;;
    esac
    [ -d "$VOLUME/Toolbox" ] || fail "testing volume has no Toolbox directory"
    mount_rw() { :; }
    mount_ro() { :; }
    have() { [ -e "$1" ]; }
else
    # Own discovery here: older Toolbox helpers only assign VOLUME and do not
    # provide mountsd_make_writable(). Do not source a version-dependent helper.
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -d "$candidate/Toolbox" ]; then VOLUME=$candidate; break; fi
    done
    [ -n "${VOLUME:-}" ] || fail "no Toolbox SD card discovered"
    mount_rw() { mount -uw "$1" || { say "FAIL: cannot mount $1 writeable"; return 1; }; }
    mount_ro() { mount -ur "$1"; }
    have() { [ -e "$1" ]; }
fi
# Shared by fixtures and production. Test actual file creation first: a legacy
# launcher may already have mounted the SD writable. A mount return code alone
# is not evidence that writing worked (or that an already-writable SD failed).
sd_writable() {
    sd_probe="$VOLUME/.altscreen_write_probe.$$"
    if ( : > "$sd_probe" ) 2>/dev/null; then
        rm -f "$sd_probe" || return 1
        say "SD_WRITE=PASS already_writable=1"
        return 0
    fi
    mount_rw "$VOLUME" || say "WARN: SD remount failed; checking actual write access"
    if ( : > "$sd_probe" ) 2>/dev/null; then
        rm -f "$sd_probe" || return 1
        say "SD_WRITE=PASS after_remount=1"
        return 0
    fi
    say "SD_WRITE=FAILED volume=$VOLUME"
    return 1
}
ARTIFACT_DIR="$VOLUME/Toolbox/carplay_alt_screen"
# Profile artifacts are ordinary files under profiles/<K1004|P1404>.
FIRMWARE_PROFILE=""
PAYLOAD_DIR="$ARTIFACT_DIR"
say "DIAGNOSTICS_VOLUME=$VOLUME"
SD_SCRIPTS="$VOLUME/Toolbox/scripts"
PROBE_MARKER="$(p /mnt/app/root/carplay-altscreen/state/fullchain_probe)"

SD_ROOT="$VOLUME/MMI-Cockpit-Carplay"
STATE_DIR="$SD_ROOT/state"
LOG_ROOT="$SD_ROOT/logs"
BACKUP_ROOT="$SD_ROOT/backup"
STAGING_ROOT="$SD_ROOT/staging"
BACKUP_DIR="$BACKUP_ROOT/original"
STAGE_DIR="$STAGING_ROOT/original"
LOCK_FILE="$STATE_DIR/.chain_test.lock"
LEGACY_STATE_DIR="$VOLUME/Log/MMI-Cockpit-Carplay/current"
LEGACY_BACKUP_ROOT="$VOLUME/Backup/AltScreenChain"
LOCK_BOOT_TOKEN_FILE="$(p /tmp/MMI-Cockpit-Carplay.lock.boot_token)"
LOCK_OWNER_TAG="MMI-Cockpit-Carplay-Known"
BACKUP_MANIFEST="$BACKUP_DIR/manifest.txt"
COMPLETE_MARKER="$BACKUP_DIR/COMPLETE"
FIREWALL_BACKUP_DIR="$BACKUP_ROOT/firewall-original"
FIREWALL_BACKUP_FILE="$FIREWALL_BACKUP_DIR/pf.conf"
FIREWALL_COMPLETE="$FIREWALL_BACKUP_DIR/COMPLETE"
FIREWALL_BEGIN="# BEGIN ALTSCREEN TYPE111 FIREWALL"
FIREWALL_END="# END ALTSCREEN TYPE111 FIREWALL"
INSTALLED_MARKER="$STATE_DIR/INSTALLED"
BOOT_BACKUP="$BACKUP_ROOT/boot-diagnostics"
BOOT_ENABLED="$(p /mnt/app/root/carplay-altscreen/state/diagnostics.enabled)"

migrate_legacy_dir() {
    legacy=$1; target=$2; label=$3
    [ -e "$target" ] && return 0
    [ -d "$legacy" ] || return 0
    [ -f "$legacy/COMPLETE" ] || { say "FAIL: legacy $label backup is incomplete: $legacy"; return 1; }
    stage="$STAGING_ROOT/import_${label}_$$"
    rm -rf "$stage" 2>/dev/null || true
    ensure_dirs "$stage" || return 1
    cp -R "$legacy/." "$stage/" || { rm -rf "$stage"; return 1; }
    mv "$stage" "$target" || { rm -rf "$stage"; return 1; }
    say "LAYOUT_MIGRATED kind=$label from=$legacy to=$target legacy_preserved=YES"
    return 0
}
migrate_legacy_layout() {
    ensure_dirs "$STATE_DIR" "$LOG_ROOT/operations" "$LOG_ROOT/boots" "$LOG_ROOT/sessions" \
                "$LOG_ROOT/adaptive" "$BACKUP_ROOT" "$STAGING_ROOT" || return 1
    if [ -d "$LEGACY_STATE_DIR" ]; then
        for leaf in INSTALLED firmware_profile.txt ARMED ARMED_MUTATE ARMED_IAP2 ARMED_INFO \
                    ARMED_FEATURE ARMED_CREATE111 ACTIVE FORCE_START FULL_CHAIN_MODE \
                    NATIVE_DISPLAY_MODE IAP2_PROFILE run_id session_path RESTORE_PENDING_REBOOT; do
            [ -e "$LEGACY_STATE_DIR/$leaf" ] || continue
            [ -e "$STATE_DIR/$leaf" ] || cp "$LEGACY_STATE_DIR/$leaf" "$STATE_DIR/$leaf" || return 1
        done
    fi
    migrate_legacy_dir "$LEGACY_BACKUP_ROOT/original" "$BACKUP_DIR" original || return 1
    migrate_legacy_dir "$LEGACY_BACKUP_ROOT/firewall-original" "$FIREWALL_BACKUP_DIR" firewall || return 1
    migrate_legacy_dir "$LEGACY_BACKUP_ROOT/boot-diagnostics" "$BOOT_BACKUP" boot_diagnostics || return 1
    return 0
}

# --------------------------------------------------------------------- lock
LOCK_HELD=0
lock_boot_token() {
    if [ -s "$LOCK_BOOT_TOKEN_FILE" ]; then
        cat "$LOCK_BOOT_TOKEN_FILE"
        return 0
    fi
    token="$(date +%Y%m%d_%H%M%S 2>/dev/null || echo boot)_$$"
    token_dir=$(dirname -- "$LOCK_BOOT_TOKEN_FILE")
    tmp="${LOCK_BOOT_TOKEN_FILE}.new.$$"
    if ensure_dirs "$token_dir" 2>/dev/null &&
       ( umask 077; printf '%s\n' "$token" > "$tmp" ) 2>/dev/null; then
        if [ -s "$LOCK_BOOT_TOKEN_FILE" ]; then
            rm -f "$tmp" 2>/dev/null || true
        elif ! mv "$tmp" "$LOCK_BOOT_TOKEN_FILE" 2>/dev/null; then
            rm -f "$tmp" 2>/dev/null || true
        fi
        if [ -s "$LOCK_BOOT_TOKEN_FILE" ]; then
            cat "$LOCK_BOOT_TOKEN_FILE"
            return 0
        fi
    else
        rm -f "$tmp" 2>/dev/null || true
    fi
    echo "WARN: volatile boot token unavailable; stale-lock recovery is PID-only for this boot" >&2
    printf '%s\n' unavailable
    return 0
}
lock_publish_owner() {
    boot=$1
    printf '%s\n' "$LOCK_OWNER_TAG" > "$LOCK_FILE/owner" || return 1
    printf '%s\n' "$$" > "$LOCK_FILE/pid" || return 1
    printf '%s\n' "$boot" > "$LOCK_FILE/boot" || return 1
    printf '%s\n' "${CMD:-unknown}" > "$LOCK_FILE/action" || return 1
    return 0
}
lock_remove_metadata() {
    rm -f "$LOCK_FILE/owner" "$LOCK_FILE/pid" "$LOCK_FILE/boot" "$LOCK_FILE/action" 2>/dev/null || return 1
}
lock_reap_stale() {
    boot=$1
    [ -d "$LOCK_FILE" ] || return 0

    # Legacy known builds created an empty directory. Give a concurrently
    # starting new controller time to publish metadata before treating it stale.
    if [ ! -f "$LOCK_FILE/pid" ]; then
        sleep 1
        if [ ! -f "$LOCK_FILE/pid" ]; then
            if rmdir "$LOCK_FILE" 2>/dev/null; then
                say "LOCK_STALE_RECOVERED reason=legacy_empty"
                return 0
            fi
            fail "operation lock exists without valid owner metadata"
        fi
    fi

    old_owner=$(cat "$LOCK_FILE/owner" 2>/dev/null || true)
    old_pid=$(cat "$LOCK_FILE/pid" 2>/dev/null || true)
    old_boot=$(cat "$LOCK_FILE/boot" 2>/dev/null || true)
    case "$old_owner" in
      ""|"$LOCK_OWNER_TAG") ;;
      *) fail "operation lock is owned by an unknown controller" ;;
    esac

    stale=0
    reason=""
    if [ -n "$old_boot" ] && [ "$old_boot" != "unavailable" ] && [ "$boot" != "unavailable" ] && [ "$old_boot" != "$boot" ]; then
        stale=1; reason=previous_boot
    else
        case "$old_pid" in
          ''|*[!0-9]*) stale=1; reason=invalid_pid ;;
          *)
            if kill -0 "$old_pid" 2>/dev/null; then
                fail "another AltScreen operation is in progress pid=$old_pid"
            fi
            stale=1; reason=dead_pid
            ;;
        esac
    fi

    if [ "$stale" = 1 ]; then
        lock_remove_metadata || fail "stale operation lock metadata could not be removed"
        rmdir "$LOCK_FILE" 2>/dev/null || fail "stale operation lock directory could not be removed"
        [ ! -e "$LOCK_FILE" ] || fail "stale operation lock still exists after cleanup"
        say "LOCK_STALE_RECOVERED reason=$reason old_pid=${old_pid:-unknown}"
    fi
}
lock_acquire() {
    sd_writable || fail "cannot make SD writable"
    migrate_legacy_layout || fail "cannot initialize unified SD layout"
    ensure_dirs "$(dirname -- "$LOCK_FILE")" || fail "cannot create lock directory"
    boot=$(lock_boot_token) || fail "cannot establish volatile boot token for operation lock"
    if ! mkdir "$LOCK_FILE" 2>/dev/null; then
        lock_reap_stale "$boot"
        mkdir "$LOCK_FILE" 2>/dev/null || fail "another AltScreen operation is in progress"
    fi
    if ! lock_publish_owner "$boot"; then
        lock_remove_metadata >/dev/null 2>&1 || true
        rmdir "$LOCK_FILE" >/dev/null 2>&1 || true
        fail "operation lock owner metadata could not be published"
    fi
    LOCK_HELD=1
    say "LOCK_ACQUIRED pid=$$ action=${CMD:-unknown}"
}
lock_release() {
    [ "$LOCK_HELD" = 1 ] || return 0
    owner=$(cat "$LOCK_FILE/owner" 2>/dev/null || true)
    pid=$(cat "$LOCK_FILE/pid" 2>/dev/null || true)
    [ "$owner" = "$LOCK_OWNER_TAG" ] || { say "FAIL: refusing to release unowned operation lock"; return 1; }
    [ "$pid" = "$$" ] || { say "FAIL: refusing to release operation lock owned by pid=$pid"; return 1; }
    lock_remove_metadata || { say "FAIL: operation lock metadata could not be removed"; return 1; }
    rmdir "$LOCK_FILE" 2>/dev/null || { say "FAIL: operation lock directory could not be removed"; return 1; }
    [ ! -e "$LOCK_FILE" ] || { say "FAIL: operation lock still exists after release"; return 1; }
    LOCK_HELD=0
    say "LOCK_RELEASE=PASS"
    return 0
}
cleanup() {
    cleanup_rc=$?
    trap - 0
    trap '' 1 2 15
    if [ "${LIVE_DIRTY:-0}" = 1 ] && [ "$cleanup_rc" -ne 0 ]; then
        if mount_rw "$(p /mnt/app)" && mount_rw "$(p /mnt/system)"; then
            MR_APP=1; MR_SYS=1
        fi
        if restore_originals; then
            echo 'ROLLBACK=PASS'
            rm -f "$INSTALLED_MARKER" "$STATE_DIR/ACTIVE" "$STATE_DIR/ARMED" "$STATE_DIR/ARMED_MUTATE"
        else
            echo 'ROLLBACK=FAILED'; touch "$STATE_DIR/DISABLE_FAILED"
        fi
    fi
    finish_mounts || cleanup_rc=1
    lock_release
    exit "$cleanup_rc"
}
trap cleanup 0
trap 'say "INTERRUPTED"; exit 130' 1 2 15

# ------------------------------------------------------------------ utilities
# cmp/cksum only prove that a copy is byte-identical. They are never used as a
# firmware compatibility or authorization decision.
same_bytes() {
    [ -f "$1" ] && [ -f "$2" ] || return 1
    cmp -s "$1" "$2" 2>/dev/null
}
nonempty() { [ -s "$1" ]; }
copy_snapshot() {
    cp "$1" "$2"
}

# Locate the first existing candidate, echoing it. No PATH is invented.
locate_first() {
    for cand in $1; do
        if [ -e "$(p "$cand")" ]; then printf '%s\n' "$cand"; return 0; fi
    done
    return 1
}

# Copy with a temp name then rename, so an in-use executable is never truncated.
stage_and_publish() (
    src=$1; dst=$2; mode=$3
    dir=$(dirname -- "$dst")
    tmp="$dir/.$(basename -- "$dst").new.$$"
    cp "$src" "$tmp" || { say "FAIL: cannot copy $src to $tmp"; rm -f "$tmp"; return 1; }
    chmod "$mode" "$tmp" || { say "FAIL: cannot chmod $tmp"; rm -f "$tmp"; return 1; }
    same_bytes "$src" "$tmp" || { rm -f "$tmp"; return 1; }
    mv "$tmp" "$dst" || { say "FAIL: cannot publish $dst"; rm -f "$tmp"; return 1; }
    return 0
)

# Historical installs used the same in-directory staging convention. Remove
# only unpublished files from interrupted writes when restoring that profile.
cleanup_stale_system_publish_files() {
    for rel in "$LIVE_JSON_SI" "$LIVE_JSON_DIO" "$LIVE_PF_CONF" \
               /mnt/system/etc/boot/startup.sh /etc/boot/startup.sh; do
        dst=$(p "$rel")
        dir=$(dirname -- "$dst")
        base=$(basename -- "$dst")
        for stale in "$dir/.$base.new."*; do
            [ -e "$stale" ] || [ -L "$stale" ] || continue
            suffix=${stale##*.}
            case "$suffix" in ''|*[!0-9]*) continue ;; esac
            [ ! -d "$stale" ] || { say "FAIL: unexpected staging directory: $stale"; return 1; }
            rm -f "$stale" || return 1
            say "STALE_SYSTEM_STAGE_REMOVED=$stale"
        done
    done
}

# ---------------------------------------------------------------- subcommands
cmd_install() {
    lock_acquire
    check_sources || { lock_release; exit 1; }
    MR_SYS=0; MR_APP=0
    rollback_needed=0
    if ! backup_originals; then
        say "FAIL: backup incomplete; no mutation was attempted"
        finish_mounts
        lock_release
        exit 1
    fi
    if ! backup_firewall; then
        say "FAIL: firewall backup incomplete; no mutation was attempted"
        finish_mounts
        lock_release
        exit 1
    fi
    mount_rw "$(p /mnt/system)" || exit 1; MR_SYS=1
    mount_rw "$(p /mnt/app)" || exit 1; MR_APP=1
    LIVE_DIRTY=1
    if ! restore_overlay_baseline; then
        say "FAIL: could not retire the previous overlay location; attempting restore"
        rollback_needed=1
    fi
    if [ "$rollback_needed" != 1 ] && ! deploy_overlay; then
        say "FAIL: overlay deployment failed; attempting restore from originals"
        rollback_needed=1
    fi
    if [ "$rollback_needed" = 1 ]; then
        if restore_originals; then
            LIVE_DIRTY=0
            say "ROLLBACK=OK state=RESTORED"
            finish_mounts; lock_release; exit 1
        fi
        say "ROLLBACK=FAILED state=DISABLE_FAILED; run install/restore again before reboot"
        finish_mounts; lock_release; exit 1
    fi
    # Package the menu and the chain scripts onto the device.
    install_scripts_and_menu || { err_install=1; exit "$err_install"; }
    install_boot_diagnostics || exit 1
    # Move the CarPlay library search root into the owned runtime tree, then
    # isolate only known competing hooks. The original JSON remains in backup.
    awk -v libdir="$LIVE_LIBTARGET" -v legacy_libdir="$LEGACY_LIBTARGET" \
        -f "$SD_SCRIPTS/altscreen_preload.awk" "$(p "$LIVE_JSON_SI")" > "$STATE_DIR/config.libpath" || exit 1
    awk -v allow_absent=1 -v exclude=/mnt/app/root/hooks/libcarplay_hook.so \
        -v exclude2=/mnt/app/root/hooks/libcp_mirror.so -f "$SD_SCRIPTS/altscreen_preload.awk" \
        "$STATE_DIR/config.libpath" > "$STATE_DIR/config.stage1" || exit 1
    awk -v allow_absent=1 -v exclude=/mnt/app/root/hooks/libcarplay_altscreen.so \
        -f "$SD_SCRIPTS/altscreen_preload.awk" "$STATE_DIR/config.stage1" > "$STATE_DIR/config.stage2" || exit 1
    stage_and_publish "$STATE_DIR/config.stage2" "$(p "$LIVE_JSON_SI")" 644 || exit 1
    rm -f "$STATE_DIR/config.libpath" "$STATE_DIR/config.stage1" "$STATE_DIR/config.stage2" || exit 1
    remove_legacy_firewall_rule || exit 1
    ensure_dirs "$STATE_DIR" || fail "cannot create state directory"
    date > "$STATE_DIR/installed_at.txt" 2>/dev/null || true
    touch "$INSTALLED_MARKER" || fail "cannot publish INSTALLED marker"
    rm -f "$STATE_DIR/ARMED" "$STATE_DIR/ARMED_MUTATE" "$STATE_DIR/ARMED_IAP2" \
          "$STATE_DIR/ARMED_INFO" "$STATE_DIR/ARMED_FEATURE" "$STATE_DIR/ARMED_CREATE111" \
          "$STATE_DIR/ACTIVE" "$STATE_DIR/FORCE_START" "$STATE_DIR/FULL_CHAIN_MODE" "$STATE_DIR/NATIVE_DISPLAY_MODE" 2>/dev/null || true
    say "DIO_BACKED_UP_AND_UNCHANGED=YES dio=$(p "$(locate_first "$LIVE_DIO_CANDIDATES" || echo /eso/bin/apps/dio_manager)")"
    say "OVERLAY=libairplax.so,libairplay.so,libNmeBaseClasses.so target=$(p "$LIVE_LIBTARGET")"
    say "FIREWALL=TYPE111_RUNTIME_EXACT_PORT interface=carplay0 persistent_high_port_range=ABSENT"
    say "ARMED=NONE reboot_required=YES"
    finish_mounts || exit 1
    LIVE_DIRTY=0
    printf '%s\n' "$FIRMWARE_PROFILE" > "$STATE_DIR/firmware_profile.txt" 2>/dev/null ||
        say "WARN: could not persist the selected firmware profile"
    say "INSTALL=PASS backup=$BACKUP_DIR"
    lock_release
    return 0
}

# -------------------------------------------------- firmware profile select
# Select a payload, without comparing factory binary checksums or whitelists.
# Current train is used only to choose K1004 vs P1404. An explicit SD selection
# overrides it; identities.txt is historical build evidence and is not read.
select_firmware_profile() {
    FIRMWARE_PROFILE=LEGACY_FLAT
    PAYLOAD_DIR="$ARTIFACT_DIR"
        profiles_dir="$ARTIFACT_DIR/profiles"
    if [ ! -d "$profiles_dir" ]; then
        say "FIRMWARE_PROFILE=LEGACY_FLAT"
        return 0
    fi
    selected=${REQUESTED_PROFILE:-}
    selection_source=explicit_argument
    if [ -z "$selected" ] && [ -f "$profiles_dir/SELECT_PROFILE" ]; then
        selected=$(tr -d '\r\n ' < "$profiles_dir/SELECT_PROFILE")
        selection_source=SD_SELECT_PROFILE
    fi
    if [ -z "$selected" ]; then
        for version_rel in /net/rcc/dev/shmem/version.txt /dev/shmem/version.txt /net/mmx/dev/shmem/version.txt; do
            version_path=$(p "$version_rel")
            [ -r "$version_path" ] || continue
            train=$(sed -n '/Current train/p' "$version_path" | head -n 1)
            case "$train" in
                *AUG22_K1004*) selected=K1004 ;;
                *AUG22_P1404*) selected=P1404 ;;
                *) continue ;;
            esac
            selection_source="$version_rel"
            say "FIRMWARE_TRAIN=$train"
            break
        done
    fi
    case "$selected" in
        K1004|P1404) : ;;
        '') say "SELECT_PROFILE_REQUIRED: cannot determine K1004/P1404 from Current train; put K1004 or P1404 in Toolbox/carplay_alt_screen/profiles/SELECT_PROFILE"; return 1 ;;
        *) say "FAIL: SELECT_PROFILE must name the K1004 or P1404 payload"; return 1 ;;
    esac
    FIRMWARE_PROFILE=$selected
    PAYLOAD_DIR="$profiles_dir/$selected"
    say "FIRMWARE_PROFILE=$selected source=$selection_source firmware_checksum_gate=DISABLED payload=PLAINTEXT"
    return 0
}

check_sources() {
    ensure_dirs "$STATE_DIR" || return 1
    [ -s "$VOLUME/Toolbox/GEM/mqb-carplayAltScreen.esd" ] || return 1
    [ -s "$SD_SCRIPTS/altscreen_preload.awk" ] || { say "FAIL: package altscreen_preload.awk missing"; return 1; }
    dio_rel=$(locate_first "$LIVE_DIO_CANDIDATES") || { say "FAIL: stock dio_manager not found in the fixed candidate list"; return 1; }
    nme_rel=$(locate_first "$LIVE_NME_CANDIDATES") || { say "FAIL: stock libNmeBaseClasses.so not found in the fixed candidate list"; return 1; }
    for rel in "$dio_rel" "$nme_rel"; do
        nonempty "$(p "$rel")" || { say "FAIL: $rel is missing or empty"; return 1; }
    done
    for rel in "$LIVE_LIBAIRPLAY" "$LIVE_JSON_SI" "$LIVE_JSON_DIO" "$LIVE_PF_CONF"; do
        nonempty "$(p "$rel")" || { say "FAIL: required source $rel is missing or empty"; return 1; }
    done
    # Choose the payload from the train or the owner's selection, never binary CRC.
    select_firmware_profile || return 1
    for lib in libairplay.so libairplax.so libNmeBaseClasses.so; do
        [ -s "$PAYLOAD_DIR/$lib" ] || { say "FAIL: missing library $lib in $PAYLOAD_DIR"; return 1; }
    done
    for lib in libairplay.so libairplax.so libNmeBaseClasses.so; do
        :
    done
    for s in altscreen_chain_test.sh altscreen_boot_diag.sh altscreen_live_diag.sh install_mmi_cockpit_carplay_rx.sh start_mmi_cockpit_carplay_test.sh \
             start_mmi_cockpit_carplay_rx_test.sh force_start_mmi_cockpit_carplay_rx_test.sh \
             stop_mmi_cockpit_carplay_test.sh status_mmi_cockpit_carplay_test.sh \
             finish_mmi_cockpit_carplay_test.sh; do
        src="$SD_SCRIPTS/$s"
        [ -f "$src" ] || { say "FAIL: $s is required for installation"; return 1; }
        case "$s" in
            *.sh) sh -n "$src" || { say "FAIL: $s has a shell syntax error"; return 1; } ;;
            *) [ -x "$src" ] || { say "FAIL: required executable is not executable: $s"; return 1; } ;;
        esac
    done
    locate_first "/mnt/system/etc/boot/startup.sh /etc/boot/startup.sh" >/dev/null || {
        say "FAIL: boot startup.sh not found; diagnostics cannot be installed"; return 1;
    }
    say "SOURCES_CHECK=PASS dio=$dio_rel nme=$nme_rel"
    return 0
}

# Back up the actual device bytes. The first COMPLETE backup is never replaced.
backup_originals() (
    if [ -f "$COMPLETE_MARKER" ]; then
        verify_backup || return 1
        say "BACKUP=EXISTING kept (first originals are reused)"
        return 0
    fi
    STAGE_DIR="${STAGE_DIR}.$$"
    ensure_dirs "$STAGE_DIR/files" || return 1
    : > "$STAGE_DIR/manifest.txt" || return 1
    for rel in "$dio_rel" "$LIVE_LIBAIRPLAY" "$nme_rel" "$LIVE_JSON_SI" "$LIVE_JSON_DIO"; do
        src=$(p "$rel")
        nonempty "$src" || { say "FAIL: cannot back up missing source $rel"; return 1; }
        dst="$STAGE_DIR/files/$(echo "$rel" | tr '/' '_')"
        cp "$src" "$dst" || { say "FAIL: copy failed for $rel"; return 1; }
        same_bytes "$src" "$dst" || { say "FAIL: backup copy differs for $rel"; return 1; }
        cksum < "$dst" > "$dst.cksum" || return 1
        printf '%s\n' "$rel" >> "$STAGE_DIR/manifest.txt" || return 1
    done
    # Overlay presence is recorded, including its actual pre-install location.
    # On an upgraded vehicle the old product overlay lives in lib-target while
    # the freshly staged unified runtime lib/ directory is still empty.
    overlay_source_dir="$LIVE_LIBTARGET"
    new_overlay_present=0; legacy_overlay_present=0
    for n in libairplay.so libairplax.so libNmeBaseClasses.so; do
        [ ! -f "$(p "$LIVE_LIBTARGET")/$n" ] || new_overlay_present=1
        [ ! -f "$(p "$LEGACY_LIBTARGET")/$n" ] || legacy_overlay_present=1
    done
    if [ "$new_overlay_present" != 1 ] && [ "$legacy_overlay_present" = 1 ]; then
        overlay_source_dir="$LEGACY_LIBTARGET"
    fi
    printf '%s\n' "$overlay_source_dir" > "$STAGE_DIR/overlay_dir.txt"
    : > "$STAGE_DIR/overlay_present.txt"
    for n in libairplay.so libairplax.so libNmeBaseClasses.so; do
        if [ -f "$(p "$overlay_source_dir")/$n" ]; then
            cp "$(p "$overlay_source_dir")/$n" "$STAGE_DIR/files/overlay_$n" || return 1
            same_bytes "$(p "$overlay_source_dir")/$n" "$STAGE_DIR/files/overlay_$n" || return 1
            cksum < "$STAGE_DIR/files/overlay_$n" > "$STAGE_DIR/files/overlay_$n.cksum" || return 1
            printf '%s\n' "$n" >> "$STAGE_DIR/overlay_present.txt"
        fi
    done
    ensure_dirs "$(dirname -- "$BACKUP_DIR")" || return 1
    if [ -e "$BACKUP_DIR" ]; then
        say "FAIL: $BACKUP_DIR exists without COMPLETE; refusing to overwrite a partial backup"
        return 1
    fi
    mv "$STAGE_DIR" "$BACKUP_DIR" || return 1
    # COMPLETE is published only after every file and manifest is in place.
    cp "$BACKUP_DIR/manifest.txt" "$BACKUP_DIR/manifest.verify" 2>/dev/null || true
    same_bytes "$BACKUP_MANIFEST" "$BACKUP_DIR/manifest.verify" || { say "FAIL: manifest verification failed"; return 1; }
    rm -f "$BACKUP_DIR/manifest.verify"
    date > "$COMPLETE_MARKER" 2>/dev/null || touch "$COMPLETE_MARKER"
    say "BACKUP=COMPLETE dir=$BACKUP_DIR"
    return 0
)

# The firewall is a separate, backward-compatible transaction. Existing cars
# may already hold the original five-file COMPLETE backup from an earlier build.
verify_firewall_backup() (
    [ -f "$FIREWALL_COMPLETE" ] && [ -s "$FIREWALL_BACKUP_FILE" ] &&
        [ -f "$FIREWALL_BACKUP_FILE.cksum" ] && [ -f "$FIREWALL_BACKUP_DIR/path" ] || return 1
    [ "$(cat "$FIREWALL_BACKUP_DIR/path")" = "$LIVE_PF_CONF" ] || return 1
    [ "$(cksum < "$FIREWALL_BACKUP_FILE")" = "$(cat "$FIREWALL_BACKUP_FILE.cksum")" ] || return 1
)

backup_firewall() (
    if [ -f "$FIREWALL_COMPLETE" ]; then
        verify_firewall_backup || return 1
        say "FIREWALL_BACKUP=EXISTING kept"
        return 0
    fi
    [ ! -e "$FIREWALL_BACKUP_DIR" ] || {
        say "FAIL: $FIREWALL_BACKUP_DIR exists without COMPLETE; refusing to overwrite it"; return 1; }
    firewall_stage="${FIREWALL_BACKUP_DIR}.staging.$$"
    ensure_dirs "$firewall_stage" || return 1
    cp "$(p "$LIVE_PF_CONF")" "$firewall_stage/pf.conf" || return 1
    same_bytes "$(p "$LIVE_PF_CONF")" "$firewall_stage/pf.conf" || return 1
    cksum < "$firewall_stage/pf.conf" > "$firewall_stage/pf.conf.cksum" || return 1
    printf '%s\n' "$LIVE_PF_CONF" > "$firewall_stage/path" || return 1
    mv "$firewall_stage" "$FIREWALL_BACKUP_DIR" || return 1
    touch "$FIREWALL_COMPLETE" || return 1
    verify_firewall_backup || return 1
    say "FIREWALL_BACKUP=COMPLETE file=$FIREWALL_BACKUP_FILE"
)

strip_firewall_block() {
    awk -v begin="$FIREWALL_BEGIN" -v end="$FIREWALL_END" '
        $0 == begin { if (inside || seen) exit 9; inside=1; seen=1; next }
        $0 == end { if (!inside) exit 9; inside=0; next }
        !inside { print }
        END { if (inside) exit 9 }
    ' "$1"
}

remove_legacy_firewall_rule() (
    firewall_live=$(p "$LIVE_PF_CONF")
    strip_firewall_block "$firewall_live" > "$STATE_DIR/pf.clean" || return 1
    if same_bytes "$STATE_DIR/pf.clean" "$firewall_live"; then
        rm -f "$STATE_DIR/pf.clean" "$STATE_DIR/FIREWALL_PATCHED"
        say "FIREWALL_LEGACY_STATIC=ABSENT runtime_scope=exact_session_port"
        return 0
    fi
    stage_and_publish "$STATE_DIR/pf.clean" "$firewall_live" 644 || return 1
    rm -f "$STATE_DIR/pf.clean" "$STATE_DIR/FIREWALL_PATCHED" || return 1
    say "FIREWALL_LEGACY_STATIC=REMOVED runtime_scope=exact_session_port"
)

restore_firewall_original() (
    firewall_live=$(p "$LIVE_PF_CONF")
    if [ ! -f "$FIREWALL_COMPLETE" ]; then
        if [ -f "$STATE_DIR/FIREWALL_PATCHED" ] || grep -qF "$FIREWALL_BEGIN" "$firewall_live" 2>/dev/null; then
            say "FAIL: firewall is patched but its original backup is unavailable"
            return 1
        fi
        say "FIREWALL_RESTORE=NOT_REQUIRED legacy_install=1"
        return 0
    fi
    verify_firewall_backup || return 1
    stage_and_publish "$FIREWALL_BACKUP_FILE" "$firewall_live" 644 || return 1
    same_bytes "$FIREWALL_BACKUP_FILE" "$firewall_live" || return 1
    rm -f "$STATE_DIR/FIREWALL_PATCHED" || return 1
    say "FIREWALL_RESTORE=PASS original_bytes=VERIFIED"
)

deploy_overlay() {
    art="$PAYLOAD_DIR"
    ensure_dirs "$(p "$LIVE_LIBTARGET")" || return 1
    # libairplax.so first: the proxy in libairplay.so refers to it.
    for pair in "libairplax.so:libairplax.so" "libairplay.so:libairplay.so" "libNmeBaseClasses.so:libNmeBaseClasses.so"; do
        srcname=${pair%%:*}; dstname=${pair##*:}
        dst="$(p "$LIVE_LIBTARGET")/$dstname"
        src="$art/$srcname"
        [ -f "$src" ] || { say "FAIL: overlay member $srcname is missing from the package"; return 1; }
        stage_and_publish "$src" "$dst" 755 || return 1
        same_bytes "$src" "$dst" || { say "FAIL: deployed $dstname differs from the package"; return 1; }
    done
    say "OVERLAY_DEPLOYED=PASS target=$(p "$LIVE_LIBTARGET") members=3"
    return 0
}

install_scripts_and_menu() {
    sd_scripts="$VOLUME/Toolbox/scripts"
    [ -d "$sd_scripts" ] || { say "FAIL: $sd_scripts not found on the SD card"; return 1; }
    for s in altscreen_chain_test.sh altscreen_boot_diag.sh altscreen_live_diag.sh install_mmi_cockpit_carplay_rx.sh start_mmi_cockpit_carplay_test.sh \
             start_mmi_cockpit_carplay_rx_test.sh force_start_mmi_cockpit_carplay_rx_test.sh \
             stop_mmi_cockpit_carplay_test.sh status_mmi_cockpit_carplay_test.sh \
             finish_mmi_cockpit_carplay_test.sh altscreen_preload.awk; do
        [ -s "$sd_scripts/$s" ] || { say "FAIL: $s is missing/empty in the SD package"; return 1; }
    done
    esd="$VOLUME/Toolbox/GEM/mqb-carplayAltScreen.esd"
    [ -s "$esd" ] || { say "FAIL: GEM menu is missing/empty in the SD package"; return 1; }
    say "CONTROLLER_COMPANIONS=ROUTER_RUNTIME path=$(p /mnt/app/root/carplay-altscreen/bin) no_eso_write=YES"
    say "GEM_MENU=PACKAGE_OWNED source=$esd no_eso_write=YES"
    return 0
}

# Keep a separate first backup so pre-diagnostics five-file backups remain valid.
# Restore removes only our delimited block, preserving other startup additions.
strip_boot_block() {
    awk '
        $0 == "# BEGIN ALTSCREEN DIAGNOSTICS" { if (inside || seen++) exit 9; inside=1; next }
        $0 == "# END ALTSCREEN DIAGNOSTICS" { if (!inside) exit 9; inside=0; next }
        !inside { print }
        END { if (inside) exit 9 }
    ' "$1"
}
install_boot_diagnostics() (
    rel=$(locate_first "/mnt/system/etc/boot/startup.sh /etc/boot/startup.sh") || return 1
    startup=$(p "$rel")
    sh -n "$startup" || return 1
    if [ ! -f "$BOOT_BACKUP/COMPLETE" ]; then
        ensure_dirs "$BOOT_BACKUP" || return 1
        # Never replace an existing incomplete snapshot.
        [ ! -e "$BOOT_BACKUP/startup.sh" ] || { say "FAIL: incomplete boot backup"; return 1; }
        cp "$startup" "$BOOT_BACKUP/startup.sh" || return 1
        same_bytes "$startup" "$BOOT_BACKUP/startup.sh" || return 1
        cksum < "$BOOT_BACKUP/startup.sh" > "$BOOT_BACKUP/startup.cksum" || return 1
        printf '%s\n' "$rel" > "$BOOT_BACKUP/path" || return 1
        touch "$BOOT_BACKUP/COMPLETE" || return 1
    fi
    [ "$(cat "$BOOT_BACKUP/path")" = "$rel" ] || return 1
    [ "$(cksum < "$BOOT_BACKUP/startup.sh")" = "$(cat "$BOOT_BACKUP/startup.cksum")" ] || return 1
    strip_boot_block "$startup" > "$STATE_DIR/boot.clean" || return 1
    cat > "$STATE_DIR/boot.block" <<'BOOT_BLOCK'
# BEGIN ALTSCREEN DIAGNOSTICS
(
    PATH=${PATH:-/bin:/usr/bin}:/proc/boot:/armle/bin:/armle/scripts:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin:/eso/bin:/eso/bin/apps
    LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}:/proc/boot:/usr/lib:/armle/lib:/armle/lib/dll:/lib:/mnt/app/root/carplay-altscreen/lib:/eso/lib:/mnt/app/usr/lib:/mnt/app/armle/lib:/mnt/app/armle/lib/dll:/mnt/app/armle/usr/lib:/lib/dll
    export PATH LD_LIBRARY_PATH
    ALTS_BOOT_ENTRY=/tmp/MMI-Cockpit-Carplay.boot_entry.log
    if ( : >> "$ALTS_BOOT_ENTRY" ) 2>/dev/null; then
        exec >> "$ALTS_BOOT_ENTRY" 2>&1
    fi
    echo "BOOT_ENTRY pid=$$"
    alts_boot_wait=0
    while [ ! -f /mnt/app/root/carplay-altscreen/bin/altscreen_boot_diag.sh ] && [ "$alts_boot_wait" -lt 60 ]; do
        sleep 2
        alts_boot_wait=$((alts_boot_wait + 1))
    done
    if [ -f /mnt/app/root/carplay-altscreen/bin/altscreen_boot_diag.sh ]; then
        echo "BOOT_HELPER_FOUND wait_steps=$alts_boot_wait"
        /bin/sh /mnt/app/root/carplay-altscreen/bin/altscreen_boot_diag.sh
        echo "BOOT_HELPER_EXIT rc=$?"
    else echo "BOOT_HELPER_MISSING after_seconds=120"; fi
) > /dev/null 2>&1 < /dev/null &
# END ALTSCREEN DIAGNOSTICS
BOOT_BLOCK
    # Place before executable startup statements, including any early exit.
    awk 'FNR==NR {block=block $0 "\n"; next}
         FNR==1 {if ($0 ~ /^#!/) {print; printf "%s",block; next} printf "%s",block}
         {print}' "$STATE_DIR/boot.block" "$STATE_DIR/boot.clean" > "$STATE_DIR/boot.new" || return 1
    sh -n "$STATE_DIR/boot.new" || return 1
    ensure_dirs "$(dirname -- "$BOOT_ENABLED")" || return 1
    touch "$BOOT_ENABLED" || return 1
    stage_and_publish "$STATE_DIR/boot.new" "$startup" 755 || return 1
    rm -f "$STATE_DIR/boot.clean" "$STATE_DIR/boot.block" "$STATE_DIR/boot.new"
    say "BOOT_DIAGNOSTICS_INSTALLED=YES entry=$rel backup=$BOOT_BACKUP"
)
remove_boot_diagnostics() (
    rm -f "$BOOT_ENABLED" || return 1
    [ -f "$BOOT_BACKUP/COMPLETE" ] || return 0
    rel=$(cat "$BOOT_BACKUP/path") || return 1
    case "$rel" in /mnt/system/etc/boot/startup.sh|/etc/boot/startup.sh) ;; *) return 1 ;; esac
    startup=$(p "$rel")
    strip_boot_block "$startup" > "$STATE_DIR/boot.restore" || return 1
    sh -n "$STATE_DIR/boot.restore" || return 1
    stage_and_publish "$STATE_DIR/boot.restore" "$startup" 755 || return 1
    rm -f "$STATE_DIR/boot.restore"
    say "BOOT_DIAGNOSTICS_REMOVED=YES"
)

verify_backup() (
    [ -f "$COMPLETE_MARKER" ] && [ -f "$BACKUP_MANIFEST" ] && [ -f "$BACKUP_DIR/overlay_present.txt" ] && [ -f "$BACKUP_DIR/overlay_dir.txt" ] || return 1
    count=0
    while IFS= read -r rel; do
        case "$rel" in /eso/bin/apps/dio_manager|/mnt/app/eso/bin/apps/dio_manager|/eso/lib/libairplay.so|/armle/usr/lib/libNmeBaseClasses.so|/mnt/app/armle/usr/lib/libNmeBaseClasses.so|/eso/lib/libNmeBaseClasses.so|/mnt/system/etc/eso/production/smartphone_integrator.json|/mnt/system/etc/eso/production/dio_manager.json) ;; *) return 1 ;; esac
        member="$BACKUP_DIR/files/$(echo "$rel" | tr '/' '_')"
        [ -s "$member" ] && [ "$(cksum < "$member")" = "$(cat "$member.cksum")" ] || return 1
        count=$((count + 1))
    done < "$BACKUP_MANIFEST"
    [ "$count" = 5 ] || return 1
    backed_dir=$(cat "$BACKUP_DIR/overlay_dir.txt" 2>/dev/null || true)
    case "$backed_dir" in "$LIVE_LIBTARGET"|"$LEGACY_LIBTARGET") ;; *) return 1 ;; esac
    while IFS= read -r name; do
        case "$name" in libairplay.so|libairplax.so|libNmeBaseClasses.so) ;; *) return 1 ;; esac
        member="$BACKUP_DIR/files/overlay_$name"
        [ -f "$member" ] && [ "$(cksum < "$member")" = "$(cat "$member.cksum")" ] || return 1
    done < "$BACKUP_DIR/overlay_present.txt"
)

finish_mounts() {
    ro_rc=0
    sync || ro_rc=1
    if [ "${MR_APP:-0}" = 1 ]; then mount_ro "$(p /mnt/app)" && MR_APP=0 || ro_rc=1; fi
    if [ "${MR_SYS:-0}" = 1 ]; then mount_ro "$(p /mnt/system)" && MR_SYS=0 || ro_rc=1; fi
    return "$ro_rc"
}

cmd_start() {
    lock_acquire
    if [ "$TESTING" != 1 ] && [ "${ALTSCREEN_INTEGRATED_START:-0}" != 1 ]; then
        say "FAIL: direct controller START is disabled; use start_mmi_cockpit_carplay_rx_test.sh so type111 and the instrument Mirror start as one transaction"
        lock_release
        exit 1
    fi
    if [ "$TESTING" != 1 ]; then
        mirror_runtime="$(p /mnt/app/root/carplay-altscreen/bin/mirror)"
        [ -x "$mirror_runtime/carplay-alt111-mirror-display" ] &&
        [ -x "$mirror_runtime/start_vehicle.sh" ] &&
        [ -f "$mirror_runtime/.mmi-cockpit-carplay-mirror-owner" ] || {
            say "FAIL: integrated Mirror runtime is missing or incomplete; run INSTALL again before START"
            lock_release
            exit 1
        }
    fi
    [ -f "$INSTALLED_MARKER" ] || { say "FAIL: INSTALL must run before START"; lock_release; exit 1; }
    [ -f "$COMPLETE_MARKER" ] || { say "FAIL: backup is not COMPLETE; refusing to arm"; lock_release; exit 1; }
    verify_backup || fail "original backup damaged"
    verify_firewall_backup || fail "original firewall backup damaged or missing; run INSTALL again"
    for lib in libairplay.so libairplax.so libNmeBaseClasses.so; do
        [ -s "$(p "$LIVE_LIBTARGET")/$lib" ] || fail "deployed library missing: $lib"
    done
    if [ -f "$STATE_DIR/ACTIVE" ]; then echo "START=ALREADY_ACTIVE run_id=$(cat "$STATE_DIR/run_id") REBOOT_REQUIRED=YES"; return 0; fi
    RUN_ID="$(date +%Y%m%d_%H%M%S)_$$"
    SESSION="$LOG_ROOT/sessions/$RUN_ID"
    ensure_dirs "$SESSION" "$STATE_DIR" || { say "FAIL: cannot create session/state"; lock_release; exit 1; }
    mount_rw "$(p /mnt/app)" || exit 1; MR_APP=1
    mount_rw "$(p /mnt/system)" || exit 1; MR_SYS=1
    LIVE_DIRTY=1
    printf '%s\n' "$RUN_ID" > "$STATE_DIR/run_id" || exit 1
    printf '%s\n' "$SESSION" > "$STATE_DIR/session_path" || exit 1
    # Owner's corrected profile: current LIVI uses USBHost=16 / WirelessCarPlay=24 /
    # VehicleStatus=21 and carries no ThemeAssets 21/17, so identification is
    # observed only. START must not create ARMED_IAP2.
    printf '%s\n' observe > "$STATE_DIR/IAP2_PROFILE" || exit 1
    rm -f "$STATE_DIR/ARMED_IAP2" || exit 1
    # FULL_CHAIN_MODE enables the private type-111 path and NATIVE_DISPLAY_MODE
    # enables the native instrument route. The route is gated by the first
    # successful real type111 renderer post. Both are required for this build.
    for m in ARMED ARMED_MUTATE ARMED_INFO ARMED_FEATURE ARMED_CREATE111 \
             FULL_CHAIN_MODE NATIVE_DISPLAY_MODE ACTIVE FORCE_START; do
        touch "$STATE_DIR/$m" || { say "FAIL: cannot create $m"; lock_release; exit 1; }
    done
    printf '%s\n' native > "$STATE_DIR/FULL_CHAIN_MODE" 2>/dev/null || true
    printf '%s\n' native > "$STATE_DIR/NATIVE_DISPLAY_MODE" 2>/dev/null || true
    # Probe marker used by the older runtime for compatibility.
    ensure_dirs "$(dirname -- "$PROBE_MARKER")" || exit 1
    printf '%s\n' "$RUN_ID" > "$PROBE_MARKER" || exit 1
    finish_mounts || exit 1
    LIVE_DIRTY=0
    say "START=PASS run_id=$RUN_ID session=$SESSION"
    say "ARMED=ARMED,ARMED_MUTATE,ARMED_INFO,ARMED_FEATURE,ARMED_CREATE111,ACTIVE,FORCE_START"
    say "NATIVE_DISPLAY_MODE=PRESENT native_instrument_display=ENABLED frame_gate=REAL_TYPE111_POST"
    say "IAP2_PROFILE=observe ARMED_IAP2=ABSENT"
    say "REBOOT_REQUIRED=YES"
    lock_release
    return 0
}

cmd_restore() {
    lock_acquire
    verify_backup || fail "original backup unavailable or damaged"
    if [ -f "$FIREWALL_COMPLETE" ]; then
        verify_firewall_backup || fail "original firewall backup unavailable or damaged"
    elif [ -f "$STATE_DIR/FIREWALL_PATCHED" ] ||
         grep -qF "$FIREWALL_BEGIN" "$(p "$LIVE_PF_CONF")" 2>/dev/null; then
        fail "firewall is patched but its original backup is unavailable"
    fi
    if [ ! -f "$COMPLETE_MARKER" ]; then
        say "FAIL: no COMPLETE backup to restore from"
        lock_release
        exit 1
    fi
    # Disarm first so nothing can mutate while we restore.
    rm -f "$STATE_DIR/ARMED" "$STATE_DIR/ARMED_MUTATE" "$STATE_DIR/ARMED_IAP2" \
          "$STATE_DIR/ARMED_INFO" "$STATE_DIR/ARMED_FEATURE" "$STATE_DIR/ARMED_CREATE111" \
          "$STATE_DIR/ACTIVE" "$STATE_DIR/FORCE_START" "$STATE_DIR/FULL_CHAIN_MODE" \
          "$STATE_DIR/NATIVE_DISPLAY_MODE" || exit 1
    mount_rw "$(p /mnt/system)" || { lock_release; exit 1; }; MR_SYS=1
    mount_rw "$(p /mnt/app)" || { lock_release; exit 1; }; MR_APP=1
    cleanup_stale_system_publish_files || { finish_mounts; lock_release; exit 1; }
    restore_originals || { say "FAIL: restore failed"; finish_mounts; lock_release; exit 1; }
    rm -f "$PROBE_MARKER" "$STATE_DIR/.mibcarplay_fullchain_probe" "$STATE_DIR/INSTALLED" || exit 1
    touch "$STATE_DIR/RESTORE_PENDING_REBOOT" || exit 1
    finish_mounts || exit 1
    say "RESTORE=PASS bytes_verified=1 state=RESTORE_PENDING_REBOOT"
    say "NOTE: running processes are not unloaded; a reboot is required before the original code is active"
    finish_mounts
    lock_release
    return 0
}

restore_overlay_baseline() (
    verify_backup || return 1
    backed_dir=$(cat "$BACKUP_DIR/overlay_dir.txt" 2>/dev/null || true)
    case "$backed_dir" in "$LIVE_LIBTARGET"|"$LEGACY_LIBTARGET") ;; *) return 1 ;; esac
    ensure_dirs "$(p "$backed_dir")" "$(p "$LIVE_LIBTARGET")" || return 1
    for n in libairplay.so libairplax.so libNmeBaseClasses.so; do
        backed_dst="$(p "$backed_dir")/$n"
        if grep -q "^$n\$" "$BACKUP_DIR/overlay_present.txt" 2>/dev/null; then
            stage_and_publish "$BACKUP_DIR/files/overlay_$n" "$backed_dst" 755 || return 1
        else
            rm -f "$backed_dst" 2>/dev/null || return 1
        fi
        if [ "$backed_dir" != "$LIVE_LIBTARGET" ]; then rm -f "$(p "$LIVE_LIBTARGET")/$n" 2>/dev/null || return 1; fi
    done
    return 0
)

restore_originals() (
    verify_backup || return 1
    ok=0
    remove_boot_diagnostics || ok=1
    restore_firewall_original || ok=1
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        case "$rel" in /eso/bin/apps/dio_manager|/mnt/app/eso/bin/apps/dio_manager|/eso/lib/libairplay.so|/armle/usr/lib/libNmeBaseClasses.so|/mnt/app/armle/usr/lib/libNmeBaseClasses.so|/eso/lib/libNmeBaseClasses.so|/mnt/system/etc/eso/production/smartphone_integrator.json|/mnt/system/etc/eso/production/dio_manager.json) ;; *) return 1 ;; esac
        key=$(echo "$rel" | tr '/' '_')
        src="$BACKUP_DIR/files/$key"
        dst=$(p "$rel")
        if [ ! -f "$src" ]; then say "FAIL: backup member missing for $rel"; ok=1; continue; fi
        restore_mode=755
        case "$rel" in *.json) restore_mode=644 ;; esac
        stage_and_publish "$src" "$dst" "$restore_mode" || { ok=1; continue; }
        same_bytes "$src" "$dst" || { say "FAIL: restored $rel does not match the backup"; ok=1; }
    done < "$BACKUP_MANIFEST"
    # Overlay baseline may belong to the pre-unification lib-target.
    restore_overlay_baseline || ok=1
    [ "$ok" = 0 ] || return 1
    return 0
)

cmd_status() {
    say "=== AltScreen chain status ==="
    for rel in "$LIVE_DIO_CANDIDATES" "$LIVE_LIBAIRPLAY" "$LIVE_NME_CANDIDATES"; do
        found=$(locate_first "$rel" 2>/dev/null) && say "PRESENT $found" 
    done
    for n in libairplay.so libairplax.so libNmeBaseClasses.so; do
        [ -f "$(p "$LIVE_LIBTARGET")/$n" ] && say "OVERLAY_PRESENT $(p "$LIVE_LIBTARGET")/$n"
    done
    [ -f "$COMPLETE_MARKER" ] && say "BACKUP=COMPLETE" || say "BACKUP=ABSENT"
    [ -f "$FIREWALL_COMPLETE" ] && say "FIREWALL_BACKUP=COMPLETE" || say "FIREWALL_BACKUP=ABSENT"
    say "FIREWALL_TYPE111=RUNTIME_EXACT_PORT persistent_high_port_range=ABSENT"
    [ -f "$INSTALLED_MARKER" ] && say "INSTALLED=YES" || say "INSTALLED=NO"
    for m in ARMED ACTIVE FORCE_START ARMED_IAP2; do
        [ -e "$STATE_DIR/$m" ] && say "MARKER $m=PRESENT" || say "MARKER $m=ABSENT"
    done
    [ -f "$STATE_DIR/IAP2_PROFILE" ] && say "IAP2_PROFILE=$(cat "$STATE_DIR/IAP2_PROFILE")"
    [ -f "$STATE_DIR/firmware_profile.txt" ] && say "FIRMWARE_PROFILE=$(cat "$STATE_DIR/firmware_profile.txt")"
    say "EVIDENCE_LEVEL=file_state; runtime video is not verified by this command"
    return 0
}

cmd_collect() {
    sd_writable || fail "cannot make SD writable"
    ensure_dirs "$STATE_DIR" || fail "cannot create state directory"
    RUN_ID=$(cat "$STATE_DIR/run_id" 2>/dev/null || echo collect_$$)
    [ -n "$RUN_ID" ] || RUN_ID=collect_$$
    SESSION="$LOG_ROOT/sessions/$RUN_ID"
    ensure_dirs "$SESSION" || fail "cannot create collection directory"
    COLLECT_TMP_PREFIX="$(p /tmp)/altscreen_collect_$$"
    REPORT_TMP="$COLLECT_TMP_PREFIX.report"
    collect_details > "$REPORT_TMP" 2>&1
    collect_rc=$?
    cat "$REPORT_TMP"
    REPORT="$SESSION/collection_report.txt.log"
    if ! copy_snapshot "$REPORT_TMP" "$REPORT.new" 2>/dev/null || ! mv "$REPORT.new" "$REPORT"; then
        rm -f "$REPORT.new" "$REPORT_TMP" "$COLLECT_TMP_PREFIX".*
        say "FAIL: collection report could not be persisted to SD"
        return 1
    fi
    rm -f "$REPORT_TMP" "$COLLECT_TMP_PREFIX".*
    say "COLLECTION_REPORT_ENCRYPTED=$REPORT"
    return "$collect_rc"
}

# Save evidence before RESTORE removes markers. Log absence is evidence too.
collect_details() {
    say "COLLECTION_BEGIN volume=$VOLUME session=$SESSION"
    date
    cmd_status
    say "STATE_BEFORE_RESTORE"
    ls -la "$STATE_DIR"
    for lib in libairplay.so libairplax.so libNmeBaseClasses.so; do
        candidate="$(p "$LIVE_LIBTARGET")/$lib"
        if [ -f "$candidate" ]; then cksum "$candidate"; else say "MISSING $candidate"; fi
    done
    for rel in "$LIVE_JSON_SI" "$LIVE_JSON_DIO"; do
        if [ -f "$(p "$rel")" ]; then
            say "FILE_BEGIN $rel"
            cat "$(p "$rel")"
            say "FILE_END $rel"
        else say "MISSING $rel"; fi
    done
    if [ -f "$(p "$LIVE_PF_CONF")" ]; then
        say "FILE_BEGIN $LIVE_PF_CONF"
        cat "$(p "$LIVE_PF_CONF")"
        say "FILE_END $LIVE_PF_CONF"
    else say "MISSING $LIVE_PF_CONF"; fi
    if command -v pfctl >/dev/null 2>&1; then
        collect_bounded "$COLLECT_TMP_PREFIX.pf_rules.txt" pfctl -sr
    elif [ -x "$(p /armle/sbin/pfctl)" ]; then
        collect_bounded "$COLLECT_TMP_PREFIX.pf_rules.txt" "$(p /armle/sbin/pfctl)" -sr
    else
        say "MISSING_COMMAND pfctl; active firewall rules not inspected"
    fi
    print_log() {
        for cand in "$1" "$2" "$3"; do
            [ -n "$cand" ] || continue
            if [ -f "$cand" ]; then
                say "LOG_BEGIN $cand"
                tail -c 1048576 "$cand" 2>/dev/null || { say "WARN: cannot collect $cand"; return 1; }
                say "LOG_END $cand"
                say "COLLECTED $cand"
                return 0
            fi
        done
        say "MISSING $1"
        return 1
    }
    print_log "$(p /tmp/MMI-Cockpit-Carplay/altscreen_hook.log)" \
              "$(p /tmp/MMI-Cockpit-Carplay.altscreen_hook.log)" \
              "$(p /tmp/altscreen_hook.log)" || true
    print_log "$(p /tmp/CinemoDioManager.log)" "" "" || true
    print_log "$(p /tmp/MMI-Cockpit-Carplay/boot_entry.log)" \
              "$(p /tmp/MMI-Cockpit-Carplay.boot_entry.log)" "" || true
    for operation in "$(p /tmp)"/altscreen_operation_*.log; do
        [ -f "$operation" ] || continue
        print_log "$operation" "" "" || true
    done
    if [ -d "$(p /tmp/MMI-Cockpit-Carplay/diagnostics)" ]; then
        for diagnostic in "$(p /tmp/MMI-Cockpit-Carplay/diagnostics)"/boots/*/*.log \
                          "$(p /tmp/MMI-Cockpit-Carplay/diagnostics)"/boots/*/streams/*.log; do
            [ -f "$diagnostic" ] || continue
            print_log "$diagnostic" "" "" || true
        done
    fi
    for rel in /mnt/system/etc/boot/startup.sh /etc/boot/startup.sh; do
        if [ -f "$(p "$rel")" ]; then
            say "FILE_BEGIN $rel"
            cat "$(p "$rel")"
            say "FILE_END $rel"
            break
        fi
    done
    if command -v pidin >/dev/null 2>&1; then
        collect_bounded "$COLLECT_TMP_PREFIX.processes.txt" pidin arguments
        collect_bounded "$COLLECT_TMP_PREFIX.dio_libraries.txt" pidin -p dio_manager libs
        collect_bounded "$COLLECT_TMP_PREFIX.dio_mappings.txt" pidin -p dio_manager mapinfo
        collect_bounded "$COLLECT_TMP_PREFIX.integrator_libraries.txt" pidin -p smartphone_integrator libs
    else
        say "MISSING_COMMAND pidin; runtime loading not inspected"
    fi
    say "COLLECT=BEST_EFFORT session=$SESSION"
    say "MISSING logs do not block restore"
    return 0
}

# Native pidin probes only, each bounded so collection cannot indefinitely block
# the finish wrapper's RESTORE. The monitored PID execs the tool itself.
collect_bounded() (
    output=$1; shift
    (exec "$@") > "$output" 2>&1 &
    probe_pid=$!
    (
        sleep 5
        if kill -0 "$probe_pid" 2>/dev/null; then
            echo "PROBE_TIMEOUT command=$*" >> "$output"
            kill -KILL "$probe_pid" 2>/dev/null || true
        fi
    ) &
    watcher_pid=$!
    wait "$probe_pid"
    probe_rc=$?
    kill "$watcher_pid" 2>/dev/null || true
    wait "$watcher_pid" 2>/dev/null || true
    echo "PROBE_OUTPUT_BEGIN command=$*"
    cat "$output"
    echo "PROBE_OUTPUT_END command=$*"
    rm -f "$output"
    echo "PROBE_RESULT status=$probe_rc output=$output"
    return 0
)

# ------------------------------------------------------------------ dispatch
CMD=${1:-}
REQUESTED_PROFILE=${2:-}
case "$CMD" in
    install) cmd_install ;;
    start)   cmd_start ;;
    status)  cmd_status ;;
    restore) cmd_restore ;;
    collect) cmd_collect ;;
    *) echo "usage: $PROG {install|start|status|restore|collect}" >&2; exit 2 ;;
esac
