#!/bin/sh
# Menu entry: INSTALL WITH NOTHING - one-step INSTALL; map only, no route guidance, no phone card.
ALTS_INSTALL_RGI_MODE=NO
ALTS_INSTALL_CARD_MODE=NO
export ALTS_INSTALL_RGI_MODE ALTS_INSTALL_CARD_MODE
INSTALLER="${0%/*}/altscreen_install.sh"
if [ ! -f "$INSTALLER" ]; then
    if [ -n "${ALTS_VOLUME:-}" ]; then
        [ -f "$ALTS_VOLUME/Toolbox/scripts/altscreen_install.sh" ] && INSTALLER="$ALTS_VOLUME/Toolbox/scripts/altscreen_install.sh"
    else
        for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
            if [ -f "$candidate/Toolbox/scripts/altscreen_install.sh" ]; then INSTALLER="$candidate/Toolbox/scripts/altscreen_install.sh"; break; fi
        done
    fi
fi
exec /bin/sh "$INSTALLER" "$@"
