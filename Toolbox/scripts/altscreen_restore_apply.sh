#!/bin/sh
# Internal V3 restore APPLY step. Do not invoke directly; use altscreen_restore_transaction.sh.
# Releases Java80 demand, removes the project-owned carplay_hook.jar, then
# restores the native AltScreen/preload transaction. No MMI Mirror is involved.
set -u

ensure_dirs() {
    for dir in "$@"; do
        [ -d "$dir" ] && continue
        mkdir -p "$dir" || return 1
    done
    return 0
}

BASE="$0"
RESOLVED=$(command -v -- "$BASE" 2>/dev/null)
[ -n "$RESOLVED" ] || RESOLVED="$BASE"
SCRIPTDIR=$(cd -P -- "$(dirname -- "$RESOLVED")" 2>/dev/null && pwd -P)
[ -n "$SCRIPTDIR" ] || { echo "FAIL: cannot resolve RESTORE directory"; exit 126; }

TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
DEVICE_ROOT=""
VOLUME=""
if [ "$TESTING" = 1 ]; then
    DEVICE_ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
else
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -d "$candidate/Toolbox" ]; then VOLUME=$candidate; break; fi
    done
fi

CONTROLLER="$SCRIPTDIR/altscreen_chain_test.sh"
[ -f "$CONTROLLER" ] || { echo "FAIL: installed chain controller missing"; exit 127; }
[ -n "$VOLUME" ] || { echo "FAIL: SD card required to restore original system files"; exit 1; }

RUNTIME="$DEVICE_ROOT/mnt/app/root/carplay-altscreen"
TXN_DIR="$VOLUME/MMI-Cockpit-Carplay/staging/restore-apply"
ENABLED="$RUNTIME/state/basevideo3.enabled"
JAR="$DEVICE_ROOT/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar"
ACTIVE="$DEVICE_ROOT/tmp/mmi-altscreen-active"
READY="$DEVICE_ROOT/tmp/mmi-altscreen-basevideo.ready"
STARTED="$DEVICE_ROOT/tmp/mmi-altscreen-controller.started"
MIRROR_STOP="$RUNTIME/bin/mirror/stop_vehicle.sh"

mount_app_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/app; }
mount_app_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/app; }
mount_system_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/system; }
mount_system_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/system; }
system_space_snapshot(){
    label=$1; target="$DEVICE_ROOT/mnt/system"
    echo "SYSTEM_SPACE_BEGIN label=$label path=$target"
    df -k "$target" 2>/dev/null || df "$target" 2>/dev/null || true
    echo "SYSTEM_SPACE_END label=$label"
}
publish_system_file(){
    src=$1; dst=$2; mode=$3; dir=${dst%/*}; base=${dst##*/}; tmp="$dir/.$base.altscreen.new.$$"
    if cmp -s "$src" "$dst" 2>/dev/null; then chmod "$mode" "$dst" 2>/dev/null || return 1; echo "SYSTEM_PUBLISH=SKIP_IDENTICAL target=$dst"; return 0; fi
    rm -f "$tmp" 2>/dev/null || true
    if cp "$src" "$tmp"; then :; else rc=$?; rm -f "$tmp" 2>/dev/null || true; echo "SYSTEM_WRITE_FAILED stage=copy target=$dst"; system_space_snapshot restore_publish_failed; return "$rc"; fi
    chmod "$mode" "$tmp" || { rc=$?; rm -f "$tmp" 2>/dev/null || true; return "$rc"; }
    cmp -s "$src" "$tmp" || { rm -f "$tmp" 2>/dev/null || true; return 1; }
    mv "$tmp" "$dst" || { rc=$?; rm -f "$tmp" 2>/dev/null || true; return "$rc"; }
}
cleanup_txn(){ [ ! -e "$TXN_DIR" ] || rm -rf "$TXN_DIR" 2>/dev/null || true; }
trap cleanup_txn 0
trap 'cleanup_txn; exit 130' 1 2 15



strip_blocks(){
    awk '
      {
        key=$0
        sub(/\r$/, "", key)
        trimmed=key
        gsub(/^[ \t]+/, "", trimmed)
        gsub(/[ \t]+$/, "", trimmed)

        if (trimmed == "# BEGIN ALT111 MIRROR AUTOSTART") {
            if (block != "") bad=8
            block="old"
            next
        }
        if (trimmed == "# END ALT111 MIRROR AUTOSTART") {
            if (block != "old") bad=8
            block=""
            next
        }
        if (trimmed == "# BEGIN ALT111 BASEVIDEO3 AUTOSTART") {
            if (block != "") bad=8
            block="new"
            next
        }
        if (trimmed == "# END ALT111 BASEVIDEO3 AUTOSTART") {
            if (block != "new") bad=8
            block=""
            next
        }
        if (block == "") print
      }
      END {
        if (bad) exit bad
        if (block != "") exit 9
      }
    ' "$1"
}

# Stop RGI through its resident Java owner before removing its executable.
rm -f "$DEVICE_ROOT/tmp/mmi-rgi.stopped" 2>/dev/null || true
: > "$DEVICE_ROOT/tmp/mmi-rgi.disabled" || { echo "FAIL: cannot withdraw RGI"; exit 1; }
rgi_wait=0
while [ ! -f "$DEVICE_ROOT/tmp/mmi-rgi.stopped" ] && [ "$rgi_wait" -lt 5 ]; do
    sleep 1; rgi_wait=$((rgi_wait + 1))
done
# Withdraw the supervisor before its runtime is removed, preventing respawn.
[ ! -x "$MIRROR_STOP" ] || /bin/sh "$MIRROR_STOP" >/dev/null 2>&1 || true
[ "${ALTSCREEN_CHAIN_TESTING:-0}" = 1 ] || slay maneuver_render >/dev/null 2>&1 || true
# Stop the pixel sidecar first. It has no context writer in this branch.
[ ! -x "$MIRROR_STOP" ] || /bin/sh "$MIRROR_STOP" >/dev/null 2>&1 || true
# Release demand while the current Java controller is still resident. It will
# observe active/ready withdrawal and return terminal1 to its stock context.
rm -f "$ACTIVE" "$READY" 2>/dev/null || true
sleep 1

if [ -f "$ENABLED" ]; then
    mount_app_rw || { echo "FAIL: cannot mount /mnt/app to disable BaseVideo3"; exit 1; }
    rm -f "$ENABLED" || { mount_app_ro >/dev/null 2>&1 || true; echo "FAIL: cannot remove BaseVideo3 enable marker"; exit 1; }
    sync >/dev/null 2>&1 || true
    mount_app_ro || { echo "FAIL: cannot remount /mnt/app read-only"; exit 1; }
fi

STARTUP=""
for candidate in "$DEVICE_ROOT/mnt/system/etc/boot/startup.sh" "$DEVICE_ROOT/etc/boot/startup.sh"; do
    if [ -f "$candidate" ]; then STARTUP=$candidate; break; fi
done
if [ -n "$STARTUP" ]; then
    rm -rf "$TXN_DIR" 2>/dev/null || true
    ensure_dirs "$TXN_DIR" || { echo "FAIL: cannot create SD RESTORE transaction directory"; exit 1; }
    CLEAN="$TXN_DIR/startup.clean"
    system_space_snapshot restore_begin
    strip_blocks "$STARTUP" > "$CLEAN" || { echo "FAIL: invalid BaseVideo3/Mirror autostart block"; exit 1; }
    sh -n "$CLEAN" || { echo "FAIL: startup.sh invalid after BaseVideo3 block removal"; exit 1; }
    if ! cmp -s "$CLEAN" "$STARTUP" 2>/dev/null; then
        mount_system_rw || { echo "FAIL: cannot mount /mnt/system writable"; exit 1; }
        rm -f "$STARTUP.basevideo3.clean."* "$STARTUP.basevideo3.block."* "$STARTUP.basevideo3.new."*               "$STARTUP.basevideo3.original."* "$STARTUP.basevideo3.restore."*               "${STARTUP%/*}/.${STARTUP##*/}.altscreen.new."* 2>/dev/null || true
        publish_system_file "$CLEAN" "$STARTUP" 755 || { mount_system_ro >/dev/null 2>&1 || true; echo "FAIL: SYSTEM_WRITE_FAILED publishing cleaned startup.sh"; exit 1; }
        sync >/dev/null 2>&1 || true
        mount_system_ro || { echo "FAIL: cannot remount /mnt/system read-only"; exit 1; }
    else
        echo "RESTORE_AUTOSTART=ALREADY_CLEAN"
    fi
    system_space_snapshot restore_after_publish
fi

mount_app_rw || { echo "FAIL: cannot mount /mnt/app to remove Java HMI"; exit 1; }
TMP="$JAR.basevideo3.restore.tmp"
rm -f "$JAR" "$TMP" || {
    mount_app_ro >/dev/null 2>&1 || true
    echo "FAIL: cannot remove project-owned carplay_hook.jar"; exit 1; }
[ ! -e "$JAR" ] || { mount_app_ro >/dev/null 2>&1 || true; echo "FAIL: carplay_hook.jar remains after removal"; exit 1; }
echo "HMI_CONTROL_PLANE=REMOVED project_owned=YES permanent_oem_backup=NOT_USED"
sync >/dev/null 2>&1 || true
mount_app_ro || { echo "FAIL: cannot remount /mnt/app read-only"; exit 1; }

rm -f "$STARTED" 2>/dev/null || true
/bin/sh "$CONTROLLER" restore
RC=$?
[ "$RC" -eq 0 ] || exit "$RC"

echo "BASEVIDEO3_BOOT_DEMAND=DISABLED"
echo "DIRECT_DISPLAY_SIDECAR=STOPPED native_dmdt=DISABLED"
echo "RESTORE=PASS integrated=AltScreen+H264Tap+DecoderTap+Displayable3+Java80 reboot_required=YES"
exit 0
