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
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -d "$candidate/Toolbox" ]; then VOLUME=$candidate; break; fi
    done
    [ -n "$VOLUME" ] || { echo "FAIL: no Toolbox SD card discovered" >&2; exit 1; }
fi

STATE="$VOLUME/MMI-Cockpit-Carplay/state"
BOOT_BACKUP="$VOLUME/MMI-Cockpit-Carplay/backup/boot-diagnostics"
TXN_ROOT="$VOLUME/MMI-Cockpit-Carplay/staging/diag-txn"
ENABLED="$ROOT/mnt/app/root/carplay-altscreen/state/diagnostics.enabled"
DEVICE_SCRIPTS="$ROOT/mnt/app/root/carplay-altscreen/bin"
ensure_dirs "$STATE" || exit 1

mount_system_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/system; }
mount_system_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/system; }
mount_app_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/app; }
mount_app_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/app; }
publish_system_file(){
    src=$1; dst=$2; mode=$3; dir=${dst%/*}; base=${dst##*/}; tmp="$dir/.$base.altscreen.new.$$"
    if cmp -s "$src" "$dst" 2>/dev/null; then chmod "$mode" "$dst" 2>/dev/null || return 1; echo "SYSTEM_PUBLISH=SKIP_IDENTICAL target=$dst"; return 0; fi
    rm -f "$tmp" 2>/dev/null || true
    if cp "$src" "$tmp"; then :; else rc=$?; rm -f "$tmp" 2>/dev/null || true; return "$rc"; fi
    chmod "$mode" "$tmp" || { rc=$?; rm -f "$tmp" 2>/dev/null || true; return "$rc"; }
    cmp -s "$src" "$tmp" || { rm -f "$tmp" 2>/dev/null || true; return 1; }
    mv "$tmp" "$dst" || { rc=$?; rm -f "$tmp" 2>/dev/null || true; return "$rc"; }
}

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
    txn="$TXN_ROOT/diag-install.$$"
    ensure_dirs "$txn" || return 1
    trap 'rm -rf "$txn" 2>/dev/null || true' 0
    trap 'rm -rf "$txn" 2>/dev/null || true; exit 130' 1 2 15
    if ! mount_app_rw; then return 1; fi
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

    clean="$txn/startup.clean"; block="$txn/startup.block"; new="$txn/startup.new"
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
    # Flat /tmp contract: this path is available without creating directories.
    ALTS_BOOT_ENTRY=/tmp/altscreen_boot_entry.log
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
       ! ensure_dirs "$(dirname -- "$ENABLED")" || ! touch "$ENABLED"; then
        rm -f "$ENABLED" 2>/dev/null || true
        mount_app_ro >/dev/null 2>&1 || true
        return 1
    fi
    if ! cmp -s "$new" "$startup" 2>/dev/null; then
        mount_system_rw || { rm -f "$ENABLED" 2>/dev/null || true; mount_app_ro >/dev/null 2>&1 || true; return 1; }
        sys_rw=1
        if ! publish_system_file "$new" "$startup" 755 || ! sync; then
            rm -f "$ENABLED" 2>/dev/null || true
            mount_app_ro >/dev/null 2>&1 || true; mount_system_ro >/dev/null 2>&1 || true
            return 1
        fi
        mount_system_ro || return 1
        sys_rw=0
    fi
    if ! mount_app_ro; then return 1; fi
    app_rw=0
    rm -rf "$txn" 2>/dev/null || true
    trap - 0 1 2 15
    echo "PERSISTENT_DIAGNOSTICS=ENABLED auto_sd=YES source=/tmp/altscreen_hook.log runtime=/mnt/app/root/carplay-altscreen/bin adaptive_status=plaintext_filtered adaptive_failsafe=direct_sd full_stream=plaintext"
    return 0
}

precheck_remove_diag(){
    startup=$(find_startup) || { echo "DIAG_REMOVE_PRECHECK=FAIL reason=STARTUP_NOT_FOUND production_changed=NO" >&2; return 1; }
    if [ -e "$BOOT_BACKUP" ]; then
        if ! verify_boot_backup || [ "$(cat "$BOOT_BACKUP/path")" != "$(startup_rel "$startup")" ]; then
            echo "DIAG_REMOVE_PRECHECK=FAIL reason=BOOT_BACKUP_INVALID production_changed=NO" >&2
            return 1
        fi
    elif grep -q '^# BEGIN ALTSCREEN DIAGNOSTICS$' "$startup" 2>/dev/null; then
        echo "DIAG_REMOVE_PRECHECK=FAIL reason=BOOT_BACKUP_MISSING production_changed=NO" >&2
        return 1
    fi
    sh -n "$startup" >/dev/null 2>&1 || { echo "DIAG_REMOVE_PRECHECK=FAIL reason=STARTUP_SYNTAX_INVALID production_changed=NO" >&2; return 1; }
    ensure_dirs "$TXN_ROOT" || { echo "DIAG_REMOVE_PRECHECK=FAIL reason=SD_STAGING_UNAVAILABLE production_changed=NO" >&2; return 1; }
    clean="$TXN_ROOT/diag-remove-precheck.tmp"
    rm -f "$clean" 2>/dev/null || true
    if ! strip_block "$startup" > "$clean" || ! sh -n "$clean" >/dev/null 2>&1; then
        rm -f "$clean" 2>/dev/null || true
        echo "DIAG_REMOVE_PRECHECK=FAIL reason=DIAGNOSTICS_BLOCK_INVALID production_changed=NO" >&2
        return 1
    fi
    rm -f "$clean" 2>/dev/null || true
    echo "DIAG_REMOVE_PRECHECK=PASS production_changed=NO"
    return 0
}

remove_diag(){
    precheck_remove_diag || return 1
    startup=$(find_startup) || { echo "FAIL: startup.sh not found while disabling diagnostics" >&2; return 1; }
    sh -n "$startup" || return 1
    sys_rw=0; app_rw=0
    txn="$TXN_ROOT/diag-remove.$$"
    ensure_dirs "$txn" || return 1
    trap 'rm -rf "$txn" 2>/dev/null || true' 0
    trap 'rm -rf "$txn" 2>/dev/null || true; exit 130' 1 2 15
    clean="$txn/startup.clean"
    if ! strip_block "$startup" > "$clean" || ! sh -n "$clean"; then return 1; fi
    # AWK normalizes a missing final newline. If no unrelated startup edits
    # remain, publish the verified first backup so RESTORE is byte-exact.
    # Preserve later non-project edits when the cleaned content differs.
    if [ -f "$BOOT_BACKUP/COMPLETE" ]; then
        normalized="$txn/startup.original.normalized"
        awk '{print}' "$BOOT_BACKUP/startup.sh" > "$normalized" || return 1
        if cmp -s "$clean" "$normalized"; then
            cp "$BOOT_BACKUP/startup.sh" "$clean" &&
            cmp -s "$BOOT_BACKUP/startup.sh" "$clean" || return 1
            echo "STARTUP_RESTORE_BASELINE=EXACT"
        else
            echo "STARTUP_RESTORE_BASELINE=PROJECT_BLOCKS_REMOVED unrelated_edits=PRESERVED"
        fi
    fi
    if ! mount_app_rw; then return 1; fi
    app_rw=1
    if ! rm -f "$ENABLED"; then mount_app_ro >/dev/null 2>&1 || true; return 1; fi
    if ! cmp -s "$clean" "$startup" 2>/dev/null; then
        mount_system_rw || { mount_app_ro >/dev/null 2>&1 || true; return 1; }
        sys_rw=1
        if ! publish_system_file "$clean" "$startup" 755 || ! sync; then
            mount_app_ro >/dev/null 2>&1 || true; mount_system_ro >/dev/null 2>&1 || true
            return 1
        fi
        mount_system_ro || return 1
        sys_rw=0
    fi
    if ! mount_app_ro; then return 1; fi
    app_rw=0
    rm -rf "$txn" 2>/dev/null || true
    trap - 0 1 2 15
    echo "PERSISTENT_DIAGNOSTICS=DISABLED"
    return 0
}

case "$ACTION" in
  install) install_diag ;;
  remove-precheck) precheck_remove_diag ;;
  remove) remove_diag ;;
  *) echo "usage: $0 {install|remove-precheck|remove}" >&2; exit 2 ;;
esac
