#!/bin/sh
# Build the universal MIB2Q AUG22 preload fallback (QNX ARMv7 LE).
#
# K1004/P1404 continue to use build_k1004_overlay.sh and their measured direct
# overlay.  This standalone object is packaged only as the fallback for AUG22
# binaries that do not match either known baseline.  Its stock calls are resolved
# by name and its internal libairplay redirects are derived from ELF relocations,
# never from a firmware-relative GOT/function offset.
set -eu
export LC_ALL=C

SRC="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
# Source-only geometry/position regression check, before creating build output.
python3 "$SRC/tools/validate_card_safe_area.py"
OUT="${1:-$SRC/../build}"
STUBS="$OUT/stubs"
INCLUDE_SRC="$OUT/source_include"
TARGET="armv7-linux-gnueabi"
CF="-target $TARGET -march=armv7-a -marm -mfloat-abi=softfp -fPIC -O2 -ffreestanding"
CF="$CF -fno-stack-protector -fno-builtin -nostdinc -Wall -Wextra -Werror"
CF="$CF -isystem $(clang -print-file-name=include)"
CF="$CF -I$INCLUDE_SRC -I$INCLUDE_SRC/qnxshim"
LF="-fuse-ld=lld -nostdlib -shared -Wl,--build-id=none,--no-rosegment,-z,norelro,-z,max-page-size=4096"
LF="$LF -Wl,--hash-style=sysv -Wl,--no-as-needed -L$STUBS -Wl,--allow-shlib-undefined"

LLD_PROG="$(clang --print-prog-name=ld.lld 2>/dev/null || true)"
if [ -n "$LLD_PROG" ] && [ -x "$LLD_PROG" ]; then
  LLD_VERSION="$($LLD_PROG --version 2>/dev/null | head -1)"
elif command -v ld.lld >/dev/null 2>&1; then
  LLD_PROG="$(command -v ld.lld)"
  LLD_VERSION="$($LLD_PROG --version 2>/dev/null | head -1)"
else
  LLD_PROG="clang:-fuse-ld=lld"
  LLD_VERSION="lld selected by clang -fuse-ld=lld"
fi

mkdir -p "$OUT" "$STUBS"
rm -f "$INCLUDE_SRC"
ln -s "$SRC" "$INCLUDE_SRC"
echo "int __altscreen_stub_anchor(void) { return 0; }" > "$OUT/empty_stub.c"
clang $CF -c -o "$OUT/empty_stub.o" "$OUT/empty_stub.c"
for lib in libc.so.3 libm.so.2; do
  clang --target=$TARGET -fuse-ld=lld -nostdlib -shared \
        -Wl,--build-id=none,-soname,$lib,-z,max-page-size=4096 \
        "$OUT/empty_stub.o" -o "$STUBS/$lib"
done

# The checked-in hook keeps the exact direct-overlay implementation and a no-op
# non-direct redirect shim.  For this UNIVERSAL build only, replace that shim in
# an output-tree copy with the dynamic ELF relocation resolver.  The K1004/P1404
# source and binaries therefore stay byte-for-byte on their proven code path.
HOOK_SRC="$OUT/altscreen_hook_universal.c"
python3 - "$SRC/altscreen_hook.c" "$HOOK_SRC" <<'PY'
from pathlib import Path
import sys
src, out = map(Path, sys.argv[1:])
text = src.read_text()
old = '''#else
int p1404_direct_install_internal_redirects(void) { return 1; }
int p1404_direct_internal_redirects_ready(void) { return 1; }
#endif
'''
new = '''#else
extern int aug22_dynamic_install_internal_redirects(void);
extern int aug22_dynamic_internal_redirects_ready_logged(void);
int p1404_direct_install_internal_redirects(void) {
    return aug22_dynamic_install_internal_redirects();
}
int p1404_direct_internal_redirects_ready(void) {
    return aug22_dynamic_internal_redirects_ready_logged();
}
#endif
'''
if text.count(old) != 1:
    raise SystemExit('UNIVERSAL_BUILD_FAIL dynamic redirect shim source drift')
out.write_text(text.replace(old, new))
PY

# The known direct overlay already accepts both measured process identities and
# has a QNX fallback for systems where /proc/<pid>/exefile is ELF content rather
# than a readable pathname.  Reuse those two measured behaviours only in this
# output-tree UNIVERSAL copy; checked-in p1404_resolve.c stays untouched.
RESOLVE_SRC="$OUT/p1404_resolve_universal.c"
python3 - "$SRC/p1404_resolve.c" "$RESOLVE_SRC" <<'PY'
from pathlib import Path
import sys
src, out = map(Path, sys.argv[1:])
text = src.read_text()
process_marker = '''#ifdef ALTSCREEN_DIRECT_PROXY
    /* Captured QNX 6.5 libc exports _cmdname, _argv and __progname.'''
process_replacement = '''#if 1
    /* Captured QNX 6.5 libc exports _cmdname, _argv and __progname.'''
if text.count(process_marker) != 1:
    raise SystemExit('UNIVERSAL_BUILD_FAIL process-name fallback source drift')
text = text.replace(process_marker, process_replacement, 1)
old = '''#else
    return !strcmp(name, P1404_HOST_PROCESS);
#endif
}'''
new = '''#else
    return !strcmp(name, P1404_HOST_PROCESS) ||
           !strcmp(name, K1004_HOST_PROCESS);
#endif
}'''
if text.count(old) != 1:
    raise SystemExit('UNIVERSAL_BUILD_FAIL process identity source drift')
out.write_text(text.replace(old, new))
PY

SRCS=""
# p1404_airplay_fullchain.c includes p1404_airplay.c after renaming only the
# legacy PlatformControl symbol; never compile p1404_airplay.c separately here.
# p1404_private111_backend.c p1404_firewall.c remains the same authenticated/private111 core used
# by the known profiles.  Universalization changes only discovery/interposition.
for c in altscreen_core.c altscreen_paths.c altscreen_profile.c altscreen_state.c altscreen_state_private.c private111_direct_tap.c p1404_private111.c p1404_private111_backend.c p1404_firewall.c p1404_cockpit_native.c p1404_setup_merge.c p1404_resolve.c p1404_iap2.c p1404_observe.c p1404_airplay_fullchain.c aug22_dynamic_reloc.c aug22_dynamic_diag.c altscreen_hook.c; do
  [ -f "$SRC/$c" ] || { echo "missing $SRC/$c"; exit 1; }
  input="$SRC/$c"
  [ "$c" != altscreen_hook.c ] || input="$HOOK_SRC"
  [ "$c" != p1404_resolve.c ] || input="$RESOLVE_SRC"
  clang $CF -c "$input" -o "$OUT/${c%.c}.o"
  SRCS="$SRCS $OUT/${c%.c}.o"
done
clang -target $TARGET -march=armv7-a -marm -c "$SRC/p1404_trampoline.s" -o "$OUT/p1404_trampoline.o"

clang --target=$TARGET $LF -Wl,-soname,libcarplay_altscreen.so -Wl,--version-script,$INCLUDE_SRC/libcarplay_altscreen.map -Wl,-l:libc.so.3 -Wl,-l:libm.so.2 \
      $SRCS "$OUT/p1404_trampoline.o" -o "$OUT/libcarplay_altscreen.so"

printf "\\002\\000\\000\\005" | dd of="$OUT/libcarplay_altscreen.so" bs=1 seek=36 conv=notrunc status=none

SO="$OUT/libcarplay_altscreen.so"
python3 "$SRC/tools/validate_qnx_load_layout.py" "$SO" > "$OUT/QNX_LOAD_LAYOUT.json"
RE=arm-linux-gnueabihf-readelf
DS=arm-linux-gnueabihf-readelf

echo "===== ELF verification (03.C) ====="
file "$SO"
$RE -h "$SO" | sed -n "1,14p"
$RE -d "$SO"
echo "--- defined dynamic symbols ---"
$DS --dyn-syms -W "$SO" | grep -w FUNC | awk "{print \$8}" | sort
echo "--- undefined dynamic symbols ---"
$DS --dyn-syms -W "$SO" | grep UND | awk "{print \$8}" | sort -u

NEEDED="$($RE -d "$SO" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' | tr '\n' ' ' | sed 's/ $//')"
[ "$NEEDED" = "libc.so.3 libm.so.2" ] || {
  echo "QNX_BUILD_FAIL unexpected DT_NEEDED: $NEEDED" >&2
  exit 1
}
$RE -h "$SO" | grep -Eq 'Flags:.*0x5000002' || {
  echo "QNX_BUILD_FAIL wrong ARM e_flags" >&2
  exit 1
}
UNDEFINED="$($DS --dyn-syms -W "$SO" | grep UND | awk '{print $8}' | sort -u)"
BAD_RUNTIME="$(printf '%s\n' "$UNDEFINED" | grep -E '^(__atomic|__sync|__aeabi|__gnu|__tls|__cxa|_Unwind)' || true)"
[ -z "$BAD_RUNTIME" ] || {
  echo "QNX_BUILD_FAIL compiler/runtime helper imports detected:" >&2
  printf '%s\n' "$BAD_RUNTIME" >&2
  exit 1
}
SURFACE="$($DS --dyn-syms -W "$SO" | grep -w GLOBAL | grep -v UND | awk '{print $8}' | sort -u)"
EXPECTED_SURFACE='AirPlayReceiverServerPlatformCopyProperty
AirPlayReceiverSessionPlatformControl
AirPlayReceiverSessionPlatformCopyProperty
AirPlayReceiverSessionScreen_CopyDisplaysInfo
AirPlayReceiverSessionSetup
AirPlayReceiverSessionStart
AirPlayReceiverSessionTearDown
ScreenCopyMain
ScreenStreamCreate
ScreenStreamProcessData
ScreenStreamStart
_ScreenStreamSetProperty
_ZN3dio13CScreenRender6configERKNS_16st_screen_configE
_ZN3dio13CScreenRender6renderEPh
alt_oem_map_startup_ready
altscreen_oem_map_ready
close
recv
screen_create_window_buffers
screen_create_window_group
send
write'
[ "$SURFACE" = "$EXPECTED_SURFACE" ] || {
  echo "QNX_BUILD_FAIL preload export surface drift" >&2
  echo "actual:" >&2
  printf '%s\n' "$SURFACE" >&2
  exit 1
}
echo "QNX_ELF_INVARIANTS=PASS"

for marker in \
  'rate_policy=uncapped_source_callbacks' \
  'ALTAREA_LAYOUT_SAFE_V3' \
  '/tmp/mmi-mirror-hmi.state' \
  'safe_source=' \
  'safe_physical=' \
  'safe_yh_mapping=default72_cardfull88_bottom450' \
  'safearea_revision=V38FIX1_CARD_FULL_88_450' \
  'logo-card-full-safe-88-450' \
  'logo-card-small-safe-72-450' \
  'CARD_FULL_TOP88' \
  'renderer_geometry_revision=V31_ONE_TO_ONE_CLIP' \
  'map_plane_terminal_y_policy=metadata_only_not_renderer_offset' \
  'renderer_offset=' \
  'runtime_switch=updateViewArea' \
  'gate=LayoutMIB2HighB9' \
  'canvas_gate=%d' \
  'predeclared_even_if_hmi_late=' \
  'PHASE=ALT111_VIEWAREA_SUBMIT' \
  'PHASE=ALT111_VIEWAREA_TARGET' \
  'PHASE=ALT111_VIEWAREA_RESULT' \
  'same_session=1' \
  'renderer_scale=0' \
  'maps:/car/instrumentcluster/map?showSpeedLimit=user&showCompass=user&showETA=no&maneuverLayout=' \
  'OEM_STEPS_V1' \
  'OEM_TARGET_FOLLOW_V1' \
  'PHASE=WHEEL_ZOOM_TARGET' \
  'PHASE=WHEEL_ZOOM_TARGET_REBASE' \
  'PHASE=WHEEL_ZOOM_FRAME_STALL' \
  'PHASE=WHEEL_ZOOM_FRAME_RECOVERED' \
  'PHASE=WHEEL_ZOOM_STALL_ABORT' \
  'PHASE=WHEEL_ZOOM_PACED_SEND' \
  'response_gates_next=0' \
  'PHASE=SESSION_LIFECYCLE_ENTER' \
  'complete_session_serialization=1' \
  'PHASE=TEARDOWN_DUPLICATE' \
  'PHASE=OEM_MAP_READY' \
  'oem_map_not_initialized' \
  'PHASE=PHONE_CARD_LAYOUT_TARGET' \
  'PHASE=PHONE_CARD_UI_REFRESH' \
  'PHASE=PHONE_CARD_UI_OFFER' \
  'maneuverLayout=leftaligned' \
  'maneuverLayout=topaligned' \
  'maneuverLayout=rightaligned'
do
  grep -a -Fq "$marker" "$SO" || {
    echo "QNX_BUILD_FAIL missing runtime marker: $marker" >&2
    exit 1
  }
done
echo "QNX_LAYOUT_SAFEAREA_MARKERS=PASS"

# Build metadata is release evidence and must be byte-stable across arbitrary
# candidate directories. Normalize the private build root and avoid wall-clock,
# host-kernel and absolute linker-path fields.
DISPLAY_CF=$(printf '%s\n' "$CF" | sed "s#$OUT#<BUILD>#g")
DISPLAY_LF=$(printf '%s\n' "$LF" | sed "s#$OUT#<BUILD>#g")
echo "===== BUILD_INFO (03.A) ====="
{
  echo "component        libcarplay_altscreen.so"
  echo "safearea_revision V38FIX1_CARD_FULL_88_450"
  echo "safearea_card_full 300,88,840,362 bottom=450 positions=LEFT,TOP,RIGHT"
  echo "safearea_default_full 370,72,700,378"
  echo "safearea_small 490,72,460,378"
  echo "accepted_train   MIB2Q_AUG22_FALLBACK_ONLY"
  echo "compat_policy    KNOWN_K1004_P1404_FIRST_THEN_DYNAMIC"
  echo "abi_profiles     UNIVERSAL_DYNAMIC_RELOCATION"
  echo "host_processes   dio_manager,smartphone_integrator"
  echo "process_name     procfs_then_qnx_cmdname_argv_progname"
  echo "target_stack     stock exports by dlsym; internal GOT by ELF relocation name; fail-open on ambiguity"
  echo "runtime_logging  resolver verdict to flat /tmp/altscreen_hook.log; boot recorder appends to SD automatically; no nested /tmp dependency"
  echo "session_lifecycle PER_RECEIVER_SETUP_START_STOP_SERIALIZED_FULL_TEARDOWN_DEDUP"
  echo "observer_census IDLE_SLOT_REUSE_5S_SATURATION_LOG_EVERY_4096"
  echo "runtime_log_cap STOP_PRODUCER_FORMATTING_AT_16MB"
  echo "compiler         $(clang --version | head -1)"
  echo "linker           $LLD_VERSION"
  echo "linker_program   $(basename "$LLD_PROG")"
  echo "source_date_epoch ${SOURCE_DATE_EPOCH:-UNSET}"
  echo "sysroot          none (freestanding: -nostdinc -nostdlib + QNX-named stubs)"
  echo "CFLAGS          $DISPLAY_CF"
  echo "LDFLAGS         $DISPLAY_LF -Wl,-soname,libcarplay_altscreen.so"
  echo "NEEDED          $NEEDED"
  echo "UNDEFINED       $(printf '%s\n' "$UNDEFINED" | awk 'BEGIN{s=""} {s=s (s ? " " : "") $0} END{print s}')"
  echo "PRELOAD_SURFACE $(printf '%s\n' "$SURFACE" | awk 'BEGIN{s=""} {s=s (s ? " " : "") $0} END{print s}')"
  echo "e_flags          $($RE -h "$SO" | grep Flags)"
} > "$OUT/BUILD_INFO.txt"
cat "$OUT/BUILD_INFO.txt"
echo
echo "===== MANIFEST (03.E) ====="
{
  echo "CarPlay AltScreen MIB2Q AUG22 universal fallback candidate"
  echo
  echo "Known exact K1004/P1404 devices do not use this binary."
  echo "Runtime resolves stock libairplay functions and GOT slots by symbol/relocation name."
  echo "Install backup, transaction authorization, automatic encrypted SD diagnostics and SAVE+RESTORE remain enabled."
  echo
  for f in libcarplay_altscreen.so; do
    echo "$f:"
    echo "  POSIX cksum: $(cksum "$OUT/$f")"
    echo "  size:        $(wc -c < "$OUT/$f")"
    echo "  SHA-256:     $(sha256sum "$OUT/$f" | cut -d" " -f1)"
  done
} > "$OUT/MANIFEST.txt"
cat "$OUT/MANIFEST.txt"
echo
echo "--- checksums (03.E) ---"
cksum "$SO"
sha256sum "$SO"
ls -l "$SO"
