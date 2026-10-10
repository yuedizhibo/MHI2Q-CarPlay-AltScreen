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

# MIB2Q AUG22 universal AltScreen controller.
#
# Since 2026-09-17 this is the only active installation/runtime path for supported
# AUG22 vehicles, including K1004 and P1404.  It does not replace dio_manager,
# stock libairplay or Nme.  It installs the standalone dynamic resolver as
# LD_PRELOAD and reuses the vehicle's stock implementation at runtime.
#
# Historical K1004/P1404 profile overlays remain in the repository for reference
# and legacy RESTORE only; this controller never selects or deploys them.
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

PROG=altscreen_chain_test_universal
TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
FIXED_ROOT=""
VOLUME=""
MR_APP=0
MR_SYS=0
LOCK_HELD=0
LIVE_DIRTY=0

say(){ echo "$1"; }
fail(){ echo "FAIL: $1" >&2; exit 1; }
p(){ printf '%s%s\n' "$FIXED_ROOT" "$1"; }

if [ "$TESTING" = 1 ]; then
    FIXED_ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
    case "$FIXED_ROOT" in /tmp/*|/var/tmp/*) ;; *) fail "invalid ALTSCREEN_CHAIN_ROOT" ;; esac
    case "$VOLUME" in /tmp/*|/var/tmp/*) ;; *) fail "invalid ALTSCREEN_CHAIN_VOLUME" ;; esac
    mount_rw(){ :; }
    mount_ro(){ :; }
else
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -d "$candidate/Toolbox" ]; then VOLUME=$candidate; break; fi
    done
    [ -n "$VOLUME" ] || fail "no Toolbox SD card discovered"
    mount_rw(){ mount -uw "$1"; }
    mount_ro(){ mount -ur "$1"; }
fi

SD_RW_HELPER="$VOLUME/Toolbox/scripts/altscreen_sd_writable.sh"
[ -f "$SD_RW_HELPER" ] || fail "SD writable helper missing: $SD_RW_HELPER"
. "$SD_RW_HELPER"

ARTIFACT_DIR="$VOLUME/Toolbox/carplay_alt_screen"
UNIVERSAL_SRC="$ARTIFACT_DIR/universal/libcarplay_altscreen.so"
UNIVERSAL_REL="/mnt/app/root/carplay-altscreen/lib/libcarplay_altscreen.so"
LEGACY_UNIVERSAL_REL="/mnt/app/root/hooks/libcarplay_altscreen.so"
UNIVERSAL_DST="$(p "$UNIVERSAL_REL")"
RGI_META_SRC="$ARTIFACT_DIR/rgi_meta/libcarplay_rgi_meta.so"
RGI_META_REL="/mnt/app/root/carplay-altscreen/lib/libcarplay_rgi_meta.so"
RGI_META_DST="$(p "$RGI_META_REL")"
RGI_CONFIG="$VOLUME/Toolbox/scripts/altscreen_v33_rgi_config.sh"
PRELOAD_AWK="$VOLUME/Toolbox/scripts/altscreen_preload.awk"
LIVE_DIO_CANDIDATES="/eso/bin/apps/dio_manager /mnt/app/eso/bin/apps/dio_manager"
LIVE_NME_CANDIDATES="/armle/usr/lib/libNmeBaseClasses.so /mnt/app/armle/usr/lib/libNmeBaseClasses.so /eso/lib/libNmeBaseClasses.so"
LIVE_LIBAIRPLAY="/eso/lib/libairplay.so"
LIVE_JSON_SI="/mnt/system/etc/eso/production/smartphone_integrator.json"
LIVE_JSON_DIO="/mnt/system/etc/eso/production/dio_manager.json"
LIVE_PF_CONF="/mnt/system/etc/pf.conf"
LIVE_LIBTARGET="/mnt/app/root/carplay-altscreen/lib"
LEGACY_LIBTARGET="/mnt/app/root/lib-target"

SD_ROOT="$VOLUME/MMI-Cockpit-Carplay"
STATE_DIR="$SD_ROOT/state"
LOG_ROOT="$SD_ROOT/logs"
BACKUP_ROOT="$SD_ROOT/backup"
STAGING_ROOT="$SD_ROOT/staging"
BACKUP_DIR="$BACKUP_ROOT/original"
LEGACY_STATE_DIR="$VOLUME/Log/MMI-Cockpit-Carplay/current"
LEGACY_BACKUP_ROOT="$VOLUME/Backup/AltScreenChain"
BACKUP_MANIFEST="$BACKUP_DIR/manifest.txt"
COMPLETE_MARKER="$BACKUP_DIR/COMPLETE"
FIREWALL_BACKUP_DIR="$BACKUP_ROOT/firewall-original"
FIREWALL_BACKUP_FILE="$FIREWALL_BACKUP_DIR/pf.conf"
FIREWALL_COMPLETE="$FIREWALL_BACKUP_DIR/COMPLETE"
UNIVERSAL_BACKUP_DIR="$BACKUP_ROOT/universal-hook-original"
UNIVERSAL_BACKUP_FILE="$UNIVERSAL_BACKUP_DIR/libcarplay_altscreen.so"
UNIVERSAL_BACKUP_COMPLETE="$UNIVERSAL_BACKUP_DIR/COMPLETE"
LOCK_FILE="$STATE_DIR/.chain_test.lock"
LOCK_BOOT_TOKEN_FILE="$(alts_posix_tmp_dir "$(p /tmp)")/altscreen_boot_token"
LOCK_OWNER_TAG="MMI-Cockpit-Carplay-Universal"
INSTALLED_MARKER="$STATE_DIR/INSTALLED"
PROBE_MARKER="$(p /mnt/app/root/carplay-altscreen/state/fullchain_probe)"
TXN_ROOT="$STAGING_ROOT/controller-txn"
TXN_DIR="$TXN_ROOT/universal.$$"
FIREWALL_BEGIN="# BEGIN ALTSCREEN TYPE111 FIREWALL"
FIREWALL_END="# END ALTSCREEN TYPE111 FIREWALL"

locate_first(){
    for cand in $1; do [ -e "$(p "$cand")" ] && { echo "$cand"; return 0; }; done
    return 1
}
same_bytes(){ [ -f "$1" ] && [ -f "$2" ] && cmp -s "$1" "$2" 2>/dev/null; }
nonempty(){ [ -s "$1" ]; }
system_space_snapshot(){
    label=$1
    target="$(p /mnt/system)"
    say "SYSTEM_SPACE_BEGIN label=$label path=$target"
    df -k "$target" 2>/dev/null || df "$target" 2>/dev/null || true
    say "SYSTEM_SPACE_END label=$label"
}
cleanup_txn(){ [ ! -e "$TXN_DIR" ] || rm -rf "$TXN_DIR" 2>/dev/null || true; }
trap cleanup_txn 0
stage_and_publish() (
    src=$1; dst=$2; mode=$3; dir=$(dirname -- "$dst"); base=$(basename -- "$dst")
    tmp="$dir/.$base.new.$$"
    ensure_dirs "$dir" || return 1
    if same_bytes "$src" "$dst"; then
        chmod "$mode" "$dst" 2>/dev/null || return 1
        say "PUBLISH_SKIP_IDENTICAL path=$dst"
        return 0
    fi
    rm -f "$dir/.$base.new."* 2>/dev/null || true
    if cp "$src" "$tmp"; then
        :
    else
        rc=$?
        rm -f "$tmp" 2>/dev/null || true
        case "$dst" in "$(p /mnt/system)/"*) say "SYSTEM_WRITE_FAILED stage=copy target=$dst"; system_space_snapshot publish_copy_failed ;; esac
        return "$rc"
    fi
    chmod "$mode" "$tmp" || { rc=$?; rm -f "$tmp" 2>/dev/null || true; return "$rc"; }
    same_bytes "$src" "$tmp" || { rm -f "$tmp" 2>/dev/null || true; return 1; }
    mv "$tmp" "$dst" || { rc=$?; rm -f "$tmp" 2>/dev/null || true; return "$rc"; }
)

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
    migrate_legacy_dir "$LEGACY_BACKUP_ROOT/universal-hook-original" "$UNIVERSAL_BACKUP_DIR" universal_hook || return 1
    return 0
}

lock_boot_token(){
    if [ -s "$LOCK_BOOT_TOKEN_FILE" ]; then
        cat "$LOCK_BOOT_TOKEN_FILE"
        return 0
    fi
    token="$(date +%Y%m%d_%H%M%S 2>/dev/null || echo boot)_$$"
    token_dir=$(dirname -- "$LOCK_BOOT_TOKEN_FILE")
    tmp="${LOCK_BOOT_TOKEN_FILE}.new.$$"

    # QNX mkdir -p may return EEXIST for an already-existing /tmp. Never call
    # mkdir for an existing token directory; only create it when actually absent.
    if [ ! -d "$token_dir" ]; then
        if ! ensure_dirs "$token_dir" 2>/dev/null; then
            echo "WARN: volatile boot token unavailable; stale-lock recovery is PID-only for this boot" >&2
            printf '%s\n' unavailable
            return 0
        fi
    fi

    token_gate="${LOCK_BOOT_TOKEN_FILE}.init"
    if ensure_dirs "$token_dir" 2>/dev/null && mkdir "$token_gate" 2>/dev/null; then
        if [ ! -s "$LOCK_BOOT_TOKEN_FILE" ]; then
            if ( umask 077; printf '%s\n' "$token" > "$tmp" ) 2>/dev/null; then
                mv "$tmp" "$LOCK_BOOT_TOKEN_FILE" 2>/dev/null || true
            fi
        fi
        rm -f "$tmp" 2>/dev/null || true
        rmdir "$token_gate" 2>/dev/null || true
    else
        # A crashed publisher only disables the boot hint; never remove a gate
        # owned by a competing publisher. PID checks remain mandatory without it.
        token_wait=0
        while [ ! -s "$LOCK_BOOT_TOKEN_FILE" ] && [ "$token_wait" -lt 3 ]; do
            token_wait=$((token_wait + 1)); sleep 1
        done
    fi
    if [ -s "$LOCK_BOOT_TOKEN_FILE" ]; then
        cat "$LOCK_BOOT_TOKEN_FILE"
        return 0
    fi

    echo "WARN: volatile boot token unavailable; stale-lock recovery is PID-only for this boot" >&2
    printf '%s\n' unavailable
    return 0
}
lock_publish_owner(){
    boot=$1
    printf '%s\n' "$LOCK_OWNER_TAG" > "$LOCK_FILE/owner" || return 1
    printf '%s\n' "$$" > "$LOCK_FILE/pid" || return 1
    printf '%s\n' "$boot" > "$LOCK_FILE/boot" || return 1
    printf '%s\n' "${CMD:-unknown}" > "$LOCK_FILE/action" || return 1
    return 0
}
lock_remove_metadata(){
    rm -f "$LOCK_FILE/owner" "$LOCK_FILE/pid" "$LOCK_FILE/boot" "$LOCK_FILE/action" 2>/dev/null || return 1
}
lock_reap_stale(){
    boot=$1
    [ -d "$LOCK_FILE" ] || return 0

    # Legacy builds created an empty directory with no owner metadata. Give a
    # concurrently-starting new controller time to publish its owner before
    # deciding that an empty lock is stale.
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
lock_acquire(){
    altscreen_sd_ensure_writable "$VOLUME" "CONTROLLER_${CMD:-UNKNOWN}" ||
        fail "cannot make Toolbox SD writable"
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
lock_release(){
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
finish_mounts(){
    rc=0; sync || rc=1
    if [ "$MR_APP" = 1 ]; then mount_ro "$(p /mnt/app)" && MR_APP=0 || rc=1; fi
    if [ "$MR_SYS" = 1 ]; then mount_ro "$(p /mnt/system)" && MR_SYS=0 || rc=1; fi
    return "$rc"
}

verify_backup() (
    [ -f "$COMPLETE_MARKER" ] && [ -f "$BACKUP_MANIFEST" ] && [ -f "$BACKUP_DIR/overlay_present.txt" ] && [ -f "$BACKUP_DIR/overlay_dir.txt" ] || return 1
    count=0; seen=" "
    while IFS= read -r rel; do
        case "$rel" in
          /eso/bin/apps/dio_manager|/mnt/app/eso/bin/apps/dio_manager) role=dio ;;
          /eso/lib/libairplay.so) role=airplay ;;
          /armle/usr/lib/libNmeBaseClasses.so|/mnt/app/armle/usr/lib/libNmeBaseClasses.so|/eso/lib/libNmeBaseClasses.so) role=nme ;;
          /mnt/system/etc/eso/production/smartphone_integrator.json) role=si ;;
          /mnt/system/etc/eso/production/dio_manager.json) role=config ;;
          *) return 1 ;;
        esac
        # Exactly one member for each of the five required roles. Counting
        # lines alone could accept duplicates and leave another file unrestored.
        case "$seen" in *" $role "*) return 1 ;; esac
        seen="$seen$role "
        member="$BACKUP_DIR/files/$(echo "$rel" | tr '/' '_')"
        [ -s "$member" ] && [ -f "$member.cksum" ] && [ "$(cksum < "$member")" = "$(cat "$member.cksum")" ] || return 1
        count=$((count+1))
    done < "$BACKUP_MANIFEST"
    [ "$count" = 5 ] || return 1
    backed_dir=$(cat "$BACKUP_DIR/overlay_dir.txt" 2>/dev/null || true)
    case "$backed_dir" in "$LIVE_LIBTARGET"|"$LEGACY_LIBTARGET") ;; *) return 1 ;; esac
    while IFS= read -r name; do
        case "$name" in libairplay.so|libairplax.so|libNmeBaseClasses.so) ;; *) return 1 ;; esac
        member="$BACKUP_DIR/files/overlay_$name"
        [ -f "$member" ] && [ -f "$member.cksum" ] && [ "$(cksum < "$member")" = "$(cat "$member.cksum")" ] || return 1
    done < "$BACKUP_DIR/overlay_present.txt"
)

backup_originals() (
    if [ -f "$COMPLETE_MARKER" ]; then verify_backup || return 1; say "BACKUP=EXISTING kept"; return 0; fi
    live_si=$(p "$LIVE_JSON_SI")
    if grep -Eq 'libcarplay_(altscreen|rgi_meta)\.so' "$live_si" 2>/dev/null ||
       [ -e "$(p /mnt/app/eso/hmi/lsd/jars/carplay_hook.jar)" ]; then
        say "NATIVE_REINSTALL_PRECHECK=FAIL reason=SMARTPHONE_INTEGRATOR_PRELOAD_PRESENT path=$LIVE_JSON_SI token=libcarplay_altscreen.so"
        say "FAIL: trusted original backup is absent but live CarPlay config already references an AltScreen hook; reuse the original SD backup or restore stock first"
        return 1
    fi
    stage="$STAGING_ROOT/universal-original.$$"
    [ ! -e "$BACKUP_DIR" ] || { say "FAIL: incomplete original backup exists"; return 1; }
    ensure_dirs "$stage/files" || return 1
    : > "$stage/manifest.txt"; : > "$stage/overlay_present.txt"
    dio_rel=$(locate_first "$LIVE_DIO_CANDIDATES") || return 1
    nme_rel=$(locate_first "$LIVE_NME_CANDIDATES") || return 1
    for rel in "$dio_rel" "$LIVE_LIBAIRPLAY" "$nme_rel" "$LIVE_JSON_SI" "$LIVE_JSON_DIO"; do
        src=$(p "$rel"); nonempty "$src" || return 1
        dst="$stage/files/$(echo "$rel" | tr '/' '_')"
        cp "$src" "$dst" && same_bytes "$src" "$dst" || return 1
        cksum < "$dst" > "$dst.cksum" || return 1
        echo "$rel" >> "$stage/manifest.txt" || return 1
    done
    overlay_source_dir="$LIVE_LIBTARGET"
    new_overlay_present=0; legacy_overlay_present=0
    for name in libairplay.so libairplax.so libNmeBaseClasses.so; do
        [ ! -f "$(p "$LIVE_LIBTARGET")/$name" ] || new_overlay_present=1
        [ ! -f "$(p "$LEGACY_LIBTARGET")/$name" ] || legacy_overlay_present=1
    done
    if [ "$new_overlay_present" != 1 ] && [ "$legacy_overlay_present" = 1 ]; then
        overlay_source_dir="$LEGACY_LIBTARGET"
    fi
    echo "$overlay_source_dir" > "$stage/overlay_dir.txt"
    for name in libairplay.so libairplax.so libNmeBaseClasses.so; do
        src="$(p "$overlay_source_dir")/$name"
        if [ -f "$src" ]; then
            cp "$src" "$stage/files/overlay_$name" && same_bytes "$src" "$stage/files/overlay_$name" || return 1
            cksum < "$stage/files/overlay_$name" > "$stage/files/overlay_$name.cksum" || return 1
            echo "$name" >> "$stage/overlay_present.txt" || return 1
        fi
    done
    ensure_dirs "$(dirname -- "$BACKUP_DIR")" || return 1
    mv "$stage" "$BACKUP_DIR" || return 1
    touch "$COMPLETE_MARKER" || return 1
    verify_backup || return 1
    say "BACKUP=COMPLETE dir=$BACKUP_DIR"
)

verify_firewall_backup() (
    [ -f "$FIREWALL_COMPLETE" ] && [ -s "$FIREWALL_BACKUP_FILE" ] && [ -f "$FIREWALL_BACKUP_FILE.cksum" ] || return 1
    [ "$(cksum < "$FIREWALL_BACKUP_FILE")" = "$(cat "$FIREWALL_BACKUP_FILE.cksum")" ]
)
backup_firewall() (
    if [ -f "$FIREWALL_COMPLETE" ]; then verify_firewall_backup; return $?; fi
    [ ! -e "$FIREWALL_BACKUP_DIR" ] || return 1
    tmp="${FIREWALL_BACKUP_DIR}.new.$$"; ensure_dirs "$tmp" || return 1
    cp "$(p "$LIVE_PF_CONF")" "$tmp/pf.conf" && same_bytes "$(p "$LIVE_PF_CONF")" "$tmp/pf.conf" || return 1
    cksum < "$tmp/pf.conf" > "$tmp/pf.conf.cksum" || return 1
    echo "$LIVE_PF_CONF" > "$tmp/path" || return 1
    mv "$tmp" "$FIREWALL_BACKUP_DIR" || return 1
    touch "$FIREWALL_COMPLETE" || return 1
    verify_firewall_backup
)

verify_universal_backup() (
    [ -f "$UNIVERSAL_BACKUP_COMPLETE" ] && [ -f "$UNIVERSAL_BACKUP_DIR/present" ] || return 1
    hook_path=$(cat "$UNIVERSAL_BACKUP_DIR/path" 2>/dev/null || echo "$LEGACY_UNIVERSAL_REL")
    case "$hook_path" in "$UNIVERSAL_REL"|"$LEGACY_UNIVERSAL_REL") ;; *) return 1 ;; esac
    present=$(cat "$UNIVERSAL_BACKUP_DIR/present")
    case "$present" in
      0) [ ! -e "$UNIVERSAL_BACKUP_FILE" ] ;;
      1) [ -s "$UNIVERSAL_BACKUP_FILE" ] && [ -f "$UNIVERSAL_BACKUP_FILE.cksum" ] &&
         [ "$(cksum < "$UNIVERSAL_BACKUP_FILE")" = "$(cat "$UNIVERSAL_BACKUP_FILE.cksum")" ] ;;
      *) return 1 ;;
    esac
)
backup_universal_hook() (
    if [ -f "$UNIVERSAL_BACKUP_COMPLETE" ]; then verify_universal_backup; return $?; fi
    [ ! -e "$UNIVERSAL_BACKUP_DIR" ] || return 1
    tmp="${UNIVERSAL_BACKUP_DIR}.new.$$"; ensure_dirs "$tmp" || return 1
    echo "$UNIVERSAL_REL" > "$tmp/path" || return 1
    if [ -f "$UNIVERSAL_DST" ]; then
        echo 1 > "$tmp/present"
        cp "$UNIVERSAL_DST" "$tmp/libcarplay_altscreen.so" && same_bytes "$UNIVERSAL_DST" "$tmp/libcarplay_altscreen.so" || return 1
        cksum < "$tmp/libcarplay_altscreen.so" > "$tmp/libcarplay_altscreen.so.cksum" || return 1
    else
        echo 0 > "$tmp/present"
    fi
    mv "$tmp" "$UNIVERSAL_BACKUP_DIR" || return 1
    touch "$UNIVERSAL_BACKUP_COMPLETE" || return 1
    verify_universal_backup
)

strip_firewall_block(){
    awk -v begin="$FIREWALL_BEGIN" -v end="$FIREWALL_END" '
      $0==begin {if(inside||seen++)exit 9;inside=1;next}
      $0==end {if(!inside)exit 9;inside=0;next}
      !inside {print}
      END{if(inside)exit 9}
    ' "$1"
}
remove_legacy_firewall_rule() (
    live=$(p "$LIVE_PF_CONF")
    ensure_dirs "$TXN_DIR" || return 1
    clean="$TXN_DIR/pf.clean"
    strip_firewall_block "$live" > "$clean" || return 1
    if ! same_bytes "$clean" "$live"; then
        stage_and_publish "$clean" "$live" 644 || { rm -f "$clean"; return 1; }
    fi
    rm -f "$clean" "$STATE_DIR/FIREWALL_PATCHED"
    say "FIREWALL=TYPE111_RUNTIME_EXACT_PORT interface=carplay0 persistent_high_port_range=ABSENT"
)
restore_firewall() (
    [ -f "$FIREWALL_COMPLETE" ] || return 0
    verify_firewall_backup || return 1
    stage_and_publish "$FIREWALL_BACKUP_FILE" "$(p "$LIVE_PF_CONF")" 644 || return 1
    same_bytes "$FIREWALL_BACKUP_FILE" "$(p "$LIVE_PF_CONF")" || return 1
    rm -f "$STATE_DIR/FIREWALL_PATCHED"
)

restore_overlay_baseline() (
    verify_backup || return 1
    backed_dir=$(cat "$BACKUP_DIR/overlay_dir.txt" 2>/dev/null || true)
    case "$backed_dir" in "$LIVE_LIBTARGET"|"$LEGACY_LIBTARGET") ;; *) return 1 ;; esac
    # Do not synthesize runtime/lib merely to restore an originally-absent
    # overlay. stage_and_publish creates parents only when a real file exists.
    for name in libairplay.so libairplax.so libNmeBaseClasses.so; do
        dst="$(p "$backed_dir")/$name"
        if grep -q "^$name\$" "$BACKUP_DIR/overlay_present.txt" 2>/dev/null; then
            stage_and_publish "$BACKUP_DIR/files/overlay_$name" "$dst" 755 || return 1
        else
            [ ! -e "$dst" ] || rm -f "$dst" || return 1
        fi
        if [ "$backed_dir" != "$LIVE_LIBTARGET" ]; then
            live_dst="$(p "$LIVE_LIBTARGET")/$name"
            [ ! -e "$live_dst" ] || rm -f "$live_dst" || return 1
        fi
    done
    [ -s "$BACKUP_DIR/overlay_present.txt" ] ||
        say "OVERLAY_BASELINE=ABSENT no_runtime_dir_synthesis=YES"
)

restore_universal_hook() (
    verify_universal_backup || return 1
    present=$(cat "$UNIVERSAL_BACKUP_DIR/present")
    hook_rel=$(cat "$UNIVERSAL_BACKUP_DIR/path" 2>/dev/null || echo "$LEGACY_UNIVERSAL_REL")
    case "$hook_rel" in "$UNIVERSAL_REL"|"$LEGACY_UNIVERSAL_REL") ;; *) return 1 ;; esac
    hook_dst=$(p "$hook_rel")
    if [ "$present" = 1 ]; then
        stage_and_publish "$UNIVERSAL_BACKUP_FILE" "$hook_dst" 755 || return 1
    else
        rm -f "$hook_dst" || return 1
    fi
    [ "$hook_rel" = "$UNIVERSAL_REL" ] || rm -f "$UNIVERSAL_DST" 2>/dev/null || return 1
)
restore_originals() (
    verify_backup || return 1
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        src="$BACKUP_DIR/files/$(echo "$rel" | tr '/' '_')"; dst=$(p "$rel"); mode=755
        case "$rel" in *.json) mode=644 ;; esac
        stage_and_publish "$src" "$dst" "$mode" || return 1
        same_bytes "$src" "$dst" || return 1
    done < "$BACKUP_MANIFEST"
    restore_overlay_baseline || return 1
    restore_universal_hook || return 1
    # V3.3 metadata transport lives at a project-owned unique path.  The
    # original smartphone_integrator/dio_manager files are restored above.
    rm -f "$RGI_META_DST" 2>/dev/null || return 1
    restore_firewall || return 1
)

check_sources(){
    ensure_dirs "$STATE_DIR" || return 1
    nonempty "$UNIVERSAL_SRC" || { say "FAIL: universal hook missing: $UNIVERSAL_SRC"; return 1; }
    nonempty "$RGI_META_SRC" || { say "FAIL: V3.3 RGI metadata hook missing: $RGI_META_SRC"; return 1; }
    nonempty "$RGI_CONFIG" || { say "FAIL: V3.3 RGI config helper missing: $RGI_CONFIG"; return 1; }
    nonempty "$PRELOAD_AWK" || { say "FAIL: altscreen_preload.awk missing"; return 1; }
    dio_rel=$(locate_first "$LIVE_DIO_CANDIDATES") || return 1
    nme_rel=$(locate_first "$LIVE_NME_CANDIDATES") || return 1
    for rel in "$dio_rel" "$nme_rel" "$LIVE_LIBAIRPLAY" "$LIVE_JSON_SI" "$LIVE_JSON_DIO" "$LIVE_PF_CONF"; do
        nonempty "$(p "$rel")" || { say "FAIL: required source missing: $rel"; return 1; }
    done
    awk -v validate=1 -f "$PRELOAD_AWK" "$(p "$LIVE_JSON_SI")" >/dev/null || return 1
    return 0
}

cmd_install(){
    rgi_mode=${ALTS_INSTALL_RGI_MODE:-WITH}
    case "$rgi_mode" in NO|WITH) ;; *) say "FAIL: invalid RGI install mode"; return 1 ;; esac
    lock_acquire
    ensure_dirs "$TXN_DIR" || { lock_release; return 1; }
    system_space_snapshot install_begin
    check_sources || { lock_release; return 1; }
    backup_originals || { say "FAIL: original backup failed"; lock_release; return 1; }
    backup_firewall || { say "FAIL: firewall backup failed"; lock_release; return 1; }
    backup_universal_hook || { say "FAIL: universal hook backup failed"; lock_release; return 1; }
    mount_rw "$(p /mnt/app)" || { lock_release; return 1; }; MR_APP=1
    mount_rw "$(p /mnt/system)" || { finish_mounts; lock_release; return 1; }; MR_SYS=1
    LIVE_DIRTY=1
    ensure_dirs "$(dirname -- "$UNIVERSAL_DST")" "$(p "$LIVE_LIBTARGET")" "$(dirname -- "$PROBE_MARKER")" || goto_fail=1
    if [ "${goto_fail:-0}" != 1 ]; then restore_overlay_baseline || goto_fail=1; fi
    if [ "${goto_fail:-0}" != 1 ]; then stage_and_publish "$UNIVERSAL_SRC" "$UNIVERSAL_DST" 755 || goto_fail=1; fi
    if [ "${goto_fail:-0}" != 1 ]; then stage_and_publish "$RGI_META_SRC" "$RGI_META_DST" 755 || goto_fail=1; fi
    if [ "${goto_fail:-0}" != 1 ]; then
        # Normalize both project preloads idempotently.  RGI first, then
        # AltScreen, so the final stable order is AltScreen:RGI:other-stock.
        cfg_rgi="$TXN_DIR/rgi-preload-config"
        cfg="$TXN_DIR/universal-config"
        rgi_preload="$RGI_META_REL"
        [ "$rgi_mode" != NO ] || rgi_preload=""
        awk -v hook="$rgi_preload" -v exclude2="$RGI_META_REL" -v allow_absent=1 \
            -v exclude=/mnt/app/root/hooks/libcarplay_hook.so \
            -v insert_if_absent=1 -f "$PRELOAD_AWK" "$(p "$LIVE_JSON_SI")" > "$cfg_rgi" || goto_fail=1
        if [ "${goto_fail:-0}" != 1 ]; then
            awk -v hook="$UNIVERSAL_REL" \
                -v exclude=/mnt/app/root/hooks/libcarplay_hook.so \
                -v exclude2=/mnt/app/root/hooks/libcp_mirror.so -v exclude_prefix=/mnt/app/root/hooks/libcarplay_altscreen.so \
                -v insert_if_absent=1 -f "$PRELOAD_AWK" "$cfg_rgi" > "$cfg" || goto_fail=1
        fi
        if [ "${goto_fail:-0}" != 1 ]; then
            stage_and_publish "$cfg" "$(p "$LIVE_JSON_SI")" 644 || goto_fail=1
        fi
        rm -f "$cfg_rgi" "$cfg"
    fi
    if [ "${goto_fail:-0}" != 1 ] && [ "$rgi_mode" = WITH ]; then
        /bin/sh "$RGI_CONFIG" apply "$(p "$LIVE_JSON_DIO")" || goto_fail=1
    fi
    if [ "${goto_fail:-0}" != 1 ]; then
        awk -v query="$UNIVERSAL_REL" -f "$PRELOAD_AWK" "$(p "$LIVE_JSON_SI")" >/dev/null || goto_fail=1
        if [ "$rgi_mode" = WITH ]; then
            awk -v query="$RGI_META_REL" -f "$PRELOAD_AWK" "$(p "$LIVE_JSON_SI")" >/dev/null || goto_fail=1
            /bin/sh "$RGI_CONFIG" verify "$(p "$LIVE_JSON_DIO")" >/dev/null || goto_fail=1
        elif awk -v query="$RGI_META_REL" -f "$PRELOAD_AWK" "$(p "$LIVE_JSON_SI")" >/dev/null; then
            goto_fail=1
        fi
    fi
    if [ "${goto_fail:-0}" != 1 ]; then remove_legacy_firewall_rule || goto_fail=1; fi
    if [ "${goto_fail:-0}" = 1 ]; then
        say "FAIL: universal deployment failed; restoring originals"
        restore_originals || say "WARN: rollback restore failed"
        finish_mounts || true; LIVE_DIRTY=0; lock_release; return 1
    fi
    touch "$INSTALLED_MARKER" || { restore_originals; finish_mounts || true; LIVE_DIRTY=0; lock_release; return 1; }
    echo UNIVERSAL > "$STATE_DIR/firmware_profile.txt" || { restore_originals; finish_mounts || true; LIVE_DIRTY=0; lock_release; return 1; }
    rm -f "$STATE_DIR/ARMED" "$STATE_DIR/ARMED_MUTATE" "$STATE_DIR/ARMED_IAP2" \
          "$STATE_DIR/ARMED_INFO" "$STATE_DIR/ARMED_FEATURE" "$STATE_DIR/ARMED_CREATE111" \
          "$STATE_DIR/ACTIVE" "$STATE_DIR/FORCE_START" "$STATE_DIR/FULL_CHAIN_MODE" "$STATE_DIR/NATIVE_DISPLAY_MODE" \
          "$STATE_DIR/RESTORE_PENDING_REBOOT"
    finish_mounts || { lock_release; return 1; }
    LIVE_DIRTY=0
    system_space_snapshot install_end
    cleanup_txn
    say "FIRMWARE_PROFILE=UNIVERSAL source=aug22_unified_policy stock_reuse=YES"
    say "UNIVERSAL_PRELOAD=INSTALLED path=$UNIVERSAL_REL resolver=ELF_DYNAMIC_RELOCATION"
    say "INSTALL_RGI_MODE=$rgi_mode native_preload=$rgi_preload reboot_required=YES"
    lock_release || return 1
    say "INSTALL=PASS reboot_required=YES"
}

cmd_start(){
    lock_acquire
    if [ "$TESTING" != 1 ] && [ "${ALTSCREEN_INTEGRATED_START:-0}" != 1 ]; then
        say "FAIL: direct controller START is disabled; use start_mmi_cockpit_carplay_rx_test.sh so native negotiation and stream-driven display start as one transaction"
        lock_release
        return 1
    fi
    [ -f "$INSTALLED_MARKER" ] || { say "FAIL: INSTALL must run before START"; lock_release; return 1; }
    verify_backup || { say "FAIL: original backup damaged"; lock_release; return 1; }
    verify_firewall_backup || { say "FAIL: firewall backup damaged"; lock_release; return 1; }
    [ -s "$UNIVERSAL_DST" ] || { say "FAIL: universal preload missing"; lock_release; return 1; }
    awk -v query="$UNIVERSAL_REL" -f "$PRELOAD_AWK" "$(p "$LIVE_JSON_SI")" >/dev/null || {
        say "FAIL: universal preload is not armed in carplay env"; lock_release; return 1; }

    run_id="$(date +%Y%m%d_%H%M%S)_$"; session="$LOG_ROOT/sessions/$run_id"
    ensure_dirs "$session" "$STATE_DIR" || { lock_release; return 1; }
    echo "$run_id" > "$STATE_DIR/run_id"; echo "$session" > "$STATE_DIR/session_path"

    # V3.5 production policy: preload installation is the persistent enable;
    # contract. SD markers remain diagnostics/rollback metadata only and may not
    # gate a CarPlay session. Remove legacy arming files so an old card cannot
    # accidentally reintroduce V3.3 authorization semantics.
    rm -f "$STATE_DIR/ARMED" "$STATE_DIR/ARMED_MUTATE" "$STATE_DIR/ARMED_IAP2" \
          "$STATE_DIR/ARMED_INFO" "$STATE_DIR/ARMED_FEATURE" "$STATE_DIR/ARMED_CREATE111" \
          "$STATE_DIR/FORCE_START" "$STATE_DIR/FULL_CHAIN_MODE" "$STATE_DIR/NATIVE_DISPLAY_MODE"
    touch "$STATE_DIR/ACTIVE" || { lock_release; return 1; }

    say "AUTH_PRIVATE111_CORE=V35_EARLY_PROTOCOL_READY authority=installed_preload sd_runtime_gate=DISABLED geometry_gate=ASYNC"
    say "NEGOTIATION_POLICY=ONE_CARPLAY_SESSION automatic_main110_then_private111 no_display_gate=YES early_capability_wait=BOUNDED"
    say "DISPLAY_PATH=PRIVATE111_DIRECT source=ScreenStreamProcessData h264_shm=/carplay111_h264 decoder_backend=stock_omx_screen_linearized_shm decoded_shm=/carplay111_decoded sink=displayable3_gles context_owner=JAVA80 window58_readback=0"
    say "IAP2_THEMEASSETS_MUTATION=DISABLED policy=V33_PROVEN_PRIVATE111_WITHOUT_THEMEASSETS"
    lock_release || return 1
    say "START=PASS profile=UNIVERSAL run_id=$run_id reboot_required=YES"
}

cmd_restore_precheck(){
    lock_acquire
    rc=0
    verify_backup || { say "FAIL: original backup unavailable or damaged"; rc=1; }
    if [ "$rc" = 0 ]; then
        verify_universal_backup || { say "FAIL: universal hook backup unavailable or damaged"; rc=1; }
    fi
    if [ "$rc" = 0 ]; then
        verify_firewall_backup || { say "FAIL: firewall original backup unavailable or damaged"; rc=1; }
    fi
    if [ "$rc" = 0 ]; then
        say "RESTORE_PRECHECK=PASS profile=UNIVERSAL production_changed=NO"
    else
        say "RESTORE_PRECHECK=FAIL profile=UNIVERSAL production_changed=NO"
    fi
    lock_release || rc=1
    return "$rc"
}

cmd_restore(){
    lock_acquire
    ensure_dirs "$TXN_DIR" || { lock_release; return 1; }
    system_space_snapshot restore_begin
    verify_backup || { say "FAIL: original backup unavailable or damaged"; lock_release; return 1; }
    verify_universal_backup || { say "FAIL: universal hook backup unavailable or damaged"; lock_release; return 1; }
    rm -f "$STATE_DIR/ARMED" "$STATE_DIR/ARMED_MUTATE" "$STATE_DIR/ARMED_IAP2" \
          "$STATE_DIR/ARMED_INFO" "$STATE_DIR/ARMED_FEATURE" "$STATE_DIR/ARMED_CREATE111" \
          "$STATE_DIR/ACTIVE" "$STATE_DIR/FORCE_START" "$STATE_DIR/FULL_CHAIN_MODE" "$STATE_DIR/NATIVE_DISPLAY_MODE"
    mount_rw "$(p /mnt/app)" || { lock_release; return 1; }; MR_APP=1
    mount_rw "$(p /mnt/system)" || { finish_mounts; lock_release; return 1; }; MR_SYS=1
    restore_originals || { say "FAIL: restore failed"; finish_mounts || true; lock_release; return 1; }
    rm -f "$PROBE_MARKER" "$INSTALLED_MARKER"
    touch "$STATE_DIR/RESTORE_PENDING_REBOOT"
    finish_mounts || { lock_release; return 1; }
    system_space_snapshot restore_end
    cleanup_txn
    lock_release || return 1
    say "RESTORE=PASS profile=UNIVERSAL bytes_verified=1 reboot_required=YES"
}

cmd_status(){
    echo "=== AltScreen AUG22 universal status ==="
    if [ -d "$LOCK_FILE" ]; then
        echo "OPERATION_LOCK=PRESENT pid=$(cat "$LOCK_FILE/pid" 2>/dev/null || echo unknown)"
    else
        echo "OPERATION_LOCK=ABSENT"
    fi
    [ -f "$INSTALLED_MARKER" ] && echo "INSTALLED=YES" || echo "INSTALLED=NO"
    [ -s "$UNIVERSAL_DST" ] && echo "UNIVERSAL_PRELOAD=PRESENT $UNIVERSAL_REL" || echo "UNIVERSAL_PRELOAD=ABSENT"
    if awk -v query="$UNIVERSAL_REL" -f "$PRELOAD_AWK" "$(p "$LIVE_JSON_SI")" >/dev/null 2>&1; then
        echo "UNIVERSAL_PRELOAD_CONFIG=ARMED"
    else
        echo "UNIVERSAL_PRELOAD_CONFIG=NOT_ARMED"
    fi
    for m in ACTIVE ARMED FORCE_START ARMED_IAP2; do [ -e "$STATE_DIR/$m" ] && echo "MARKER $m=PRESENT" || echo "MARKER $m=ABSENT"; done
    echo "NEGOTIATION_POLICY=V35_EARLY_PROTOCOL_READY sd_runtime_gate=DISABLED geometry_gate=ASYNC"
    echo "DISPLAY_START_POLICY=STREAM_DRIVEN stable_decoded_frames=2 fixed_delay=NONE"
    echo "FIRMWARE_PROFILE=UNIVERSAL"
    echo "RESOLVER_POLICY=dynamic_symbols_plus_ELF_relocations fail_open=YES"
    echo "FIREWALL_TYPE111=RUNTIME_EXACT_PORT persistent_high_port_range=ABSENT"
}

cmd_collect(){
    cmd_status
    for candidate in \
        "$(p /tmp/altscreen_hook.log)" \
        "$(p /tmp/MMI-Cockpit-Carplay/altscreen_hook.log)" \
        "$(p /tmp/MMI-Cockpit-Carplay.altscreen_hook.log)" \
        "$(p /tmp/CinemoDioManager.log)" \
        "$(p /tmp/altscreen_boot_entry.log)" \
        "$(p /tmp/MMI-Cockpit-Carplay/boot_entry.log)" \
        "$(p /tmp/MMI-Cockpit-Carplay.boot_entry.log)" \
        "$(p /tmp/MMI-Cockpit-Carplay/mirror/autostart.log)" \
        "$(p /tmp/mmi-mirror-controller.log)" \
        "$(p /tmp/mmi-mirror-controller.started)" \
        "$(p /tmp/mmi-mirror-active)" \
        "$(p /tmp/mmi-mirror-basevideo.ready)"; do
        if [ -f "$candidate" ]; then
            echo "LOG_BEGIN $candidate"; tail -c 1048576 "$candidate" 2>/dev/null || true; echo "LOG_END $candidate"
        fi
    done
    if command -v pidin >/dev/null 2>&1; then
        pidin -p dio_manager libs 2>/dev/null || true
        pidin -p dio_manager mapinfo 2>/dev/null || true
    fi
    echo "COLLECT=BEST_EFFORT profile=UNIVERSAL"
}

CMD=${1:-}
case "$CMD" in
  install) cmd_install ;;
  start) cmd_start ;;
  status) cmd_status ;;
  restore-precheck) cmd_restore_precheck ;;
  restore) cmd_restore ;;
  collect) cmd_collect ;;
  *) echo "usage: $PROG {install|start|status|restore-precheck|restore|collect}" >&2; exit 2 ;;
esac
