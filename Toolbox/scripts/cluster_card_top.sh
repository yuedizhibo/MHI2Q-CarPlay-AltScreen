#!/bin/sh
# Menu entry: CARD TOP - place the iPhone's maneuver card (top).
LAYOUT="${0%/*}/cluster_card_layout.sh"
if [ ! -f "$LAYOUT" ]; then
    if [ -n "${ALTS_VOLUME:-}" ]; then
        [ -f "$ALTS_VOLUME/Toolbox/scripts/cluster_card_layout.sh" ] && LAYOUT="$ALTS_VOLUME/Toolbox/scripts/cluster_card_layout.sh"
    else
        for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
            if [ -f "$candidate/Toolbox/scripts/cluster_card_layout.sh" ]; then LAYOUT="$candidate/Toolbox/scripts/cluster_card_layout.sh"; break; fi
        done
    fi
fi
exec /bin/sh "$LAYOUT" top
