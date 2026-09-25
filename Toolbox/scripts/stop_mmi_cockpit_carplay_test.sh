#!/bin/sh
# private111 direct-display V2 RESTORE ORIGINAL.
# Releases Java80 demand, removes the project-owned carplay_hook.jar, then
# restores the native AltScreen/preload transaction.
set -u

BASE="$0"
RESOLVED=$(command -v -- "$BASE" 2>/dev/null)
[ -n "$RESOLVED" ] || RESOLVED="$BASE"
SCRIPTDIR=$(cd -P -- "$(dirname -- "$RESOLVED")" 2>/dev/null && pwd -P)
[ -n "$SCRIPTDIR" ] || { echo "FAIL: cannot resolve RESTORE directory"; exit 126; }

TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
DEVICE_ROOT=""
if [ "$TESTING" = 1 ]; then
    DEVICE_ROOT=${ALTSCREEN_CHAIN_ROOT:-}
fi

APP_BIN="$DEVICE_ROOT/mnt/app/root/carplay-altscreen/bin"
APP_SELF="$APP_BIN/stop_mmi_cockpit_carplay_test.sh"
if [ "$SCRIPTDIR" != "$APP_BIN" ] && [ -f "$APP_SELF" ] && [ -f "$APP_BIN/altscreen_chain_test.sh" ]; then
    echo "APP_RUNTIME_FORWARD action=RESTORE from=$SCRIPTDIR to=/mnt/app/root/carplay-altscreen/bin"
    if [ "$#" -gt 0 ]; then
        exec /bin/sh "$APP_SELF" "$@"
    else
        exec /bin/sh "$APP_SELF"
    fi
fi

CONTROLLER="$SCRIPTDIR/altscreen_chain_test.sh"
[ -f "$CONTROLLER" ] || { echo "FAIL: installed chain controller missing"; exit 127; }
/bin/sh "$CONTROLLER" restore-preflight || exit 1
RUNTIME="$DEVICE_ROOT/mnt/app/root/carplay-altscreen"
ENABLED="$RUNTIME/state/basevideo3.enabled"
JAR="$DEVICE_ROOT/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar"
ACTIVE="$DEVICE_ROOT/tmp/mmi-mirror-active"
READY="$DEVICE_ROOT/tmp/mmi-mirror-basevideo.ready"
STARTED="$DEVICE_ROOT/tmp/mmi-mirror-controller.started"
MIRROR_STOP="$RUNTIME/bin/mirror/stop_vehicle.sh"

mount_app_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/app; }
mount_app_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/app; }
mount_system_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/system; }
mount_system_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/system; }

# A power loss during RESTORE leaves the hook in stock forwarding mode. The
# next RESTORE resumes from the verified SD originals.
mount_app_rw || { echo "FAIL: cannot mount /mnt/app for restore journal"; exit 1; }
touch "$RUNTIME/state/transaction.pending" || {
    mount_app_ro >/dev/null 2>&1 || true
    echo "FAIL: cannot mark restore pending"; exit 1;
}
sync >/dev/null 2>&1 || true
mount_app_ro || { echo "FAIL: cannot remount /mnt/app after restore journal"; exit 1; }

strip_blocks(){
    awk '
      $0 == "# BEGIN ALT111 MIRROR AUTOSTART" { in_old=1; next }
      $0 == "# END ALT111 MIRROR AUTOSTART"   { in_old=0; next }
      $0 == "# BEGIN ALT111 BASEVIDEO3 AUTOSTART" { in_new=1; next }
      $0 == "# END ALT111 BASEVIDEO3 AUTOSTART"   { in_new=0; next }
      !in_old && !in_new { print }
      END { if (in_old || in_new) exit 9 }
    ' "$1"
}

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
    CLEAN="$DEVICE_ROOT/tmp/MMI-Cockpit-Carplay.startup.restore.$$"
    mount_system_rw || { echo "FAIL: cannot mount /mnt/system writable"; exit 1; }
    strip_blocks "$STARTUP" > "$CLEAN" || {
        rm -f "$CLEAN"; mount_system_ro >/dev/null 2>&1 || true
        echo "FAIL: invalid BaseVideo3/Mirror autostart block"; exit 1; }
    sh -n "$CLEAN" || {
        rm -f "$CLEAN"; mount_system_ro >/dev/null 2>&1 || true
        echo "FAIL: startup.sh invalid after BaseVideo3 block removal"; exit 1; }
    PUBLISH="$(dirname -- "$STARTUP")/.$(basename -- "$STARTUP").new.$$"
    cp "$CLEAN" "$PUBLISH" && chmod 755 "$PUBLISH" &&
        cmp -s "$CLEAN" "$PUBLISH" && mv "$PUBLISH" "$STARTUP" || {
        rm -f "$CLEAN" "$PUBLISH"; mount_system_ro >/dev/null 2>&1 || true
        echo "FAIL: cannot publish cleaned startup.sh"; exit 1; }
    rm -f "$CLEAN"
    for stale in "$DEVICE_ROOT/tmp/MMI-Cockpit-Carplay.startup.clean."* \
                 "$DEVICE_ROOT/tmp/MMI-Cockpit-Carplay.startup.block."* \
                 "$DEVICE_ROOT/tmp/MMI-Cockpit-Carplay.startup.new."* \
                 "$DEVICE_ROOT/tmp/MMI-Cockpit-Carplay.startup.original."* \
                 "$DEVICE_ROOT/tmp/MMI-Cockpit-Carplay.startup.restore."*; do
        [ -e "$stale" ] || [ -L "$stale" ] || continue
        suffix=${stale##*.}
        case "$suffix" in ''|*[!0-9]*) continue ;; esac
        kill -0 "$suffix" 2>/dev/null && continue
        [ ! -d "$stale" ] && rm -f "$stale" || {
            mount_system_ro >/dev/null 2>&1 || true
            echo "FAIL: cannot remove stale startup draft: $stale"; exit 1;
        }
    done
    # Remove transaction files left by older START versions on /mnt/system.
    for stale in "$STARTUP".basevideo3.clean.* "$STARTUP".basevideo3.block.* \
                 "$STARTUP".basevideo3.new.* "$STARTUP".basevideo3.original.* \
                 "$STARTUP".basevideo3.restore.*; do
        [ ! -e "$stale" ] || rm -f "$stale" || {
            mount_system_ro >/dev/null 2>&1 || true
            echo "FAIL: cannot remove stale BaseVideo3 startup transaction: $stale"; exit 1;
        }
    done
    sync >/dev/null 2>&1 || true
    mount_system_ro || { echo "FAIL: cannot remount /mnt/system read-only"; exit 1; }
fi

mount_app_rw || { echo "FAIL: cannot mount /mnt/app to remove Java HMI"; exit 1; }
rm -f "$JAR" "$JAR.basevideo3.tmp" "$JAR.basevideo3.restore.tmp" || {
    mount_app_ro >/dev/null 2>&1 || true
    echo "FAIL: cannot remove standalone carplay_hook.jar"; exit 1; }
[ ! -e "$JAR" ] || { mount_app_ro >/dev/null 2>&1 || true; echo "FAIL: carplay_hook.jar remains after removal"; exit 1; }
echo "HMI_CONTROL_PLANE=REMOVED"
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
