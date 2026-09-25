# Select the SD holding this Toolbox, then verify an actual write/read/delete.
# This file is sourced by update_toolbox.sh in the vehicle's ksh environment.
VOLUME=""
for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
    [ -d "$candidate/Toolbox" ] || continue
    if [ "${TOOLBOX_REQUIRE_ALTSCREEN:-0}" = 1 ]; then
        [ -f "$candidate/Toolbox/scripts/update_toolbox.sh" ] || continue
        [ -f "$candidate/Toolbox/GEM/mqb-carplayAltScreen.esd" ] || continue
    fi
    VOLUME=$candidate
    break
done
if [ -z "$VOLUME" ] && [ "${TOOLBOX_REQUIRE_ALTSCREEN:-0}" != 1 ]; then
    echo "No SD-cards found."
    exit 0
fi
if [ -n "$VOLUME" ]; then
    probe="$VOLUME/.mmi-toolbox-write-probe.$$"
    token="mmi-toolbox-$$"
    sd_write_probe(){
        ( umask 077; printf '%s\n' "$token" > "$probe" ) 2>/dev/null || return 1
        [ "$(cat "$probe" 2>/dev/null)" = "$token" ] || return 1
        rm -f "$probe" 2>/dev/null
    }
    if ! sd_write_probe; then
        rm -f "$probe" 2>/dev/null || true
        mount -uw "$VOLUME" >/dev/null 2>&1 || true
        if ! sd_write_probe; then
            rm -f "$probe" 2>/dev/null || true
            echo "Toolbox SD cannot be written: $VOLUME" >&2
            VOLUME=""
        fi
    fi
fi
[ -n "$VOLUME" ] || {
    [ "${TOOLBOX_REQUIRE_ALTSCREEN:-0}" = 1 ] || exit 1
}
[ -n "$VOLUME" ] && export VOLUME
