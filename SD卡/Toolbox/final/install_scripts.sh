#!/bin/ksh

echo Install scripts and menus from SD card

VOLUME="${1}"

# Make it writable
on -f mmx /bin/mount -uw /mnt/app || exit 1

# Copy scripts
on -f mmx /bin/mkdir -p /net/mmx/mnt/app/eso/hmi/engdefs/scripts/mqb || { on -f mmx /bin/mount -ur /mnt/app; exit 1; }
on -f mmx /bin/cp -r "$VOLUME"/Toolbox/scripts/*.sh /net/mmx/mnt/app/eso/hmi/engdefs/scripts/mqb || { on -f mmx /bin/mount -ur /mnt/app; exit 1; }
on -f mmx /bin/chmod a+rwx /net/mmx/mnt/app/eso/hmi/engdefs/scripts/mqb || { on -f mmx /bin/mount -ur /mnt/app; exit 1; }

# Copy additional GreenMenus
on -f mmx /bin/cp -r "$VOLUME"/Toolbox/GEM/*.esd /net/mmx/mnt/app/eso/hmi/engdefs || { on -f mmx /bin/mount -ur /mnt/app; exit 1; }

# Make readonly again
on -f mmx /bin/mount -ur /mnt/app || exit 1

echo Finished copying
