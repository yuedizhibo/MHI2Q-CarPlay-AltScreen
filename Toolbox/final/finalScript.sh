#!/bin/ksh

echo "FinalScript for HW ${1} on medium ${2}..."
VOLUME="${2:-}"
if [ -n "$VOLUME" ]; then
    [ -f "$VOLUME/Toolbox/final/install_scripts.sh" ] || {
        echo "FAIL: supplied SD medium has no Toolbox installer: $VOLUME" >&2
        exit 1
    }
else
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -f "$candidate/Toolbox/final/install_scripts.sh" ] &&
           [ -f "$candidate/Toolbox/GEM/mqb-carplayAltScreen.esd" ]; then
            VOLUME=$candidate
            break
        fi
    done
fi
[ -n "$VOLUME" ] || { echo "FAIL: matching Toolbox SD card not found" >&2; exit 1; }

# The SWDL medium may be mounted read-only. Logging falls back to one flat
# volatile file; reading the package and installing the Toolbox can continue.
on -f mmx /bin/mount -uw "$VOLUME" >/dev/null 2>&1 || true
PROBE="$VOLUME/.mmi-swdl-write-probe.$$"
TOKEN="mmi-swdl-$$"
LOGFILE=/tmp/MMI-Cockpit-Carplay.install_final.log
if ( umask 077; printf '%s\n' "$TOKEN" > "$PROBE" ) 2>/dev/null &&
   [ "$(cat "$PROBE" 2>/dev/null)" = "$TOKEN" ]; then
    rm -f "$PROBE" 2>/dev/null || true
    [ -d "$VOLUME/Log" ] || mkdir -p "$VOLUME/Log"
    if [ -d "$VOLUME/Log" ]; then
        LOGFILE="$VOLUME/Log/install_final.txt"
    fi
else
    rm -f "$PROBE" 2>/dev/null || true
    echo "SD_LOG=VOLATILE_ONLY volume=$VOLUME"
fi

/bin/ksh "$VOLUME/Toolbox/final/install_scripts.sh" "$VOLUME" > "$LOGFILE" 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    /bin/ksh "$VOLUME/Toolbox/final/cleanup.sh" >> "$LOGFILE" 2>&1
    RC=$?
fi
if [ "$RC" -eq 0 ]; then
    export LD_LIBRARY_PATH=/mnt/app/root/lib-target:/eso/lib:/mnt/app/usr/lib:/mnt/app/armle/lib:/mnt/app/armle/lib/dll:/mnt/app/armle/usr/lib
    export IPL_CONFIG_DIR=/etc/eso/production
    on -f mmx /net/mmx/mnt/app/eso/bin/apps/pc b:0:0xC002000D 1 >> "$LOGFILE" 2>&1
    RC=$?
fi
on -f mmx /bin/mount -ur "$VOLUME" >/dev/null 2>&1 || true
[ "$RC" -eq 0 ] || { echo "FAIL: Toolbox SWDL install; see $LOGFILE" >&2; exit "$RC"; }
touch /tmp/SWDLScript.Result || exit 1
echo "Done. log=$LOGFILE"
