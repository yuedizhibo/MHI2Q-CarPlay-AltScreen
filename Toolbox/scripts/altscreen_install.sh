#!/bin/sh
# MMI-Cockpit-Carplay one-step INSTALL (GEM: INSTALL WITH RGI / INSTALL NO RGI).
#
#   1. check the package and firmware
#   2. remove any previous installation (transactional RESTORE ORIGINAL)
#   3. install (transactional INSTALL, rolls back on failure)
#   4. enable the boot autostart (START without launching anything live)
#   5. verify the final state
#
# One full head-unit reboot afterwards is enough. The GEM screen shows short
# progress lines and one RESULT block; the full detail is streamed to
# MMI-Cockpit-Carplay/logs/operations/install_*.log on the SD card.
set -u

ALTS_INSTALL_RGI_MODE=${ALTS_INSTALL_RGI_MODE:-WITH}
case "$ALTS_INSTALL_RGI_MODE" in
    WITH) MODE_TEXT="AltScreen + RGI" ;;
    NO)   MODE_TEXT="AltScreen only (no RGI)" ;;
    *)    echo "RESULT: FAILED - invalid install mode '$ALTS_INSTALL_RGI_MODE'"; exit 2 ;;
esac
export ALTS_INSTALL_RGI_MODE

TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
ROOT=""
VOLUME=""
if [ "$TESTING" = 1 ]; then
    ROOT=${ALTSCREEN_CHAIN_ROOT:-}
    VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
    case "$ROOT" in /tmp/*|/var/tmp/*) ;; *) echo "RESULT: FAILED - invalid ALTSCREEN_CHAIN_ROOT"; exit 2 ;; esac
    case "$VOLUME" in /tmp/*|/var/tmp/*) ;; *) echo "RESULT: FAILED - invalid ALTSCREEN_CHAIN_VOLUME"; exit 2 ;; esac
else
    for candidate in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
        if [ -d "$candidate/Toolbox" ]; then VOLUME=$candidate; break; fi
    done
fi

if [ -z "$VOLUME" ] || [ ! -d "$VOLUME/Toolbox" ]; then
    echo "=============================================="
    echo " RESULT: FAILED"
    echo " No SD card with a Toolbox folder was found."
    echo " Nothing was changed."
    echo " Insert the SD card and run INSTALL again."
    echo "=============================================="
    exit 1
fi

SCRIPTS="$VOLUME/Toolbox/scripts"
CONSOLE="$SCRIPTS/altscreen_console.sh"
if [ ! -f "$CONSOLE" ]; then
    echo "=============================================="
    echo " RESULT: FAILED"
    echo " Package incomplete: altscreen_console.sh missing."
    echo " Nothing was changed. Copy the full Toolbox folder"
    echo " to the SD card again."
    echo "=============================================="
    exit 1
fi
. "$CONSOLE"

SD_RW_HELPER="$SCRIPTS/altscreen_sd_writable.sh"
SD_OK=0
if [ -f "$SD_RW_HELPER" ]; then
    . "$SD_RW_HELPER"
    altscreen_sd_ensure_writable "$VOLUME" INSTALL_JOURNAL >/dev/null 2>&1 && SD_OK=1
fi
alts_ui_open_log "$VOLUME" install "$ROOT/tmp" || true
alts_ui_log "INSTALL_MODE=$ALTS_INSTALL_RGI_MODE volume=$VOLUME testing=$TESTING"

SD="$VOLUME/MMI-Cockpit-Carplay"
STEP_TOTAL=5
STEP_NO=0
STEP_NAME=""
step() {
    STEP_NO=$((STEP_NO + 1))
    STEP_NAME=$1
    alts_ui ""
    alts_ui "[$STEP_NO/$STEP_TOTAL] $STEP_NAME..."
    alts_ui_log "STEP_BEGIN n=$STEP_NO name=$STEP_NAME"
}
ok() {
    alts_ui "      OK${1:+ - $1}"
}

# finish STATUS RC LINE...
finish() {
    finish_status=$1; finish_rc=$2; shift 2
    if [ "$finish_status" = SUCCESS ]; then
        alts_ui_result "$finish_status" "$@"
    else
        alts_ui_result "$finish_status (step $STEP_NO/$STEP_TOTAL: $STEP_NAME)" "$@"
    fi
    alts_ui_close_log "$finish_rc"
    alts_ui_unlock
    exit "$finish_rc"
}

on_signal() {
    trap - 1 2 15
    finish FAILED 130 \
        "The installer was interrupted." \
        "Run INSTALL again before rebooting; it repairs" \
        "or rolls back any partial state automatically."
}
trap on_signal 1 2 15

# Run the transactional RESTORE ORIGINAL; returns its exit status.
run_restore() {
    ALTS_OPLOG_CAPTURED=1 alts_ui_run /bin/sh "$SCRIPTS/stop_mmi_cockpit_carplay_test.sh"
}

# Called when a later step failed after something was installed: put the head
# unit back to stock and report what state it is in now.
fail_and_restore() {
    reason=$1
    alts_ui "      Removing the partial installation..."
    if run_restore; then
        finish FAILED 1 "$reason" \
            "The pre-install configuration was restored." \
            "Fully reboot the head unit before using CarPlay," \
            "then check the log and run INSTALL again."
    fi
    finish FAILED 1 "$reason" \
        "Automatic removal ALSO failed." \
        "Do NOT reboot. Run RESTORE ORIGINAL now and keep" \
        "this SD card (it holds the stock backup)."
}

alts_ui_header \
    "MMI-Cockpit-Carplay installer" \
    "Mode: $MODE_TEXT" \
    "Keep the SD card inserted and the power on." \
    "Do not reboot until RESULT is shown."

# ---------------------------------------------------------------- step 1
step "Checking package and firmware"
alts_ui_lock "$ROOT/tmp" || finish FAILED 1 \
    "$ALTS_UI_LOCK_MESSAGE" \
    "$ALTS_UI_LOCK_DETAIL" \
    "Nothing was changed. Check the log before retrying."
[ "$SD_OK" = 1 ] || finish FAILED 1 \
    "The SD card is not writable." \
    "Nothing was changed. Check the card (FAT32," \
    "not write-protected) and run INSTALL again."

for f in \
    scripts/install_mmi_cockpit_carplay_rx.sh \
    scripts/altscreen_install_transaction.sh \
    scripts/altscreen_restore_transaction.sh \
    scripts/altscreen_restore_apply.sh \
    scripts/altscreen_chain_test.sh \
    scripts/altscreen_chain_test_universal.sh \
    scripts/start_mmi_cockpit_carplay_rx_test.sh \
    scripts/stop_mmi_cockpit_carplay_test.sh \
    carplay_alt_screen/hmi/carplay_hook-basevideo3.jar \
    carplay_alt_screen/hmi/BUILD_INFO.txt \
    carplay_alt_screen/universal/libcarplay_altscreen.so \
    carplay_alt_screen/rgi_meta/libcarplay_rgi_meta.so \
    carplay_alt_screen/mirror_display/release/carplay-alt111-mirror-display \
    carplay_alt_screen/rgi_renderer/release/maneuver_render
do
    [ -s "$VOLUME/Toolbox/$f" ] || finish FAILED 1 \
        "Package incomplete: Toolbox/$f is missing." \
        "Nothing was changed. Copy the full Toolbox folder" \
        "to the SD card again."
done

if ! ALTS_OPLOG_CAPTURED=1 ALTS_PACKAGE_PREFLIGHT=1 \
     alts_ui_run /bin/sh "$SCRIPTS/install_mmi_cockpit_carplay_rx.sh"; then
    finish FAILED 1 \
        "The package is incomplete or its scripts are invalid." \
        "The previous installation was not removed." \
        "Copy the full Toolbox folder to the SD card again."
fi

TRAIN=""
for rel in /net/rcc/dev/shmem/version.txt /dev/shmem/version.txt /net/mmx/dev/shmem/version.txt; do
    [ -r "$ROOT$rel" ] || continue
    TRAIN=$(sed -n '/Current train/p' "$ROOT$rel" 2>/dev/null | head -n 1)
    [ -n "$TRAIN" ] && break
done
alts_ui_log "FIRMWARE_TRAIN=$TRAIN"
case "$TRAIN" in
    *AUG22*) ;;
    "") finish FAILED 1 \
            "Could not read the firmware version." \
            "Nothing was changed. Only AUG22 firmware" \
            "is supported." ;;
    *)  finish FAILED 1 \
            "Unsupported firmware: ${TRAIN##*[ =]}" \
            "Nothing was changed. Only AUG22 firmware" \
            "is supported." ;;
esac
ok "firmware ${TRAIN##*[ =]}"

# ---------------------------------------------------------------- step 2
step "Checking for a previous installation"
alts_detect_previous "$ROOT" "$VOLUME"
OLD=$ALTS_PREVIOUS

if [ -n "$OLD" ]; then
    alts_ui "      Previous version found - removing it first"
    if ! run_restore; then
        if alts_ui_log_has 'recovery_required=YES'; then
            finish FAILED 1 \
                "Could not remove the previous version and" \
                "its rollback was incomplete." \
                "Do NOT reboot. Run RESTORE ORIGINAL again and" \
                "keep this SD card (it holds the stock backup)."
        fi
        finish FAILED 1 \
            "Could not remove the previous version." \
            "Nothing new was installed. Use the SD card from" \
            "the previous install (it holds the stock backup" \
            "in MMI-Cockpit-Carplay/backup), then run INSTALL."
    fi
    ok "previous version removed"
else
    ok "none found"
fi

# ---------------------------------------------------------------- step 3
step "Installing $MODE_TEXT"
if ! ALTS_OPLOG_CAPTURED=1 alts_ui_run /bin/sh "$SCRIPTS/install_mmi_cockpit_carplay_rx.sh"; then
    if alts_ui_log_has 'recovery_required=YES' || alts_ui_log_has 'rollback=INCOMPLETE'; then
        finish FAILED 1 \
            "Installation failed and the rollback was" \
            "incomplete." \
            "Do NOT reboot. Run RESTORE ORIGINAL now and keep" \
            "this SD card (it holds the stock backup)."
    fi
    if [ -n "$OLD" ]; then
        finish FAILED 1 \
            "Installation failed; all changes were rolled back." \
            "The previous version was already removed, so the" \
            "head unit is stock now. Fully reboot the head" \
            "unit before using CarPlay, then run INSTALL again."
    fi
    finish FAILED 1 \
        "Installation failed; all changes were rolled back." \
        "The head unit is unchanged. Check the log, then" \
        "run INSTALL again."
fi
ok

# ---------------------------------------------------------------- step 4
step "Enabling autostart"
if ! ALTS_START_CAPTURED=1 ALTS_START_DEFER_SUPERVISOR=1 \
     alts_ui_run /bin/sh "$SCRIPTS/start_mmi_cockpit_carplay_rx_test.sh"; then
    fail_and_restore "Could not enable the boot autostart."
fi
ok

# ---------------------------------------------------------------- step 5
step "Verifying installation"
RUNTIME="$ROOT/mnt/app/root/carplay-altscreen"
SI="$ROOT/mnt/system/etc/eso/production/smartphone_integrator.json"
STARTUP=""
for startup in "$ROOT/mnt/system/etc/boot/startup.sh" "$ROOT/etc/boot/startup.sh"; do
    [ -f "$startup" ] && { STARTUP=$startup; break; }
done
problem=""
[ -f "$SD/state/INSTALLED" ] || problem="install marker missing"
[ -n "$problem" ] || [ -f "$RUNTIME/state/basevideo3.enabled" ] || problem="autostart flag missing"
[ -n "$problem" ] || [ -s "$ROOT/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar" ] || problem="HMI JAR missing"
[ -n "$problem" ] || [ -x "$RUNTIME/bin/mirror/carplay-alt111-mirror-display" ] || problem="display program missing"
[ -n "$problem" ] || grep -q 'libcarplay_altscreen.so' "$SI" 2>/dev/null || problem="CarPlay preload not set"
if [ -z "$problem" ]; then
    [ -n "$STARTUP" ] || problem="startup.sh not found"
fi
if [ -z "$problem" ]; then
    n=$(grep -c '^# BEGIN ALT111 BASEVIDEO3 AUTOSTART$' "$STARTUP" 2>/dev/null || true)
    [ "${n:-0}" = 1 ] || problem="boot autostart block missing"
fi
if [ -z "$problem" ]; then
    n=$(grep -c '^# BEGIN ALTSCREEN DIAGNOSTICS$' "$STARTUP" 2>/dev/null || true)
    [ "${n:-0}" = 1 ] || problem="boot diagnostics block missing"
fi
if [ -z "$problem" ]; then
    if [ "$ALTS_INSTALL_RGI_MODE" = WITH ]; then
        grep -q 'libcarplay_rgi_meta.so' "$SI" 2>/dev/null || problem="RGI preload not set"
        [ -n "$problem" ] || [ ! -e "$RUNTIME/state/rgi.disabled" ] || problem="RGI unexpectedly disabled"
    else
        ! grep -q 'libcarplay_rgi_meta.so' "$SI" 2>/dev/null || problem="RGI preload present in NO RGI mode"
        [ -n "$problem" ] || [ -f "$RUNTIME/state/rgi.disabled" ] || problem="RGI not disabled"
    fi
fi
if [ -n "$problem" ]; then
    alts_ui_log "FINAL_VERIFY=FAIL reason=$problem"
    fail_and_restore "Final check failed: $problem."
fi
alts_ui_log "FINAL_VERIFY=PASS mode=$ALTS_INSTALL_RGI_MODE"
ok

trap - 1 2 15
finish SUCCESS 0 \
    "$MODE_TEXT is installed and enabled." \
    "Next: fully reboot the head unit, then connect" \
    "CarPlay and start navigation." \
    "Keep this SD card: it holds the stock backup."
