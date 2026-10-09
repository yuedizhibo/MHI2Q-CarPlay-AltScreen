#!/bin/sh
# Menu entry: one-step INSTALL with map + native/Java/renderer route guidance.
ALTS_INSTALL_RGI_MODE=WITH
export ALTS_INSTALL_RGI_MODE
INSTALLER="${0%/*}/altscreen_install.sh"
if [ ! -f "$INSTALLER" ]; then
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -f "$candidate/Toolbox/scripts/altscreen_install.sh" ]; then INSTALLER="$candidate/Toolbox/scripts/altscreen_install.sh"; break; fi
    done
fi
exec /bin/sh "$INSTALLER" "$@"
