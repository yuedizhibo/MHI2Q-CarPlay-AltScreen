#!/bin/sh

TOOLBOX_REQUIRE_ALTSCREEN=1
SCRIPT_DIR=$(CDPATH='' cd -P -- "$(dirname -- "$0")" 2>/dev/null && pwd -P)
if [ -f "$SCRIPT_DIR/util_mountsd.sh" ]; then
    . "$SCRIPT_DIR/util_mountsd.sh"
else
    . /eso/hmi/engdefs/scripts/mqb/util_mountsd.sh
fi
if [ -z "$VOLUME" ]
then
	echo "No writable matching Toolbox SD found"
	exit 1
fi

# Make it writable
mount -uw /mnt/app || exit 1

echo "Copying scripts from $VOLUME"
mkdir -p /eso/hmi/engdefs/scripts/mqb
cp -r "$VOLUME"/Toolbox/scripts/* /eso/hmi/engdefs/scripts/mqb || { mount -ur /mnt/app; exit 1; }
chmod a+rwx /eso/hmi/engdefs/scripts/mqb || { mount -ur /mnt/app; exit 1; }

echo "Copying GreenEngineeringMenu from $VOLUME"
cp "$VOLUME"/Toolbox/GEM/*.esd /eso/hmi/engdefs || { mount -ur /mnt/app; exit 1; }

# Make readonly again
mount -ur /mnt/app || exit 1

echo Done.

exit 0
