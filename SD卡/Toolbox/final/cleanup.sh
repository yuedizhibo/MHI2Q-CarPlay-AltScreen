#!/bin/ksh
# This script will cleanup old MIB Toolbox installations. 

echo "Cleanup, mounting app rw"
on -f mmx mount -uw /mnt/app || exit 1
rc=0

echo "Deleting old mqbcoding.esd pre v4.1"
echo "___________________________________" 
# Pass the command as a string to remote ksh preserve the *
on -f mmx ksh -c 'rm -rvf /eso/hmi/engdefs/mqbcoding.esd*' || rc=1

echo "" 
echo "Deleting old MIB Toolbox scripts pre v4.1"
echo "__________________________________________" 
on -f mmx ksh -c 'rm -vf /eso/bin/PhoneCustomer/*.sh' || rc=1
on -f mmx ksh -c 'rm -vf /eso/bin/PhoneCustomer/default/*.sh' || rc=1
on -f mmx rm -rvf /eso/bin/PhoneCustomer/default/scripts || rc=1

on -f mmx mount -ur /mnt/app || rc=1

echo "Cleanup complete."
exit "$rc"
