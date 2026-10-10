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
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -d "$candidate/Toolbox" ]; then VOLUME=$candidate; break; fi
    done
    [ -n "$VOLUME" ] || fail "no Toolbox SD card discovered"
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
RGI_SD="$ARTIFACT_DIR/rgi_renderer/release"
PERSIST_DIAG_SD="$SD_SCRIPTS/altscreen_persistent_diag.sh"
LIVE_DIO_CANDIDATES="/eso/bin/apps/dio_manager /mnt/app/eso/bin/apps/dio_manager"
LIVE_LIBAIRPLAY="/eso/lib/libairplay.so"

RUNTIME_ROOT="$(p /mnt/app/root/carplay-altscreen)"
RUNTIME_BIN="$RUNTIME_ROOT/bin"
RUNTIME_STAGE="$(p /mnt/app/root/.carplay-altscreen.new)"
RUNTIME_STAGE_PARENT="$(p /mnt/app/root)"
RUNTIME_PREV="$(p /mnt/app/root/.carplay-altscreen.previous)"
ROUTER_TMP="$(p /tmp/altscreen_router_child_install.$$)"
RESTORE_TXN_DIR="$SD_ROOT/restore-transaction/active"
INSTALL_TXN_DIR="$SD_ROOT/install-transaction/active"
RUNTIME_OWNER=.mmi-cockpit-carplay-runtime-owner
RUNTIME_PUBLISHED=0
RUNTIME_HAD_CURRENT=0
RUNTIME_SCRIPTS="altscreen_chain_test.sh altscreen_chain_test_known.sh altscreen_chain_test_universal.sh altscreen_console.sh altscreen_install.sh altscreen_v33_rgi_config.sh altscreen_sd_writable.sh altscreen_install_transaction.sh altscreen_restore_transaction.sh altscreen_restore_apply.sh altscreen_persistent_diag.sh altscreen_adaptive_diag.sh altscreen_boot_diag.sh altscreen_live_diag.sh altscreen_preload.awk install_mmi_cockpit_carplay_rx.sh install_mmi_cockpit_carplay_no_rgi.sh install_mmi_cockpit_carplay_with_rgi.sh start_mmi_cockpit_carplay_test.sh start_mmi_cockpit_carplay_rx_test.sh force_start_mmi_cockpit_carplay_rx_test.sh stop_mmi_cockpit_carplay_test.sh status_mmi_cockpit_carplay_test.sh finish_mmi_cockpit_carplay_test.sh"

mount_app_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/app; }
mount_app_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/app; }

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
    for name in carplay-alt111-mirror-display start_vehicle.sh stop_vehicle.sh stream_supervisor.sh rgi_supervisor.sh BUILD_INFO.txt; do
        [ -s "$MIRROR_SD/$name" ] || { echo "FAIL: integrated direct-display sidecar missing/empty: $MIRROR_SD/$name" >&2; return 1; }
    done
    grep -Fq 'release_binary_status=PRIVATE111_DIRECT_DISPLAY_V3_5' "$MIRROR_SD/BUILD_INFO.txt" 2>/dev/null &&
    grep -Fq 'vehicle_zip_status=READY_FOR_VEHICLE_TEST' "$MIRROR_SD/BUILD_INFO.txt" 2>/dev/null || {
        echo "FAIL: direct-display release is not vehicle-ready V3.5; rebuild/promote runtime first" >&2
        return 1
    }
    for name in maneuver_render flag_atlas.rgba BUILD_INFO.txt SHA256SUMS; do
        [ -s "$RGI_SD/$name" ] || { echo "FAIL: RGI runtime missing: $name" >&2; return 1; }
    done
    sh -n "$MIRROR_SD/start_vehicle.sh" || return 1
    sh -n "$MIRROR_SD/stop_vehicle.sh" || return 1
    sh -n "$MIRROR_SD/stream_supervisor.sh" || return 1
    sh -n "$MIRROR_SD/rgi_supervisor.sh" || return 1
    printf '%s\n' '{"carplay":{"envs":[]}}' |
        awk -v validate=1 -f "$SD_SCRIPTS/altscreen_preload.awk" >/dev/null || return 1
    return 0
}

precheck_app_runtime(){
    parent="$(p /mnt/app/root)"
    probe="$parent/.altscreen-write-test"
    token="altscreen-write-test-$"
    mount_app_rw || { echo "FAIL: cannot mount /mnt/app writable" >&2; return 1; }
    ok=1
    ensure_dirs "$parent" || ok=0
    # The probe uses one fixed project-owned path so an interrupted precheck can
    # never accumulate PID-suffixed files on persistent /mnt/app.
    rm -f "$probe" 2>/dev/null || true
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
    # New packages use one fixed project-owned staging directory. Also reap
    # legacy PID-suffixed staging left by an interrupted older installer.
    rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
    for stale in "$RUNTIME_STAGE_PARENT"/.carplay-altscreen.new.*; do
        [ -e "$stale" ] || continue
        rm -rf "$stale" 2>/dev/null || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
    done
    ensure_dirs "$RUNTIME_STAGE/bin" "$RUNTIME_STAGE/lib" "$RUNTIME_STAGE/state" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
    cp "$ARTIFACT_DIR/hmi/BUILD_INFO.txt" "$RUNTIME_STAGE/state/hmi-build-info.txt" &&
    cmp -s "$ARTIFACT_DIR/hmi/BUILD_INFO.txt" "$RUNTIME_STAGE/state/hmi-build-info.txt" &&
    chmod 644 "$RUNTIME_STAGE/state/hmi-build-info.txt" || {
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    }
    if [ "${ALTS_INSTALL_RGI_MODE:-WITH}" = NO ]; then
        : > "$RUNTIME_STAGE/state/rgi.disabled" || return 1
        chmod 644 "$RUNTIME_STAGE/state/rgi.disabled" || return 1
    fi
    echo "RUNTIME_STAGING_POLICY=BOUNDED path=/mnt/app/root/.carplay-altscreen.new legacy_pid_staging=reaped"
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
    for name in carplay-alt111-mirror-display start_vehicle.sh stop_vehicle.sh stream_supervisor.sh rgi_supervisor.sh BUILD_INFO.txt LICENSE.MMI-MIRROR SHA256SUMS; do
        [ -f "$MIRROR_SD/$name" ] || continue
        cp "$MIRROR_SD/$name" "$RUNTIME_STAGE/bin/mirror/$name" || {
            rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
            mount_app_ro >/dev/null 2>&1 || true
            return 1
        }
    done
    chmod 755 "$RUNTIME_STAGE/bin/mirror/carplay-alt111-mirror-display"               "$RUNTIME_STAGE/bin/mirror/start_vehicle.sh"               "$RUNTIME_STAGE/bin/mirror/stop_vehicle.sh"               "$RUNTIME_STAGE/bin/mirror/stream_supervisor.sh" "$RUNTIME_STAGE/bin/mirror/rgi_supervisor.sh" || {
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    }
    ensure_dirs "$RUNTIME_STAGE/bin/rgi" || {
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    }
    for name in maneuver_render flag_atlas.rgba BUILD_INFO.txt SHA256SUMS; do
        cp "$RGI_SD/$name" "$RUNTIME_STAGE/bin/rgi/$name" && cmp -s "$RGI_SD/$name" "$RUNTIME_STAGE/bin/rgi/$name" || {
            rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
            mount_app_ro >/dev/null 2>&1 || true
            return 1
        }
    done
    chmod 755 "$RUNTIME_STAGE/bin/rgi/maneuver_render" || {
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    }
    printf '%s\n' 'owner=MMI-Cockpit-Carplay' 'mode=carplay-private111-direct-display-v3.5' > "$RUNTIME_STAGE/bin/mirror/$MIRROR_OWNER" || return 1
    printf '%s\n' 'owner=MMI-Cockpit-Carplay' 'runtime=carplay-altscreen' > "$RUNTIME_STAGE/$RUNTIME_OWNER" || {
        rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    }
    # The full owned runtime is moved to one fixed same-filesystem rollback
    # slot below. Do not duplicate Mirror or any other previous-version payload
    # inside the new runtime; original/OEM backups live on the SD card.
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

commit_runtime_scripts(){
    # RUNTIME_PREV is a same-INSTALL rollback slot, not a persistent backup.
    # Once router INSTALL has fully succeeded it must not remain on /mnt/app.
    if [ "$RUNTIME_HAD_CURRENT" = 1 ] && [ -d "$RUNTIME_PREV" ]; then
        [ -f "$RUNTIME_PREV/$RUNTIME_OWNER" ] || {
            echo "WARN: refusing to clean unowned runtime rollback slot" >&2
            return 1
        }
        mount_app_rw >/dev/null 2>&1 || return 1
        rm -rf "$RUNTIME_PREV" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
        sync >/dev/null 2>&1 || true
        mount_app_ro >/dev/null 2>&1 || return 1
        echo "RUNTIME_ROLLBACK_SLOT_CLEANED=PASS path=/mnt/app/root/.carplay-altscreen.previous"
    fi
    RUNTIME_PUBLISHED=0
    RUNTIME_HAD_CURRENT=0
    return 0
}

rollback_runtime_scripts(){
    mount_app_rw >/dev/null 2>&1 || return 1
    if [ "$RUNTIME_PUBLISHED" = 1 ]; then
        if [ -d "$RUNTIME_ROOT" ] && [ -f "$RUNTIME_ROOT/$RUNTIME_OWNER" ]; then rm -rf "$RUNTIME_ROOT" || true; fi
        if [ "$RUNTIME_HAD_CURRENT" = 1 ] && [ -d "$RUNTIME_PREV" ] && [ -f "$RUNTIME_PREV/$RUNTIME_OWNER" ]; then
            mv "$RUNTIME_PREV" "$RUNTIME_ROOT" >/dev/null 2>&1 || true
        fi
    fi
    rm -rf "$RUNTIME_STAGE" 2>/dev/null || true
    sync >/dev/null 2>&1 || true
    mount_app_ro >/dev/null 2>&1 || true
    RUNTIME_PUBLISHED=0
    RUNTIME_HAD_CURRENT=0
    return 0
}

cleanup_volatile_runtime(){
    tmp_root="$(p /tmp)"
    rm -f "$tmp_root"/altscreen_start_* "$tmp_root"/altscreen_router_child_install.* \
          "$(p /tmp/altscreen_hook.log)" "$(p /tmp/altscreen_boot_entry.log)" \
          "$(p /tmp/altscreen_autostart.log)" \
          "$(p /tmp/altscreen_mirror.pid)" "$(p /tmp/altscreen_mirror.lifecycle.pid)" \
          "$(p /tmp/altscreen_mirror.stop.requested)" "$(p /tmp/altscreen_mirror.log)" \
          "$(p /tmp/altscreen_mirror.autorestart.log)" "$(p /tmp/altscreen_mirror.ready)" \
          "$(p /tmp/altscreen_mirror.phone111.gate)" \
          "$(p /tmp/altscreen-private111.stream-ready)" "$(p /tmp/altscreen-private111.stream-ready.new)" \
          "$(p /tmp/altscreen_stream_supervisor.pid)" "$(p /tmp/altscreen_stream_supervisor.active)" \
          "$(p /tmp/altscreen_stream_supervisor.log)" \
          "$(p /tmp/mmi-mirror-active)" "$(p /tmp/mmi-mirror-basevideo.ready)" \
          "$(p /tmp/mmi-mirror-controller.started)" 2>/dev/null || true
    posix_tmp=$(alts_posix_tmp_dir "$(p /tmp)")
    rmdir "$posix_tmp/altscreen_mirror.recovery.lock" 2>/dev/null || true
    rmdir "$(p /tmp/altscreen_mirror.recovery.lock)" 2>/dev/null || true
    # Boot identity and operation leases survive until reboot; deleting
    # them here could let a competing writer bypass its active lease.

    # Backward-compatible cleanup only: older 2026-09-21 builds may have left
    # this namespace behind. New code never creates or writes into it.
    legacy_root="$(p /tmp/MMI-Cockpit-Carplay)"
    [ ! -e "$legacy_root" ] || rm -rf "$legacy_root" 2>/dev/null || true
    echo "VOLATILE_RUNTIME_CLEANUP=PASS policy=flat_tmp legacy_namespace=purged"
    return 0
}

runtime_owned_by_project(){
    root=$1
    [ -d "$root" ] || return 1
    [ ! -L "$root" ] || return 1
    marker="$root/$RUNTIME_OWNER"
    [ -f "$marker" ] || return 1
    grep -Fxq 'owner=MMI-Cockpit-Carplay' "$marker" 2>/dev/null || return 1
    # Current markers also carry runtime=carplay-altscreen.  Accept the older
    # one-line project owner marker for backward-compatible RESTORE, but if a
    # runtime= field exists it must name this runtime exactly.
    if grep -q '^runtime=' "$marker" 2>/dev/null; then
        grep -Fxq 'runtime=carplay-altscreen' "$marker" 2>/dev/null || return 1
    fi
    return 0
}

runtime_empty_unowned_removable(){
    root=$1
    # Non-mutating mirror of the only unowned-runtime recovery we permit.
    # The directory must contain no files/symlinks and no directories except
    # the known empty project skeleton.
    [ -d "$root" ] || return 1
    [ ! -L "$root" ] || return 1
    (
        cd "$root" || exit 1
        find . -print 2>/dev/null | sort | while IFS= read -r rel; do
            case "$rel" in
              .|./bin|./bin/mirror|./lib|./state) ;;
              *) exit 7 ;;
            esac
        done
    )
}

runtime_cleanup_precheck(){
    # Every logical reason that could make persistent runtime cleanup refuse
    # must be decided before RESTORE APPLY touches startup/JAR/native files.
    [ ! -e "$RUNTIME_STAGE" ] || {
        echo "RUNTIME_CLEANUP_PRECHECK=FAIL reason=STAGING_PATH_PRESENT production_changed=NO" >&2
        return 1
    }

    if [ -e "$RUNTIME_PREV" ]; then
        runtime_owned_by_project "$RUNTIME_PREV" || {
            echo "RUNTIME_CLEANUP_PRECHECK=FAIL reason=PREVIOUS_RUNTIME_UNOWNED production_changed=NO" >&2
            return 1
        }
    fi

    if [ ! -e "$RUNTIME_ROOT" ]; then
        echo "RUNTIME_CLEANUP_PRECHECK=PASS root=ABSENT previous=$([ -e "$RUNTIME_PREV" ] && echo OWNED || echo ABSENT) production_changed=NO"
        return 0
    fi

    [ -d "$RUNTIME_ROOT" ] && [ ! -L "$RUNTIME_ROOT" ] || {
        echo "RUNTIME_CLEANUP_PRECHECK=FAIL reason=RUNTIME_NOT_SAFE_DIRECTORY production_changed=NO" >&2
        return 1
    }

    if runtime_owned_by_project "$RUNTIME_ROOT"; then
        echo "RUNTIME_CLEANUP_PRECHECK=PASS root=OWNED previous=$([ -e "$RUNTIME_PREV" ] && echo OWNED || echo ABSENT) production_changed=NO"
        return 0
    fi

    if runtime_empty_unowned_removable "$RUNTIME_ROOT"; then
        echo "RUNTIME_CLEANUP_PRECHECK=PASS root=EMPTY_UNOWNED_RECOVERY previous=$([ -e "$RUNTIME_PREV" ] && echo OWNED || echo ABSENT) production_changed=NO"
        return 0
    fi

    echo "RUNTIME_CLEANUP_PRECHECK=FAIL reason=UNOWNED_NONEMPTY_RUNTIME path=/mnt/app/root/carplay-altscreen production_changed=NO" >&2
    return 1
}

remove_empty_unowned_runtime_residue(){
    root=$1
    # Re-run the exact non-mutating policy immediately before removal so a
    # changed/foreign runtime can never be recursively deleted.
    runtime_empty_unowned_removable "$root" || return 1
    for dir in "$root/bin/mirror" "$root/bin" "$root/lib" "$root/state"; do
        [ ! -e "$dir" ] && continue
        rmdir "$dir" 2>/dev/null || return 1
    done
    rmdir "$root" 2>/dev/null || return 1
    return 0
}

remove_runtime_scripts(){
    runtime_unowned=0
    if [ -e "$RUNTIME_ROOT" ] && ! runtime_owned_by_project "$RUNTIME_ROOT"; then
        runtime_unowned=1
    fi
    if [ -e "$RUNTIME_PREV" ]; then
        runtime_owned_by_project "$RUNTIME_PREV" || {
            echo "FAIL: refusing to remove unowned previous runtime" >&2
            return 1
        }
    fi
    [ ! -e "$RUNTIME_STAGE" ] || {
        echo "FAIL: refusing to remove unexpected runtime staging path" >&2
        return 1
    }

    if [ -e "$RUNTIME_ROOT" ] || [ -e "$RUNTIME_PREV" ]; then
        mount_app_rw || return 1
        if [ -e "$RUNTIME_ROOT" ]; then
            if [ "$runtime_unowned" = 1 ]; then
                if remove_empty_unowned_runtime_residue "$RUNTIME_ROOT"; then
                    echo "RUNTIME_EMPTY_RESIDUE_REMOVED=PASS path=/mnt/app/root/carplay-altscreen policy=empty_known_dirs_only"
                else
                    mount_app_ro >/dev/null 2>&1 || true
                    echo "FAIL: refusing to remove unowned non-empty runtime: /mnt/app/root/carplay-altscreen" >&2
                    return 1
                fi
            else
                rm -rf "$RUNTIME_ROOT" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
            fi
        fi
        [ ! -e "$RUNTIME_PREV" ] || rm -rf "$RUNTIME_PREV" || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
        sync >/dev/null 2>&1 || true
        mount_app_ro || return 1
    fi
    echo "RUNTIME_SCRIPTS_REMOVED=PASS path=/mnt/app/root/carplay-altscreen"
    return 0
}

persistent_diag_helper(){
    # Prefer the SD package copy so RESTORE always uses the same audited
    # transaction logic as the package being executed, even if an older runtime
    # helper is still installed under /mnt/app from a previous V3 build.
    if [ -f "$PERSIST_DIAG_SD" ]; then
        printf '%s\n' "$PERSIST_DIAG_SD"
        return 0
    fi
    if [ -f "$RUNTIME_BIN/altscreen_persistent_diag.sh" ]; then
        printf '%s\n' "$RUNTIME_BIN/altscreen_persistent_diag.sh"
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

route_for_restore(){
    route_for_existing && return 0

    # Recovery must not depend only on disposable state markers. If the
    # trusted universal recovery set exists, it is sufficient proof to select
    # UNIVERSAL even after an older partial uninstall removed route markers.
    if [ -f "$BACKUP_ROOT/original/COMPLETE" ] &&
       [ -f "$BACKUP_ROOT/universal-hook-original/COMPLETE" ]; then
        echo UNIVERSAL
        return 0
    fi
    if [ -f "$VOLUME/Backup/AltScreenChain/original/COMPLETE" ] &&
       [ -f "$VOLUME/Backup/AltScreenChain/universal-hook-original/COMPLETE" ]; then
        echo UNIVERSAL
        return 0
    fi
    return 1
}

restore_transaction_active(){
    [ -f "$RESTORE_TXN_DIR/PREPARED" ] || [ -f "$RESTORE_TXN_DIR/APPLYING" ]
}

install_transaction_active(){
    [ -d "$INSTALL_TXN_DIR" ] || return 1
    [ ! -f "$INSTALL_TXN_DIR/ROLLBACK_INCOMPLETE" ] || return 0
    [ -f "$INSTALL_TXN_DIR/COMMITTED" ] && return 1
    [ -f "$INSTALL_TXN_DIR/ROLLED_BACK" ] && return 1
    return 0
}

install_transaction_cleanup_terminal(){
    [ -d "$INSTALL_TXN_DIR" ] || return 0
    [ ! -f "$INSTALL_TXN_DIR/ROLLBACK_INCOMPLETE" ] || return 0
    if [ -f "$INSTALL_TXN_DIR/COMMITTED" ] || [ -f "$INSTALL_TXN_DIR/ROLLED_BACK" ]; then
        rm -rf "$INSTALL_TXN_DIR" 2>/dev/null || {
            echo "WARN: terminal install transaction retained on SD; it is non-blocking" >&2
            return 0
        }
        echo "INSTALL_TRANSACTION_TERMINAL_CLEANUP=PASS"
    fi
    return 0
}

delegate(){
    route=$1; shift
    case "$route" in
      UNIVERSAL)
        [ -f "$UNIVERSAL" ] || fail "AUG22 universal controller missing: $UNIVERSAL"
        /bin/sh "$UNIVERSAL" "$@"
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
    # Child controller output is bounded, process-local scratch. Keep it as one
    # flat /tmp file so INSTALL never depends on creating a QNX /tmp directory tree.
    tmp="$ROUTER_TMP"
    delegate "$route" install > "$tmp" 2>&1
    rc=$?
    sed 's/^INSTALL=PASS /CHILD_INSTALL=PASS /' "$tmp"
    rm -f "$tmp"
    return "$rc"
}

CMD=${1:-}
case "$CMD" in
  package-precheck)
    # Read-only package closure validation before one-step INSTALL withdraws
    # the previous runtime. Reuse the exact staging requirements below.
    validate_runtime_sources || exit 1
    echo "PACKAGE_PREFLIGHT=PASS production_changed=NO"
    ;;
  install)
    case "${ALTS_INSTALL_RGI_MODE:-WITH}" in NO|WITH) ;; *) fail "invalid RGI install mode" ;; esac
    restore_transaction_active && fail "restore transaction is active; recover/finish RESTORE ORIGINAL before INSTALL"
    install_transaction_cleanup_terminal
    if install_transaction_active && [ "${ALTS_INSTALL_TXN_ACTIVE:-0}" != 1 ]; then
        fail "install transaction is active; recover/finish transactional INSTALL before direct controller INSTALL"
    fi
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
    if ! install_runtime_scripts; then
        rollback_runtime_scripts >/dev/null 2>&1 || true
        echo "FAIL: persistent runtime could not be staged under /mnt/app/root; no CarPlay mutation attempted" >&2
        exit 1
    fi
    if ! delegate_install "$route"; then
        rollback_runtime_scripts >/dev/null 2>&1 || true
        exit 1
    fi
    if [ "$route" = UNIVERSAL ]; then
        diag=$(persistent_diag_helper || true)
        if [ -z "$diag" ] || ! /bin/sh "$diag" install; then
            echo "FAIL: universal persistent diagnostics could not be installed; rolling back" >&2
            [ -z "$diag" ] || /bin/sh "$diag" remove >/dev/null 2>&1 || true
            delegate "$route" restore >/dev/null 2>&1 || echo "WARN: rollback failed; use RESTORE ORIGINAL before reboot" >&2
            rollback_runtime_scripts >/dev/null 2>&1 || true
            exit 1
        fi
    fi
    ensure_dirs "$STATE_DIR" || exit 1
    echo "$route" > "$ROUTE_FILE" || exit 1
    commit_runtime_scripts || {
        echo "FAIL: installed runtime rollback-slot cleanup failed; transactional INSTALL will restore PRE_INSTALL state" >&2
        exit 1
    }
    echo "ROUTER_INSTALL=PASS profile=$route runtime=/mnt/app/root/carplay-altscreen/bin no_eso_write=YES"
    ;;
  restore-precheck)
    route=$(route_for_restore) || fail "no trusted restore route/recovery set is available"
    echo "ROUTER_PROFILE=$route"
    [ "$route" = UNIVERSAL ] || fail "transactional restore precheck currently requires UNIVERSAL recovery data"
    delegate "$route" restore-precheck || exit $?
    runtime_cleanup_precheck || fail "runtime cleanup precheck failed; production files unchanged"
    diag=$(persistent_diag_helper || true)
    [ -n "$diag" ] && [ -f "$diag" ] ||
        fail "universal persistent diagnostics helper is missing; production files unchanged"
    /bin/sh "$diag" remove-precheck ||
        fail "persistent diagnostics removal precheck failed; production files unchanged"
    echo "RESTORE_ROUTER_PRECHECK=PASS runtime_cleanup=SAFE persistent_diag=SAFE production_changed=NO"
    ;;
  restore)
    route=$(route_for_restore) || fail "no trusted restore route/recovery set is available"
    echo "ROUTER_PROFILE=$route"
    if [ "$route" = UNIVERSAL ]; then
        # Recheck backup/runtime safety immediately before mutation. The outer
        # transaction wrapper already ran this path before RESTORE APPLY.
        delegate "$route" restore-precheck || fail "universal restore precheck failed; production files unchanged"
        runtime_cleanup_precheck || fail "runtime cleanup changed after preflight; refusing partial restore"
        diag=$(persistent_diag_helper || true)
        [ -n "$diag" ] && [ -f "$diag" ] ||
            fail "universal persistent diagnostics helper is missing; refusing partial restore"
        /bin/sh "$diag" remove-precheck ||
            fail "persistent diagnostics removal precheck changed after outer preflight"
        /bin/sh "$diag" remove || fail "could not disable universal persistent diagnostics"
    fi
    delegate "$route" restore || exit $?
    remove_runtime_scripts || fail "originals restored but persistent runtime cleanup failed"
    cleanup_volatile_runtime
    ;;
  start)
    restore_transaction_active && fail "restore transaction is active; START is blocked until recovery/restore completes"
    install_transaction_cleanup_terminal
    install_transaction_active && fail "install transaction is active; START is blocked until INSTALL commits or rolls back"
    route=$(route_for_existing) || fail "no installed firmware route; run INSTALL first"
    echo "ROUTER_PROFILE=$route"
    delegate "$route" "$CMD"
    ;;
  status|collect)
    route=$(route_for_existing) || fail "no installed firmware route; run INSTALL first"
    echo "ROUTER_PROFILE=$route"
    restore_transaction_active && echo "RESTORE_TRANSACTION=ACTIVE path=$RESTORE_TXN_DIR"
    if install_transaction_active; then
        echo "INSTALL_TRANSACTION=ACTIVE path=$INSTALL_TXN_DIR"
    elif [ -d "$INSTALL_TXN_DIR" ]; then
        echo "INSTALL_TRANSACTION=TERMINAL_STALE path=$INSTALL_TXN_DIR non_blocking=YES"
    fi
    delegate "$route" "$CMD"
    ;;
  *) echo "usage: altscreen_chain_test.sh {package-precheck|install|start|status|restore-precheck|restore|collect}" >&2; exit 2 ;;
esac
