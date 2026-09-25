#!/bin/sh
# Install/remove the persistent boot diagnostics recorder for the AUG22 universal path.
# Known K1004/P1404 keep using the frozen certified controller's identical block.
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

ACTION=${1:-}
TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
ROOT=""
VOLUME=""
if [ "$TESTING" = 1 ]; then
    ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
    case "$ROOT" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_ROOT" >&2; exit 2 ;; esac
    case "$VOLUME" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_VOLUME" >&2; exit 2 ;; esac
else
    VOLUME=${ALTSCREEN_SD_VOLUME:-}
    if [ -n "$VOLUME" ]; then
        [ -f "$VOLUME/Toolbox/scripts/altscreen_persistent_diag.sh" ] || {
            echo "FAIL: selected SD lacks persistent diagnostics helper" >&2; exit 1;
        }
    else
        for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
            if [ -f "$candidate/MMI-Cockpit-Carplay/state/firmware_profile.txt" ] &&
               [ -f "$candidate/Toolbox/scripts/altscreen_persistent_diag.sh" ]; then
                VOLUME=$candidate; break
            fi
        done
    fi
    [ -n "$VOLUME" ] || { echo "FAIL: no Toolbox SD card discovered" >&2; exit 1; }
fi

STATE="$VOLUME/MMI-Cockpit-Carplay/state"
TEMP_PREFIX="$ROOT/tmp/MMI-Cockpit-Carplay.diag"
BOOT_BACKUP="$VOLUME/MMI-Cockpit-Carplay/backup/boot-diagnostics"
ENABLED="$ROOT/mnt/app/root/carplay-altscreen/state/diagnostics.enabled"
DEVICE_SCRIPTS="$ROOT/mnt/app/root/carplay-altscreen/bin"

# Old releases left interrupted diagnostic drafts on the SD card. New drafts
# are volatile and never need to be retained for RESTORE.
cleanup_stale_diag_stages(){
    for stale in "$STATE/diag.clean."* "$STATE/diag.block."* \
                 "$STATE/diag.new."* "$STATE/diag.restore."* \
                 "$TEMP_PREFIX.clean."* "$TEMP_PREFIX.block."* \
                 "$TEMP_PREFIX.new."* "$TEMP_PREFIX.restore."*; do
            [ -e "$stale" ] || [ -L "$stale" ] || continue
            suffix=${stale##*.}
            case "$suffix" in ''|*[!0-9]*) continue ;; esac
            kill -0 "$suffix" 2>/dev/null && continue
            [ ! -d "$stale" ] || return 1
            rm -f "$stale" || return 1
    done
}
cleanup_stale_diag_stages || exit 1

mount_system_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/system; }
mount_system_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/system; }
mount_app_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/app; }
mount_app_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/app; }

find_startup(){
    for p in "$ROOT/mnt/system/etc/boot/startup.sh" "$ROOT/etc/boot/startup.sh"; do
        [ -f "$p" ] && { printf '%s\n' "$p"; return 0; }
    done
    return 1
}

startup_rel(){
    case "$1" in
      "$ROOT/mnt/system/etc/boot/startup.sh") echo /mnt/system/etc/boot/startup.sh ;;
      "$ROOT/etc/boot/startup.sh") echo /etc/boot/startup.sh ;;
      *) return 1 ;;
    esac
}

strip_block(){
    awk '
      $0 == "# BEGIN ALTSCREEN DIAGNOSTICS" { if (inside || seen++) exit 9; inside=1; next }
      $0 == "# END ALTSCREEN DIAGNOSTICS" { if (!inside) exit 9; inside=0; next }
      !inside { print }
      END { if (inside) exit 9 }
    ' "$1"
}

verify_boot_backup(){
    [ -f "$BOOT_BACKUP/COMPLETE" ] && [ -s "$BOOT_BACKUP/startup.sh" ] &&
    [ -f "$BOOT_BACKUP/startup.cksum" ] && [ -f "$BOOT_BACKUP/path" ] || return 1
    [ "$(cksum < "$BOOT_BACKUP/startup.sh")" = "$(cat "$BOOT_BACKUP/startup.cksum")" ] || return 1
    case "$(cat "$BOOT_BACKUP/path")" in
      /mnt/system/etc/boot/startup.sh|/etc/boot/startup.sh) return 0 ;;
      *) return 1 ;;
    esac
}

install_diag(){
    startup=$(find_startup) || { echo "FAIL: startup.sh not found" >&2; return 1; }
    rel=$(startup_rel "$startup") || return 1
    [ -f "$DEVICE_SCRIPTS/altscreen_boot_diag.sh" ] || { echo "FAIL: installed altscreen_boot_diag.sh missing" >&2; return 1; }
    [ -f "$DEVICE_SCRIPTS/altscreen_live_diag.sh" ] || { echo "FAIL: installed altscreen_live_diag.sh missing" >&2; return 1; }
    [ -f "$DEVICE_SCRIPTS/altscreen_adaptive_diag.sh" ] || { echo "FAIL: installed altscreen_adaptive_diag.sh missing" >&2; return 1; }
    sh -n "$startup" || return 1

    sys_rw=0; app_rw=0
    mount_system_rw || return 1; sys_rw=1
    if ! mount_app_rw; then
        [ "$sys_rw" = 0 ] || mount_system_ro >/dev/null 2>&1 || true
        return 1
    fi
    app_rw=1

    if [ -f "$BOOT_BACKUP/COMPLETE" ]; then
        if ! verify_boot_backup || [ "$(cat "$BOOT_BACKUP/path")" != "$rel" ]; then
            echo "FAIL: boot diagnostics backup is damaged or targets another startup.sh" >&2
            mount_app_ro >/dev/null 2>&1 || true; mount_system_ro >/dev/null 2>&1 || true
            return 1
        fi
    else
        if [ -e "$BOOT_BACKUP" ]; then
            echo "FAIL: incomplete boot diagnostics backup exists" >&2
            mount_app_ro >/dev/null 2>&1 || true; mount_system_ro >/dev/null 2>&1 || true
            return 1
        fi
        ensure_dirs "$BOOT_BACKUP" || { mount_app_ro >/dev/null 2>&1 || true; mount_system_ro >/dev/null 2>&1 || true; return 1; }
        cp "$startup" "$BOOT_BACKUP/startup.sh" &&
        cmp -s "$startup" "$BOOT_BACKUP/startup.sh" &&
        cksum < "$BOOT_BACKUP/startup.sh" > "$BOOT_BACKUP/startup.cksum" &&
        printf '%s\n' "$rel" > "$BOOT_BACKUP/path" &&
        touch "$BOOT_BACKUP/COMPLETE" || {
            mount_app_ro >/dev/null 2>&1 || true; mount_system_ro >/dev/null 2>&1 || true; return 1;
        }
        verify_boot_backup || {
            mount_app_ro >/dev/null 2>&1 || true; mount_system_ro >/dev/null 2>&1 || true; return 1;
        }
    fi

    clean="$TEMP_PREFIX.clean.$$"; block="$TEMP_PREFIX.block.$$"; new="$TEMP_PREFIX.new.$$"
    if ! strip_block "$startup" > "$clean"; then
        rm -f "$clean" "$block" "$new"
        mount_app_ro >/dev/null 2>&1 || true; mount_system_ro >/dev/null 2>&1 || true
        return 1
    fi
    cat > "$block" <<'BOOT_BLOCK'
# BEGIN ALTSCREEN DIAGNOSTICS
(
    PATH=${PATH:-/bin:/usr/bin}:/proc/boot:/armle/bin:/armle/scripts:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin:/eso/bin:/eso/bin/apps
    LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}:/proc/boot:/usr/lib:/armle/lib:/armle/lib/dll:/lib:/mnt/app/root/carplay-altscreen/lib:/eso/lib:/mnt/app/usr/lib:/mnt/app/armle/lib:/mnt/app/armle/lib/dll:/mnt/app/armle/usr/lib:/lib/dll
    export PATH LD_LIBRARY_PATH
    ALTS_RUNTIME=/mnt/app/root/carplay-altscreen/bin
    ALTS_BOOT_ENTRY=/tmp/MMI-Cockpit-Carplay.boot_entry.log
    if ( : >> "$ALTS_BOOT_ENTRY" ) 2>/dev/null; then
        exec >> "$ALTS_BOOT_ENTRY" 2>&1
    fi
    echo "BOOT_ENTRY pid=$$ universal_persistent_diag=1 runtime=$ALTS_RUNTIME"
    alts_boot_wait=0
    while { [ ! -f "$ALTS_RUNTIME/altscreen_boot_diag.sh" ] || [ ! -f "$ALTS_RUNTIME/altscreen_adaptive_diag.sh" ]; } && [ "$alts_boot_wait" -lt 60 ]; do
        sleep 2
        alts_boot_wait=$((alts_boot_wait + 1))
    done
    if [ -f "$ALTS_RUNTIME/altscreen_adaptive_diag.sh" ]; then
        /bin/sh "$ALTS_RUNTIME/altscreen_adaptive_diag.sh" &
        echo "ADAPTIVE_FAILSAFE_STARTED pid=$! wait_steps=$alts_boot_wait store_required=NO"
    else
        echo "ADAPTIVE_FAILSAFE_MISSING after_seconds=120"
    fi
    if [ -f "$ALTS_RUNTIME/altscreen_boot_diag.sh" ]; then
        echo "BOOT_HELPER_FOUND wait_steps=$alts_boot_wait"
        /bin/sh "$ALTS_RUNTIME/altscreen_boot_diag.sh"
        echo "BOOT_HELPER_EXIT rc=$?"
    else
        echo "BOOT_HELPER_MISSING after_seconds=120"
    fi
) > /dev/null 2>&1 < /dev/null &
# END ALTSCREEN DIAGNOSTICS
BOOT_BLOCK
    if ! awk 'FNR==NR {block=block $0 "\n"; next}
         FNR==1 {if ($0 ~ /^#!/) {print; printf "%s",block; next} printf "%s",block}
         {print}' "$block" "$clean" > "$new" || ! sh -n "$new" ||
       ! ensure_dirs "$(dirname -- "$ENABLED")" || ! touch "$ENABLED" ||
       ! cp "$new" "$startup" || ! chmod 755 "$startup" || ! sync; then
        rm -f "$clean" "$block" "$new"
        rm -f "$ENABLED" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true; mount_system_ro >/dev/null 2>&1 || true
        return 1
    fi
    rm -f "$clean" "$block" "$new"
    if ! mount_app_ro; then
        mount_system_ro >/dev/null 2>&1 || true
        return 1
    fi
    app_rw=0
    mount_system_ro || return 1
    sys_rw=0
    echo "PERSISTENT_DIAGNOSTICS=ENABLED auto_sd=YES source=/tmp/MMI-Cockpit-Carplay.altscreen_hook.log runtime=/mnt/app/root/carplay-altscreen/bin adaptive_status=plaintext_filtered adaptive_failsafe=direct_sd full_stream=plaintext"
    return 0
}

remove_diag(){
    startup=$(find_startup) || { echo "FAIL: startup.sh not found while disabling diagnostics" >&2; return 1; }
    sh -n "$startup" || return 1
    sys_rw=0; app_rw=0
    mount_system_rw || return 1; sys_rw=1
    if ! mount_app_rw; then
        mount_system_ro >/dev/null 2>&1 || true
        return 1
    fi
    app_rw=1
    clean="$TEMP_PREFIX.restore.$$"
    if ! strip_block "$startup" > "$clean" || ! sh -n "$clean" ||
       ! rm -f "$ENABLED" || ! cp "$clean" "$startup" || ! chmod 755 "$startup" || ! sync; then
        rm -f "$clean"
        mount_app_ro >/dev/null 2>&1 || true; mount_system_ro >/dev/null 2>&1 || true
        return 1
    fi
    rm -f "$clean"
    if ! mount_app_ro; then
        mount_system_ro >/dev/null 2>&1 || true
        return 1
    fi
    app_rw=0
    mount_system_ro || return 1
    sys_rw=0
    echo "PERSISTENT_DIAGNOSTICS=DISABLED"
    return 0
}

case "$ACTION" in
  install) install_diag ;;
  remove) remove_diag ;;
  *) echo "usage: $0 {install|remove}" >&2; exit 2 ;;
esac
