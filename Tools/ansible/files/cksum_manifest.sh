#!/bin/sh
# cksum_manifest.sh PATH
# Print a POSIX-cksum manifest "crc size path" for every regular file
# under PATH, paths emitted exactly as given (call it with a relative
# path like `Toolbox` from the release root so entries verify against
# the same relative layout on the unit), sorted by path. cksum is POSIX
# and uses the same CRC algorithm on QNX, Linux and macOS, so a manifest
# generated on the laptop verifies on the head unit (note: QNX
# `cksum < file` prints padded fields plus a "STDIN" name token; the
# reader in the playbook normalises that). Binary-safe for any file type.
set -eu

dir=${1:?"usage: cksum_manifest.sh PATH"}
[ -d "$dir" ] || { echo "cksum_manifest.sh: not a directory: $dir" >&2; exit 2; }

find "$dir" -type f | LC_ALL=C sort | while IFS= read -r f; do
    set -- $(cksum < "$f")
    [ -n "${2:-}" ] || { echo "cksum_manifest.sh: cksum failed: $f" >&2; exit 1; }
    printf '%s %s %s\n' "$1" "$2" "$f"
done
