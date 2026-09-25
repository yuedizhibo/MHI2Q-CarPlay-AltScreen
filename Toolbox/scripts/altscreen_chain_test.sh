#!/bin/sh
# AltScreen controller router.
#
# Active installation policy (2026-09-17): UNIVERSAL ONLY.
#   1. every supported AUG22 installation routes to the standalone universal
#      LD_PRELOAD controller;
#   2. K1004/P1404 profile artifacts and the known controller remain in the
#      repository only as historical/reference material;
#   3. the known controller may still be invoked for RESTORE only when a vehicle
#      was installed by an older profile-based package;
#   4. anything outside the supported AUG22 train is refused before mutation.
#
# Direct-display integrates the proven MMI displayable3 GLES backend as a SHM sidecar.
# It is staged transactionally with the AUG22 runtime; Java/HMI remains the sole
# terminal/context owner.
#
# Companion scripts are staged only below /mnt/app/root/carplay-altscreen.  The
# installer never writes /eso/hmi/engdefs/scripts/mqb, because that mount is not
# consistently writable across AUG22 vehicles.
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


TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
FIXED_ROOT=""
VOLUME=""
BASE="$0"
RESOLVED=$(command -v -- "$BASE" 2>/dev/null)
[ -n "$RESOLVED" ] || RESOLVED="$BASE"
SCRIPTDIR=$(CDPATH='' cd -P -- "$(dirname -- "$RESOLVED")" 2>/dev/null && pwd -P)
[ -n "$SCRIPTDIR" ] || { echo "FAIL: cannot resolve controller directory" >&2; exit 126; }
KNOWN="$SCRIPTDIR/altscreen_chain_test_known.sh"
UNIVERSAL="$SCRIPTDIR/altscreen_chain_test_universal.sh"

fail(){ echo "FAIL: $1" >&2; exit 1; }
p(){ printf '%s%s\n' "$FIXED_ROOT" "$1"; }

if [ "$TESTING" = 1 ]; then
    FIXED_ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
    case "$FIXED_ROOT" in /tmp/*|/var/tmp/*) ;; *) fail "invalid ALTSCREEN_CHAIN_ROOT" ;; esac
    case "$VOLUME" in /tmp/*|/var/tmp/*) ;; *) fail "invalid ALTSCREEN_CHAIN_VOLUME" ;; esac
else
    action=${1:-}
    VOLUME=${ALTSCREEN_SD_VOLUME:-}
    if [ -n "$VOLUME" ]; then
        [ -f "$VOLUME/Toolbox/scripts/altscreen_chain_test_universal.sh" ] || fail "selected SD card has no AltScreen controller"
    fi
    case "$action:$VOLUME" in
      restore:|start:|status:|collect:|disarm:)
        for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
            if [ -f "$candidate/MMI-Cockpit-Carplay/state/firmware_profile.txt" ] &&
               [ -f "$candidate/Toolbox/scripts/altscreen_chain_test_universal.sh" ]; then
                VOLUME=$candidate; break
            fi
        done
        ;;
    esac
    if [ -z "$VOLUME" ]; then
        for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
            if [ -f "$candidate/Toolbox/scripts/altscreen_chain_test_universal.sh" ] &&
               [ -s "$candidate/Toolbox/carplay_alt_screen/universal/libcarplay_altscreen.so" ]; then
                VOLUME=$candidate; break
            fi
        done
    fi
    [ -n "$VOLUME" ] || fail "no matching AltScreen SD card discovered"
fi

SD_ROOT="$VOLUME/MMI-Cockpit-Carplay"
STATE_DIR="$SD_ROOT/state"
LOG_ROOT="$SD_ROOT/logs"
BACKUP_ROOT="$SD_ROOT/backup"
STAGING_ROOT="$SD_ROOT/staging"
LEGACY_STATE_DIR="$VOLUME/Log/MMI-Cockpit-Carplay/current"
ROUTE_FILE="$STATE_DIR/firmware_profile.txt"
INSTALLED_MARKER="$STATE_DIR/INSTALLED"
LEGACY_ROUTE_FILE="$LEGACY_STATE_DIR/firmware_profile.txt"
LEGACY_INSTALLED_MARKER="$LEGACY_STATE_DIR/INSTALLED"
ARTIFACT_DIR="$VOLUME/Toolbox/carplay_alt_screen"
SD_SCRIPTS="$VOLUME/Toolbox/scripts"
MIRROR_SD="$ARTIFACT_DIR/mirror_display/release"
MIRROR_OWNER=".mmi-cockpit-carplay-mirror-owner"
PERSIST_DIAG_SD="$SD_SCRIPTS/altscreen_persistent_diag.sh"
LIVE_DIO_CANDIDATES="/eso/bin/apps/dio_manager /mnt/app/eso/bin/apps/dio_manager"
LIVE_LIBAIRPLAY="/eso/lib/libairplay.so"

RUNTIME_ROOT="$(p /mnt/app/root/carplay-altscreen)"
RUNTIME_BIN="$RUNTIME_ROOT/bin"
RUNTIME_STAGE="$(p /mnt/app/root/.carplay-altscreen.new.$$)"
RUNTIME_PREV="$(p /mnt/app/root/.carplay-altscreen.previous)"
RUNTIME_OWNER=.mmi-cockpit-carplay-runtime-owner
RUNTIME_PENDING="$RUNTIME_ROOT/state/transaction.pending"
RUNTIME_PUBLISHED=0
RUNTIME_HAD_CURRENT=0
RUNTIME_SCRIPTS="altscreen_chain_test.sh altscreen_chain_test_known.sh altscreen_chain_test_universal.sh altscreen_persistent_diag.sh altscreen_adaptive_diag.sh altscreen_boot_diag.sh altscreen_live_diag.sh altscreen_preload.awk install_mmi_cockpit_carplay_rx.sh start_mmi_cockpit_carplay_test.sh start_mmi_cockpit_carplay_rx_test.sh force_start_mmi_cockpit_carplay_rx_test.sh stop_mmi_cockpit_carplay_test.sh status_mmi_cockpit_carplay_test.sh finish_mmi_cockpit_carplay_test.sh"

mount_app_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/app; }
mount_app_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/app; }

mark_runtime_pending(){
    [ -d "$RUNTIME_ROOT" ] || return 0
    [ -f "$RUNTIME_ROOT/$RUNTIME_OWNER" ] || return 1
    mount_app_rw || return 1
    ensure_dirs "$RUNTIME_ROOT/state" && touch "$RUNTIME_PENDING" || {
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    }
    sync >/dev/null 2>&1 || true
    mount_app_ro
}

clear_runtime_pending(){
    [ -f "$RUNTIME_ROOT/$RUNTIME_OWNER" ] || return 1
    mount_app_rw || return 1
    rm -f "$RUNTIME_PENDING" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
    sync >/dev/null 2>&1 || true
    mount_app_ro
}

cleanup_stale_runtime_stages(){
    parent="$(p /mnt/app/root)"
    for stale in "$parent"/.carplay-altscreen.new.*; do
        [ -e "$stale" ] || [ -L "$stale" ] || continue
        suffix=${stale##*.}
        case "$suffix" in ''|*[!0-9]*) continue ;; esac
        kill -0 "$suffix" 2>/dev/null && continue
        [ -d "$stale" ] && [ ! -L "$stale" ] || return 1
        rm -rf "$stale" || return 1
    done
}

locate_first(){
    for cand in $1; do [ -e "$(p "$cand")" ] && { echo "$cand"; return 0; }; done
    return 1
}

read_aug22_train(){
    for rel in /net/rcc/dev/shmem/version.txt /dev/shmem/version.txt /net/mmx/dev/shmem/version.txt; do
        path=$(p "$rel")
        [ -r "$path" ] || continue
        train=$(sed -n '/Current train/p' "$path" | head -n 1)
        [ -n "$train" ] || continue
        echo "$train"
        return 0
    done
    return 1
}

select_install_route(){
    requested=${1:-}

    # Test harness compatibility: legacy profile names are accepted only as
    # aliases for UNIVERSAL so tests cannot accidentally re-enable known install.
    if [ "$TESTING" = 1 ] && [ -n "${ALTSCREEN_TEST_FORCE_PROFILE:-}" ]; then
        case "$ALTSCREEN_TEST_FORCE_PROFILE" in
          UNIVERSAL) echo UNIVERSAL; return 0 ;;
          K1004|P1404)
            echo "LEGACY_PROFILE_FORCE_IGNORED requested=$ALTSCREEN_TEST_FORCE_PROFILE route=UNIVERSAL" >&2
            echo UNIVERSAL
            return 0
            ;;
          LEGACY_FLAT) echo LEGACY_FLAT; return 0 ;;
          *) return 1 ;;
        esac
    fi
    if [ "$TESTING" = 1 ] && [ ! -d "$ARTIFACT_DIR/profiles" ] && [ ! -d "$ARTIFACT_DIR/universal" ]; then
        echo LEGACY_FLAT
        return 0
    fi

    # A caller from an older menu/script may still pass K1004 or P1404.  Treat
    # those names as deprecated aliases, never as selectors for profile payloads.
    case "$requested" in
      ""|UNIVERSAL) ;;
      K1004|P1404)
        echo "LEGACY_PROFILE_REQUEST_IGNORED requested=$requested route=UNIVERSAL" >&2
        ;;
      *)
        echo "PROFILE_REFUSED unsupported=$requested" >&2
        return 1
        ;;
    esac

    # Universal still requires the stock inputs it will reuse.  We deliberately
    # do not fingerprint them for route selection.
    dio_rel=$(locate_first "$LIVE_DIO_CANDIDATES") || return 1
    [ -s "$(p "$LIVE_LIBAIRPLAY")" ] && [ -s "$(p "$dio_rel")" ] || return 1

    train=$(read_aug22_train || true)
    case "$train" in
      *AUG22*)
        echo "AUG22_UNIVERSAL_ROUTE train='$train' stock_reuse=YES profile_overlay=DISABLED" >&2
        echo UNIVERSAL
        return 0
        ;;
      *)
        echo "PROFILE_REFUSED aug22_proof=ABSENT train='$train' universal_only=YES" >&2
        return 1
        ;;
    esac
}

validate_runtime_sources(){
    for name in $RUNTIME_SCRIPTS; do
        src="$SD_SCRIPTS/$name"
        [ -s "$src" ] || { echo "FAIL: runtime companion missing/empty: $src" >&2; return 1; }
        case "$name" in *.sh) sh -n "$src" || { echo "FAIL: runtime companion shell syntax: $name" >&2; return 1; } ;; esac
    done
    for name in carplay-alt111-mirror-display start_vehicle.sh stop_vehicle.sh BUILD_INFO.txt logo.rgba watermark.rgba; do
        [ -s "$MIRROR_SD/$name" ] || { echo "FAIL: integrated direct-display sidecar missing/empty: $MIRROR_SD/$name" >&2; return 1; }
    done
    grep -Fq 'release_binary_status=PRIVATE111_DIRECT_DISPLAY_V2' "$MIRROR_SD/BUILD_INFO.txt" 2>/dev/null &&
    grep -Fq 'vehicle_zip_status=READY_FOR_VEHICLE_TEST' "$MIRROR_SD/BUILD_INFO.txt" 2>/dev/null || {
        echo "FAIL: direct-display release is not vehicle-ready V2; rebuild/promote QNX sidecar first" >&2
        return 1
    }
    sh -n "$MIRROR_SD/start_vehicle.sh" || return 1
    sh -n "$MIRROR_SD/stop_vehicle.sh" || return 1
    return 0
}

precheck_app_runtime(){
    parent="$(p /mnt/app/root)"
    probe="$parent/.altscreen-write-test.$$"
    token="altscreen-write-test-$$"
    mount_app_rw || { echo "FAIL: cannot mount /mnt/app writable" >&2; return 1; }
    ok=1
    ensure_dirs "$parent" || ok=0
    if [ "$ok" = 1 ]; then printf '%s\n' "$token" > "$probe" 2>/dev/null || ok=0; fi
    if [ "$ok" = 1 ]; then [ "$(cat "$probe" 2>/dev/null)" = "$token" ] || ok=0; fi
    rm -f "$probe" 2>/dev/null || true
    sync >/dev/null 2>&1 || true
    if ! mount_app_ro; then
        echo "FAIL: /mnt/app write precheck could not restore read-only mount" >&2
        return 1
    fi
    [ "$ok" = 1 ] || { echo "FAIL: /mnt/app/root is not safely writable" >&2; return 1; }
    echo "APP_RUNTIME_WRITE_PRECHECK=PASS path=/mnt/app/root"
    return 0
}

install_runtime_scripts(){
    validate_runtime_sources || return 1
    [ ! -e "$RUNTIME_ROOT" ] || [ -f "$RUNTIME_ROOT/$RUNTIME_OWNER" ] || {
        echo "FAIL: refusing to replace unowned runtime: /mnt/app/root/carplay-altscreen" >&2
        return 1
    }
    [ ! -e "$RUNTIME_PREV" ] || [ -f "$RUNTIME_PREV/$RUNTIME_OWNER" ] || {
        echo "FAIL: refusing to replace unowned previous runtime" >&2
        return 1
    }
    mount_app_rw || return 1
    cleanup_stale_runtime_stages || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
    # Recover a directory exchange interrupted before the prior INSTALL could
    # commit. Both directories are owned, and the published one is still inert
    # under transaction.pending. Preserve the previous runtime for this retry.
    if [ -d "$RUNTIME_PREV" ]; then
        if [ -d "$RUNTIME_ROOT" ]; then
            [ -f "$RUNTIME_PENDING" ] || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
            rm -rf "$RUNTIME_ROOT" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
        fi
        mv "$RUNTIME_PREV" "$RUNTIME_ROOT" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
    fi
    rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
    ensure_dirs "$RUNTIME_STAGE/bin" "$RUNTIME_STAGE/lib" "$RUNTIME_STAGE/state" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
    # Preserve the fixed runtime slots across an update. Never copy arbitrary
    # old files or per-session directories into the new owned runtime.
    if [ -d "$RUNTIME_ROOT/state" ]; then
        for leaf in diagnostics.enabled ARMED ARMED_MUTATE \
                    ARMED_INFO ARMED_FEATURE ARMED_CREATE111 ACTIVE FORCE_START \
                    IAP2_PROFILE run_id fullchain_probe transaction.pending; do
            [ -f "$RUNTIME_ROOT/state/$leaf" ] || continue
            cp "$RUNTIME_ROOT/state/$leaf" "$RUNTIME_STAGE/state/$leaf" || {
                rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
                mount_app_ro >/dev/null 2>&1 || true
                return 1
            }
        done
    fi
    for name in $RUNTIME_SCRIPTS; do
        src="$SD_SCRIPTS/$name"; dst="$RUNTIME_STAGE/bin/$name"
        cp "$src" "$dst" && cmp -s "$src" "$dst" || {
            rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
            mount_app_ro >/dev/null 2>&1 || true
            return 1
        }
        case "$name" in *.sh) chmod 755 "$dst" ;; *) chmod 644 "$dst" ;; esac || {
            rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
            mount_app_ro >/dev/null 2>&1 || true
            return 1
        }
    done
    ensure_dirs "$RUNTIME_STAGE/bin/mirror" || { rm -rf "$RUNTIME_STAGE" 2>/dev/null || true; mount_app_ro >/dev/null 2>&1 || true; return 1; }
    for name in carplay-alt111-mirror-display start_vehicle.sh stop_vehicle.sh BUILD_INFO.txt LICENSE.MMI-MIRROR SHA256SUMS logo.rgba watermark.rgba; do
        [ -f "$MIRROR_SD/$name" ] || continue
        cp "$MIRROR_SD/$name" "$RUNTIME_STAGE/bin/mirror/$name" || {
            rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
            mount_app_ro >/dev/null 2>&1 || true
            return 1
        }
    done
    chmod 755 "$RUNTIME_STAGE/bin/mirror/carplay-alt111-mirror-display"               "$RUNTIME_STAGE/bin/mirror/start_vehicle.sh"               "$RUNTIME_STAGE/bin/mirror/stop_vehicle.sh" || {
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    }
    printf '%s\n' 'owner=MMI-Cockpit-Carplay' 'mode=carplay-private111-direct-display-v2' > "$RUNTIME_STAGE/bin/mirror/$MIRROR_OWNER" || return 1
    printf '%s\n' 'owner=MMI-Cockpit-Carplay' 'runtime=carplay-altscreen' > "$RUNTIME_STAGE/$RUNTIME_OWNER" || {
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    }
    touch "$RUNTIME_STAGE/state/transaction.pending" || {
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    }
    # The complete previous runtime is held in RUNTIME_PREV until INSTALL commits.
    # No second copy is needed inside the newly published runtime.
    if [ -d "$RUNTIME_ROOT/bin/mirror" ] && [ ! -f "$RUNTIME_ROOT/bin/mirror/$MIRROR_OWNER" ]; then
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        echo "FAIL: current unified Mirror runtime is unowned" >&2
        return 1
    fi
    if [ -d "$RUNTIME_PREV" ]; then rm -rf "$RUNTIME_PREV" || {
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    }; fi
    if [ -d "$RUNTIME_ROOT" ]; then
        mv "$RUNTIME_ROOT" "$RUNTIME_PREV" || {
            rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
            mount_app_ro >/dev/null 2>&1 || true
            return 1
        }
        RUNTIME_HAD_CURRENT=1
    fi
    if ! mv "$RUNTIME_STAGE" "$RUNTIME_ROOT"; then
        [ "$RUNTIME_HAD_CURRENT" != 1 ] || mv "$RUNTIME_PREV" "$RUNTIME_ROOT" >/dev/null 2>&1 || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    fi
    RUNTIME_PUBLISHED=1
    sync >/dev/null 2>&1 || true
    if ! mount_app_ro; then return 1; fi
    echo "RUNTIME_SCRIPTS_INSTALLED=PASS path=/mnt/app/root/carplay-altscreen/bin no_eso_write=YES"
    return 0
}

rollback_runtime_scripts(){
    mount_app_rw >/dev/null 2>&1 || return 1
    rc=0
    if [ "$RUNTIME_PUBLISHED" = 1 ]; then
        if [ -d "$RUNTIME_ROOT" ] && [ -f "$RUNTIME_ROOT/$RUNTIME_OWNER" ]; then rm -rf "$RUNTIME_ROOT" || rc=1; fi
        if [ "$RUNTIME_HAD_CURRENT" = 1 ] && [ -d "$RUNTIME_PREV" ] && [ -f "$RUNTIME_PREV/$RUNTIME_OWNER" ]; then
            [ "$rc" != 0 ] || mv "$RUNTIME_PREV" "$RUNTIME_ROOT" >/dev/null 2>&1 || rc=1
        fi
    fi
    rm -rf "$RUNTIME_STAGE" 2>/dev/null || rc=1
    sync >/dev/null 2>&1 || true
    mount_app_ro >/dev/null 2>&1 || rc=1
    [ "$rc" = 0 ] || return 1
    RUNTIME_PUBLISHED=0
    RUNTIME_HAD_CURRENT=0
    return 0
}

cleanup_volatile_runtime(){
    volatile_root="$(p /tmp/MMI-Cockpit-Carplay)"
    if [ -e "$volatile_root" ]; then
        rm -rf "$volatile_root" 2>/dev/null || {
            echo "WARN: project volatile namespace could not be fully removed: /tmp/MMI-Cockpit-Carplay" >&2
            return 1
        }
    fi
    tmp_root="$(p /tmp)"
    for path in "$tmp_root"/MMI-Cockpit-Carplay.mirror.* \
                "$tmp_root"/MMI-Cockpit-Carplay.diag.* \
                "$tmp_root"/MMI-Cockpit-Carplay.startup.* \
                "$tmp_root"/MMI-Cockpit-Carplay.lock.boot_token* \
                "$tmp_root"/MMI-Cockpit-Carplay.boot_entry.log \
                "$tmp_root"/MMI-Cockpit-Carplay.altscreen_hook.log \
                "$tmp_root"/altscreen_hook.log \
                "$tmp_root"/altscreen_diag_* \
                "$tmp_root"/mmi-mirror-active \
                "$tmp_root"/mmi-mirror-basevideo.ready \
                "$tmp_root"/mmi-mirror-controller.started \
                "$tmp_root"/mmi-mirror-controller.log \
                "$tmp_root"/mmi-mirror-context.mode \
                "$tmp_root"/mmi-mirror-hmi.state \
                "$tmp_root"/mmi-mirror-hmi.state.tmp \
                "$tmp_root"/carplay111_linear_*_*.nv12 \
                "$tmp_root"/carplay111_consumer_*_*.nv12 \
                "$tmp_root"/carplay_alt111_pf_*_*.conf; do
        [ ! -e "$path" ] || rm -f "$path" || {
            echo "WARN: project volatile file could not be removed: $path" >&2
            return 1
        }
    done
    echo "VOLATILE_RUNTIME_CLEANUP=PASS path=/tmp project_files_and_legacy_directory"
    return 0
}

commit_runtime_scripts(){
    [ ! -e "$RUNTIME_PREV" ] || [ -f "$RUNTIME_PREV/$RUNTIME_OWNER" ] || {
        echo "FAIL: refusing to remove unowned previous runtime" >&2
        return 1
    }
    [ ! -e "$RUNTIME_PREV" ] && return 0
    mount_app_rw || return 1
    rm -rf "$RUNTIME_PREV" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
    sync >/dev/null 2>&1 || true
    mount_app_ro || return 1
    echo "RUNTIME_PREVIOUS_REMOVED=PASS"
}

remove_runtime_scripts(){
    [ ! -e "$RUNTIME_ROOT" ] || [ -f "$RUNTIME_ROOT/$RUNTIME_OWNER" ] || {
        echo "FAIL: refusing to remove unowned runtime: /mnt/app/root/carplay-altscreen" >&2
        return 1
    }
    [ ! -e "$RUNTIME_PREV" ] || [ -f "$RUNTIME_PREV/$RUNTIME_OWNER" ] || {
        echo "FAIL: refusing to remove unowned previous runtime" >&2
        return 1
    }
    mount_app_rw || return 1
    cleanup_stale_runtime_stages || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
    if [ -e "$RUNTIME_ROOT" ] || [ -e "$RUNTIME_PREV" ] || [ -e "$RUNTIME_STAGE" ]; then
        [ ! -e "$RUNTIME_ROOT" ] || rm -rf "$RUNTIME_ROOT" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
        [ ! -e "$RUNTIME_PREV" ] || rm -rf "$RUNTIME_PREV" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        sync >/dev/null 2>&1 || true
    fi
    mount_app_ro || return 1
    echo "RUNTIME_SCRIPTS_REMOVED=PASS path=/mnt/app/root/carplay-altscreen"
    return 0
}

persistent_diag_helper(){
    if [ -f "$RUNTIME_BIN/altscreen_persistent_diag.sh" ]; then
        printf '%s\n' "$RUNTIME_BIN/altscreen_persistent_diag.sh"
        return 0
    fi
    if [ -f "$PERSIST_DIAG_SD" ]; then
        printf '%s\n' "$PERSIST_DIAG_SD"
        return 0
    fi
    return 1
}

route_for_existing(){
    if [ -f "$ROUTE_FILE" ]; then cat "$ROUTE_FILE"; return 0; fi
    if [ -f "$LEGACY_ROUTE_FILE" ]; then cat "$LEGACY_ROUTE_FILE"; return 0; fi
    if [ "$TESTING" = 1 ] && [ ! -d "$ARTIFACT_DIR/profiles" ] && [ ! -d "$ARTIFACT_DIR/universal" ]; then echo LEGACY_FLAT; return 0; fi
    return 1
}

delegate(){
    route=$1; shift
    case "$route" in
      UNIVERSAL)
        [ -f "$UNIVERSAL" ] || fail "AUG22 universal controller missing: $UNIVERSAL"
        ALTSCREEN_SD_VOLUME="$VOLUME" /bin/sh "$UNIVERSAL" "$@"
        ;;
      K1004|P1404)
        # Historical profile runtime is intentionally unreachable.  The sole
        # exception is restoring a vehicle that was installed by an older build.
        [ "${1:-}" = restore ] || fail "legacy profile runtime is disabled ($route); run RESTORE ORIGINAL, reboot, then INSTALL to migrate to UNIVERSAL"
        [ -f "$KNOWN" ] || fail "legacy restore controller missing: $KNOWN"
        /bin/sh "$KNOWN" restore
        ;;
      LEGACY_FLAT)
        [ "$TESTING" = 1 ] || fail "LEGACY_FLAT is test-only"
        [ -f "$KNOWN" ] || fail "legacy test controller missing: $KNOWN"
        /bin/sh "$KNOWN" "$@"
        ;;
      *) fail "invalid persisted route: $route" ;;
    esac
}

delegate_install(){
    route=$1
    ensure_dirs "$STATE_DIR" "$LOG_ROOT" "$BACKUP_ROOT" "$STAGING_ROOT" || return 1
    tmp="$STATE_DIR/.child-install.$$"
    delegate "$route" install > "$tmp" 2>&1
    rc=$?
    tail -c 1048576 "$tmp" > "$LOG_ROOT/operations/INSTALL.log" 2>/dev/null || true
    sed 's/^INSTALL=PASS /CHILD_INSTALL=PASS /' "$tmp"
    rm -f "$tmp"
    return "$rc"
}

record_transaction(){
    ensure_dirs "$STATE_DIR" "$LOG_ROOT/operations" || return 1
    for stale in "$STATE_DIR"/.child-install.* "$STATE_DIR"/.child-restore.*; do
        [ -e "$stale" ] || [ -L "$stale" ] || continue
        suffix=${stale##*.}
        case "$suffix" in ''|*[!0-9]*) continue ;; esac
        kill -0 "$suffix" 2>/dev/null && continue
        [ ! -d "$stale" ] && rm -f "$stale" || return 1
    done
    printf '%s action=%s state=%s\n' "$(date +%Y%m%d_%H%M%S)" "$1" "$2" > "$LOG_ROOT/operations/TRANSACTION.log"
}

delegate_restore_recorded(){
    route=$1
    ensure_dirs "$STATE_DIR" "$LOG_ROOT/operations" || return 1
    tmp="$STATE_DIR/.child-restore.$$"
    delegate "$route" restore > "$tmp" 2>&1
    rc=$?
    tail -c 1048576 "$tmp" > "$LOG_ROOT/operations/RESTORE.log" 2>/dev/null || true
    cat "$tmp"
    rm -f "$tmp"
    return "$rc"
}

# Confirm actual SD writes before any persistent vehicle mutation. Some QNX
# mounts appear present but are read-only until explicitly remounted.
precheck_sd_write(){
    probe="$VOLUME/.altscreen-write-probe.$$"
    token="altscreen-write-probe-$$"
    if [ "$TESTING" = 1 ]; then
        [ -d "$VOLUME" ] || { echo "FAIL: test SD volume missing" >&2; return 1; }
    fi
    sd_probe_once(){
        ( umask 077; printf '%s\n' "$token" > "$probe" ) 2>/dev/null || return 1
        [ "$(cat "$probe" 2>/dev/null)" = "$token" ] || return 1
        rm -f "$probe" 2>/dev/null || return 1
        return 0
    }
    if sd_probe_once; then return 0; fi
    rm -f "$probe" 2>/dev/null || true
    if [ "$TESTING" != 1 ]; then
        mount -uw "$VOLUME" >/dev/null 2>&1 || true
        if sd_probe_once; then echo "SD_WRITE=READY remounted=YES volume=$VOLUME"; return 0; fi
    fi
    rm -f "$probe" 2>/dev/null || true
    echo "FAIL: SD_NOT_WRITABLE volume=$VOLUME" >&2
    return 1
}

CMD=${1:-}
case "$CMD" in
  sd-preflight)
    precheck_sd_write || exit 1
    echo "SD_WRITE=READY volume=$VOLUME"
    ;;
  install)
    precheck_sd_write || exit 1
    route=$(select_install_route "${2:-}") || exit 1
    existing_route=""
    if [ -f "$INSTALLED_MARKER" ] && [ -f "$ROUTE_FILE" ]; then existing_route="$ROUTE_FILE";
    elif [ -f "$LEGACY_INSTALLED_MARKER" ] && [ -f "$LEGACY_ROUTE_FILE" ]; then existing_route="$LEGACY_ROUTE_FILE"; fi
    if [ -n "$existing_route" ]; then
        old=$(cat "$existing_route")
        if [ "$old" != "$route" ]; then
            case "$old" in
              K1004|P1404) fail "legacy profile $old is still installed; run RESTORE ORIGINAL, reboot, then INSTALL to migrate to UNIVERSAL" ;;
              *) fail "installed route is $old but new route is $route; RESTORE ORIGINAL before switching" ;;
            esac
        fi
    fi
    echo "ROUTER_PROFILE=$route policy=AUG22_UNIVERSAL_ONLY known_profiles=REFERENCE_RESTORE_ONLY"
    validate_runtime_sources || exit 1
    precheck_app_runtime || exit 1
    record_transaction INSTALL IN_PROGRESS || fail "cannot record INSTALL transaction"
    mark_runtime_pending || fail "cannot mark installed runtime as pending"
    if ! install_runtime_scripts; then
        rollback_runtime_scripts >/dev/null 2>&1 || echo "WARN: runtime rollback incomplete; retry RESTORE ORIGINAL" >&2
        echo "FAIL: persistent runtime could not be staged under /mnt/app/root; no CarPlay mutation attempted" >&2
        exit 1
    fi
    if ! delegate_install "$route"; then
        rollback_runtime_scripts >/dev/null 2>&1 || echo "WARN: runtime rollback incomplete; retry RESTORE ORIGINAL" >&2
        exit 1
    fi
    if [ "$route" = UNIVERSAL ]; then
        diag=$(persistent_diag_helper || true)
        if [ -z "$diag" ] || ! ALTSCREEN_SD_VOLUME="$VOLUME" /bin/sh "$diag" install; then
            echo "FAIL: universal persistent diagnostics could not be installed; rolling back" >&2
            [ -z "$diag" ] || ALTSCREEN_SD_VOLUME="$VOLUME" /bin/sh "$diag" remove >/dev/null 2>&1 || true
            delegate "$route" restore >/dev/null 2>&1 || echo "WARN: rollback failed; use RESTORE ORIGINAL before reboot" >&2
            rollback_runtime_scripts >/dev/null 2>&1 || echo "WARN: runtime rollback incomplete; retry RESTORE ORIGINAL" >&2
            exit 1
        fi
    fi
    ensure_dirs "$STATE_DIR" || exit 1
    echo "$route" > "$ROUTE_FILE" || exit 1
    commit_runtime_scripts || fail "previous runtime cleanup failed after INSTALL"
    clear_runtime_pending || fail "installed runtime could not be activated; retry INSTALL"
    record_transaction INSTALL COMMITTED || echo "WARN: INSTALL completed but SD transaction log could not be updated" >&2
    echo "ROUTER_INSTALL=PASS profile=$route runtime=/mnt/app/root/carplay-altscreen/bin no_eso_write=YES"
    ;;
  restore)
    precheck_sd_write || exit 1
    route=$(route_for_existing) || fail "no installed firmware route; run INSTALL first"
    echo "ROUTER_PROFILE=$route"
    record_transaction RESTORE IN_PROGRESS || fail "cannot record RESTORE transaction"
    mark_runtime_pending || fail "cannot mark runtime as pending for RESTORE"
    if [ "$route" = UNIVERSAL ]; then
        diag=$(persistent_diag_helper || true)
        [ -n "$diag" ] || fail "universal persistent diagnostics helper is missing; refusing partial restore"
        ALTSCREEN_SD_VOLUME="$VOLUME" /bin/sh "$diag" remove || fail "could not disable universal persistent diagnostics"
    fi
    delegate_restore_recorded "$route" || exit $?
    remove_runtime_scripts || fail "originals restored but persistent runtime cleanup failed"
    cleanup_volatile_runtime || fail "project volatile cleanup failed after RESTORE"
    record_transaction RESTORE COMMITTED || echo "WARN: RESTORE completed but SD transaction log could not be updated" >&2
    ;;
  restore-preflight)
    precheck_sd_write || exit 1
    route=$(route_for_existing) || fail "no installed firmware route"
    [ "$route" = UNIVERSAL ] || fail "legacy route requires its original restore controller"
    ALTSCREEN_SD_VOLUME="$VOLUME" /bin/sh "$UNIVERSAL" restore-preflight
    ;;
  start|status|collect|disarm)
    if [ "$CMD" != status ]; then precheck_sd_write || exit 1; fi
    if [ "$CMD" = start ] && [ -e "$RUNTIME_PENDING" ]; then
        fail "an INSTALL or RESTORE is incomplete; run RESTORE ORIGINAL, then INSTALL"
    fi
    route=$(route_for_existing) || fail "no installed firmware route; run INSTALL first"
    echo "ROUTER_PROFILE=$route"
    delegate "$route" "$CMD"
    ;;
  *) echo "usage: altscreen_chain_test.sh {install|start|status|restore|restore-preflight|disarm|collect}" >&2; exit 2 ;;
esac
