#!/bin/sh
# V3.1 transactional INSTALL wrapper.
# Snapshot the exact pre-install production + SD control state, run the existing
# installer as APPLY, verify the final installed contract, and roll back on any
# handled failure.  A stale PREPARED transaction is rolled back before any new
# install is allowed.
set -u

ensure_dirs(){ for d in "$@"; do [ -d "$d" ] || mkdir -p "$d" || return 1; done; }

TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
ROOT=""; VOLUME=""; TXN_READY=0; ROLLING_BACK=0; APP_RW=0; SYS_RW=0
if [ "$TESTING" = 1 ]; then
  ROOT=${ALTSCREEN_CHAIN_ROOT:-}; VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
  case "$ROOT" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_ROOT" >&2; exit 2;; esac
  case "$VOLUME" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_VOLUME" >&2; exit 2;; esac
else
  for d in /net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1; do
    [ -d "$d/Toolbox" ] && { VOLUME=$d; break; }
  done
fi
[ -n "$VOLUME" ] && [ -d "$VOLUME/Toolbox" ] || { echo "INSTALL=REFUSED reason=SD_NOT_FOUND production_changed=NO"; exit 1; }
SD_RW_HELPER="$VOLUME/Toolbox/scripts/altscreen_sd_writable.sh"
[ -f "$SD_RW_HELPER" ] || { echo "INSTALL=REFUSED reason=SD_WRITABLE_HELPER_MISSING production_changed=NO"; exit 127; }
. "$SD_RW_HELPER"
altscreen_sd_ensure_writable "$VOLUME" INSTALL_TRANSACTION || { echo "INSTALL=REFUSED reason=SD_NOT_WRITABLE production_changed=NO"; exit 1; }

p(){ printf '%s%s\n' "$ROOT" "$1"; }
mount_app_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/app; }
mount_app_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/app; }
mount_system_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/system; }
mount_system_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/system; }

SD="$VOLUME/MMI-Cockpit-Carplay"
STATE="$SD/state"
BACKUP="$SD/backup"
STAGING="$SD/staging"
TXN_ROOT="$SD/install-transaction"
TXN="$TXN_ROOT/active"
LOG="$SD/logs/install-transaction.log"
RESTORE_TXN="$SD/restore-transaction/active"
INSTALLER="$VOLUME/Toolbox/scripts/install_mmi_cockpit_carplay_rx.sh"
CONTROLLER="$VOLUME/Toolbox/scripts/altscreen_chain_test.sh"
PRELOAD_AWK="$VOLUME/Toolbox/scripts/altscreen_preload.awk"
JAR_SOURCE="$VOLUME/Toolbox/carplay_alt_screen/hmi/carplay_hook-basevideo3.jar"
UNIVERSAL_SOURCE="$VOLUME/Toolbox/carplay_alt_screen/universal/libcarplay_altscreen.so"

RUNTIME="$(p /mnt/app/root/carplay-altscreen)"
RUNTIME_STAGE="$(p /mnt/app/root/.carplay-altscreen.new)"
RUNTIME_PREV="$(p /mnt/app/root/.carplay-altscreen.previous)"
LIBTARGET="$(p /mnt/app/root/lib-target)"
LEGACY_HOOK="$(p /mnt/app/root/hooks/libcarplay_altscreen.so)"
JAR="$(p /mnt/app/eso/hmi/lsd/jars/carplay_hook.jar)"
SI="$(p /mnt/system/etc/eso/production/smartphone_integrator.json)"
DIO="$(p /mnt/system/etc/eso/production/dio_manager.json)"
PF="$(p /mnt/system/etc/pf.conf)"
UNIVERSAL_REL="/mnt/app/root/carplay-altscreen/lib/libcarplay_altscreen.so"
UNIVERSAL_DST="$(p "$UNIVERSAL_REL")"
CHAIN_LOCK="$STATE/.chain_test.lock"

ensure_dirs "$SD/logs" "$TXN_ROOT" || { echo "INSTALL=REFUSED reason=SD_NOT_WRITABLE production_changed=NO"; exit 1; }
: >> "$LOG" 2>/dev/null || { echo "INSTALL=REFUSED reason=SD_LOG_NOT_WRITABLE production_changed=NO"; exit 1; }
exec 3>&1
exec >> "$LOG" 2>&1
log(){ echo "$*"; echo "$*" >&3; }

size(){ n=$(wc -c < "$1" 2>/dev/null || echo 0); set -- $n; echo "${1:-0}"; }
same(){ [ -f "$1" ] && [ -f "$2" ] && [ "$(size "$1")" = "$(size "$2")" ] && [ "$(cksum < "$1")" = "$(cksum < "$2")" ]; }
find_startup(){ for f in "$(p /mnt/system/etc/boot/startup.sh)" "$(p /etc/boot/startup.sh)"; do [ -f "$f" ] && { echo "$f"; return 0; }; done; return 1; }

dir_manifest(){
  src=$1; out=$2
  if [ ! -d "$src" ]; then : > "$out"; return 0; fi
  (
    cd "$src" || exit 1
    find . -type f -print 2>/dev/null | sort | while IFS= read -r rel; do
      [ -f "$rel" ] || continue
      c=$(cksum < "$rel") || exit 1
      printf '%s|%s\n' "$rel" "$c"
    done
  ) > "$out"
}
same_dir_exact(){
  a=$1; b=$2; ta="$TXN/.manifest_a.$$"; tb="$TXN/.manifest_b.$$"
  dir_manifest "$a" "$ta" && dir_manifest "$b" "$tb" && cmp -s "$ta" "$tb"
  rc=$?
  rm -f "$ta" "$tb" 2>/dev/null || true
  return "$rc"
}

finish_mounts(){
  r=0
  sync >/dev/null 2>&1 || r=1
  [ "$APP_RW" = 0 ] || { mount_app_ro >/dev/null 2>&1 || r=1; APP_RW=0; }
  [ "$SYS_RW" = 0 ] || { mount_system_ro >/dev/null 2>&1 || r=1; SYS_RW=0; }
  return "$r"
}

snapshot_marker_kind(){
  base=$1
  p=0; a=0
  [ ! -f "$base.present" ] || p=1
  [ ! -f "$base.absent" ] || a=1
  [ $((p + a)) -eq 1 ] || return 1
  [ "$p" = 1 ] && echo present || echo absent
}

snap_file(){
  src=$1; name=$2; dst="$TXN/files/$name"
  if [ -f "$src" ]; then
    cp "$src" "$dst" && same "$src" "$dst" || return 1
    cksum < "$dst" > "$TXN/meta/$name.cksum" || return 1
    touch "$dst.present"
  else
    touch "$dst.absent"
  fi
}
validate_file_snapshot(){
  name=$1; src="$TXN/files/$name"
  kind=$(snapshot_marker_kind "$src") || return 1
  if [ "$kind" = present ]; then
    [ -f "$src" ] || return 1
    if [ -f "$TXN/meta/$name.cksum" ]; then
      [ "$(cksum < "$src")" = "$(cat "$TXN/meta/$name.cksum")" ] || return 1
    else
      [ "$(cat "$TXN/FORMAT" 2>/dev/null || true)" != 2 ] || return 1
      log "SNAPSHOT_INTEGRITY=LEGACY_WEAK kind=file name=$name reason=missing_cksum_metadata"
    fi
  else
    [ ! -e "$src" ] || return 1
  fi
  return 0
}
restore_file(){
  dst=$1; name=$2; mode=$3; src="$TXN/files/$name"
  kind=$(snapshot_marker_kind "$src") || return 1
  if [ "$kind" = present ]; then
    validate_file_snapshot "$name" || return 1
    ensure_dirs "$(dirname -- "$dst")" || return 1
    tmp="${dst}.install-rollback.new"
    rm -f "$tmp" 2>/dev/null || true
    cp "$src" "$tmp" && chmod "$mode" "$tmp" && same "$src" "$tmp" && mv "$tmp" "$dst"
  else
    rm -f "$dst"
  fi
}
verify_file_snapshot(){
  dst=$1; name=$2; src="$TXN/files/$name"
  kind=$(snapshot_marker_kind "$src") || return 1
  if [ "$kind" = present ]; then same "$src" "$dst"
  else [ ! -e "$dst" ]
  fi
}

snap_dir(){
  src=$1; name=$2; dst="$TXN/dirs/$name"
  if [ -d "$src" ]; then
    ensure_dirs "$dst" || return 1
    cp -R "$src/." "$dst/" || return 1
    same_dir_exact "$src" "$dst" || return 1
    dir_manifest "$dst" "$TXN/meta/$name.manifest" || return 1
    touch "$TXN/dirs/$name.present"
  else
    touch "$TXN/dirs/$name.absent"
  fi
}
validate_dir_snapshot(){
  name=$1; src="$TXN/dirs/$name"
  kind=$(snapshot_marker_kind "$TXN/dirs/$name") || return 1
  if [ "$kind" = present ]; then
    [ -d "$src" ] || return 1
    if [ -f "$TXN/meta/$name.manifest" ]; then
      now="$TXN/meta/$name.verify.current"
      dir_manifest "$src" "$now" || { rm -f "$now" 2>/dev/null || true; return 1; }
      cmp -s "$TXN/meta/$name.manifest" "$now" || { rm -f "$now" 2>/dev/null || true; return 1; }
      rm -f "$now" 2>/dev/null || true
    else
      [ "$(cat "$TXN/FORMAT" 2>/dev/null || true)" != 2 ] || return 1
      log "SNAPSHOT_INTEGRITY=LEGACY_WEAK kind=dir name=$name reason=missing_manifest_metadata"
    fi
  else
    [ ! -e "$src" ] || return 1
  fi
  return 0
}
restore_dir(){
  dst=$1; name=$2; src="$TXN/dirs/$name"
  kind=$(snapshot_marker_kind "$TXN/dirs/$name") || return 1
  if [ "$kind" = present ]; then
    validate_dir_snapshot "$name" || return 1
    tmp="${dst}.install-rollback.new"
    rm -rf "$tmp" 2>/dev/null || true
    ensure_dirs "$tmp" || return 1
    cp -R "$src/." "$tmp/" || return 1
    same_dir_exact "$src" "$tmp" || { rm -rf "$tmp" 2>/dev/null || true; return 1; }
    rm -rf "$dst" 2>/dev/null || { rm -rf "$tmp" 2>/dev/null || true; return 1; }
    mv "$tmp" "$dst"
  else
    rm -rf "$dst" 2>/dev/null || return 1
  fi
}
verify_dir_snapshot(){
  dst=$1; name=$2; src="$TXN/dirs/$name"
  kind=$(snapshot_marker_kind "$TXN/dirs/$name") || return 1
  if [ "$kind" = present ]; then [ -d "$dst" ] && same_dir_exact "$src" "$dst"
  else [ ! -e "$dst" ]
  fi
}

validate_snapshot(){
  s=$(cat "$TXN/startup.path" 2>/dev/null || true)
  case "$s" in
    "$(p /mnt/system/etc/boot/startup.sh)"|"$(p /etc/boot/startup.sh)") ;;
    *) log "SNAPSHOT_INTEGRITY=FAIL reason=invalid_startup_path"; return 1 ;;
  esac
  for n in startup.sh smartphone_integrator.json dio_manager.json pf.conf carplay_hook.jar legacy_hook; do
    validate_file_snapshot "$n" || { log "SNAPSHOT_INTEGRITY=FAIL kind=file name=$n"; return 1; }
  done
  for n in runtime runtime_stage runtime_previous libtarget sd_state sd_backup sd_staging; do
    validate_dir_snapshot "$n" || { log "SNAPSHOT_INTEGRITY=FAIL kind=dir name=$n"; return 1; }
  done
  log "SNAPSHOT_INTEGRITY=PASS"
  return 0
}

chain_lock_precheck(){
  [ -d "$CHAIN_LOCK" ] || return 0
  if [ ! -f "$CHAIN_LOCK/pid" ]; then
    sleep 1
    if [ ! -f "$CHAIN_LOCK/pid" ]; then
      rm -f "$CHAIN_LOCK/owner" "$CHAIN_LOCK/boot" "$CHAIN_LOCK/action" 2>/dev/null || true
      rmdir "$CHAIN_LOCK" 2>/dev/null || {
        log "INSTALL=REFUSED reason=CHAIN_LOCK_INVALID production_changed=NO"
        return 1
      }
      log "CHAIN_LOCK_STALE_RECOVERED reason=empty_or_partial"
      return 0
    fi
  fi
  owner=$(cat "$CHAIN_LOCK/owner" 2>/dev/null || true)
  case "$owner" in ""|MMI-Cockpit-Carplay-Universal) ;; *)
    log "INSTALL=REFUSED reason=CHAIN_LOCK_UNKNOWN_OWNER owner=$owner production_changed=NO"
    return 1 ;;
  esac
  pid=$(cat "$CHAIN_LOCK/pid" 2>/dev/null || true)
  case "$pid" in
    ''|*[!0-9]*) reason=invalid_pid ;;
    *)
      if kill -0 "$pid" 2>/dev/null; then
        log "INSTALL=REFUSED reason=CHAIN_OPERATION_ACTIVE pid=$pid production_changed=NO"
        return 1
      fi
      reason=dead_pid
      ;;
  esac
  rm -f "$CHAIN_LOCK/owner" "$CHAIN_LOCK/pid" "$CHAIN_LOCK/boot" "$CHAIN_LOCK/action" 2>/dev/null || return 1
  rmdir "$CHAIN_LOCK" 2>/dev/null || return 1
  log "CHAIN_LOCK_STALE_RECOVERED reason=$reason old_pid=${pid:-unknown}"
  return 0
}

snapshot(){
  [ ! -e "$RESTORE_TXN" ] || { log "INSTALL=REFUSED reason=RESTORE_TRANSACTION_ACTIVE production_changed=NO"; return 1; }
  chain_lock_precheck || return 1
  rm -rf "$TXN" 2>/dev/null || return 1
  ensure_dirs "$TXN/files" "$TXN/dirs" "$TXN/meta" || return 1
  printf '%s\n' 2 > "$TXN/FORMAT" || return 1

  STARTUP=$(find_startup) || { log "INSTALL=REFUSED reason=STARTUP_NOT_FOUND production_changed=NO"; return 1; }
  printf '%s\n' "$STARTUP" > "$TXN/startup.path" || return 1

  snap_file "$STARTUP" startup.sh || return 1
  snap_file "$SI" smartphone_integrator.json || return 1
  snap_file "$DIO" dio_manager.json || return 1
  snap_file "$PF" pf.conf || return 1
  snap_file "$JAR" carplay_hook.jar || return 1
  snap_file "$LEGACY_HOOK" legacy_hook || return 1

  snap_dir "$RUNTIME" runtime || return 1
  snap_dir "$RUNTIME_STAGE" runtime_stage || return 1
  snap_dir "$RUNTIME_PREV" runtime_previous || return 1
  snap_dir "$LIBTARGET" libtarget || return 1
  snap_dir "$STATE" sd_state || return 1
  snap_dir "$BACKUP" sd_backup || return 1
  snap_dir "$STAGING" sd_staging || return 1

  printf '%s\n' "prepared" > "$TXN/pid" || return 1
  validate_snapshot || return 1
  touch "$TXN/PREPARED" || return 1
  # PREPARED must be durable before the first production mutation. Otherwise a
  # power loss could leave changed production state with no recoverable marker.
  sync >/dev/null 2>&1 || {
    rm -f "$TXN/PREPARED" 2>/dev/null || true
    log "INSTALL=REFUSED reason=PREPARED_SYNC_FAILED production_changed=NO"
    return 1
  }
  TXN_READY=1
  log "INSTALL_TRANSACTION=PREPARED persistent_state=PRE_INSTALL durable=YES"
}

verify_preinstall(){
  s=$(cat "$TXN/startup.path" 2>/dev/null || true)
  [ -n "$s" ] || return 1
  verify_file_snapshot "$s" startup.sh || return 1
  verify_file_snapshot "$SI" smartphone_integrator.json || return 1
  verify_file_snapshot "$DIO" dio_manager.json || return 1
  verify_file_snapshot "$PF" pf.conf || return 1
  verify_file_snapshot "$JAR" carplay_hook.jar || return 1
  verify_file_snapshot "$LEGACY_HOOK" legacy_hook || return 1
  verify_dir_snapshot "$RUNTIME" runtime || return 1
  verify_dir_snapshot "$RUNTIME_STAGE" runtime_stage || return 1
  verify_dir_snapshot "$RUNTIME_PREV" runtime_previous || return 1
  verify_dir_snapshot "$LIBTARGET" libtarget || return 1
  verify_dir_snapshot "$STATE" sd_state || return 1
  verify_dir_snapshot "$BACKUP" sd_backup || return 1
  verify_dir_snapshot "$STAGING" sd_staging || return 1
  return 0
}

rollback(){
  [ -f "$TXN/PREPARED" ] || return 1
  # A failed commit sync must never leave a terminal success marker while
  # rollback is attempted. Invalidate it durably before changing any files.
  if [ -f "$TXN/COMMITTED" ]; then
    touch "$TXN/ROLLBACK_INCOMPLETE" || return 1
    rm -f "$TXN/COMMITTED" && sync || {
      log "INSTALL_ROLLBACK=REFUSED reason=COMMIT_INVALIDATION_FAILED recovery_required=YES"
      return 1
    }
  fi
  if ! validate_snapshot; then
    touch "$TXN/ROLLBACK_INCOMPLETE" 2>/dev/null || true
    log "INSTALL_ROLLBACK=REFUSED reason=SNAPSHOT_INTEGRITY_FAILED production_changed=NO_BY_ROLLBACK recovery_required=YES"
    return 1
  fi
  ROLLING_BACK=1
  trap - 1 2 15
  log "INSTALL_ROLLBACK=STARTED target=PRE_INSTALL"
  r=0

  mount_system_rw >/dev/null 2>&1 && SYS_RW=1 || r=1
  mount_app_rw >/dev/null 2>&1 && APP_RW=1 || r=1

  if [ "$SYS_RW" = 1 ]; then
    s=$(cat "$TXN/startup.path" 2>/dev/null || true)
    [ -n "$s" ] && restore_file "$s" startup.sh 755 || r=1
    restore_file "$SI" smartphone_integrator.json 644 || r=1
    restore_file "$DIO" dio_manager.json 644 || r=1
    restore_file "$PF" pf.conf 644 || r=1
  fi

  if [ "$APP_RW" = 1 ]; then
    restore_file "$JAR" carplay_hook.jar 644 || r=1
    restore_file "$LEGACY_HOOK" legacy_hook 755 || r=1
    restore_dir "$RUNTIME" runtime || r=1
    restore_dir "$RUNTIME_STAGE" runtime_stage || r=1
    restore_dir "$RUNTIME_PREV" runtime_previous || r=1
    restore_dir "$LIBTARGET" libtarget || r=1
  fi

  finish_mounts || r=1
  restore_dir "$STATE" sd_state || r=1
  restore_dir "$BACKUP" sd_backup || r=1
  restore_dir "$STAGING" sd_staging || r=1
  sync >/dev/null 2>&1 || r=1

  if [ "$r" = 0 ] && verify_preinstall; then
    if rm -f "$TXN/APPLYING" "$TXN/ROLLBACK_INCOMPLETE" &&
       touch "$TXN/ROLLED_BACK" && sync; then
      log "INSTALL_ROLLBACK_VERIFY=PASS"
      log "INSTALL_ROLLBACK=PASS persistent_state=PRE_INSTALL reboot_required=YES"
      ROLLING_BACK=0
      return 0
    fi
  fi

  rm -f "$TXN/ROLLED_BACK" 2>/dev/null || true
  touch "$TXN/ROLLBACK_INCOMPLETE" 2>/dev/null || true
  log "INSTALL_ROLLBACK=FAIL recovery_required=YES transaction_retained=$TXN"
  ROLLING_BACK=0
  return 1
}

recover_stale(){
  [ -d "$TXN" ] || { log "STALE_INSTALL_TRANSACTION=ABSENT"; return 0; }
  if [ ! -f "$TXN/ROLLBACK_INCOMPLETE" ] &&
     { [ -f "$TXN/COMMITTED" ] || [ -f "$TXN/ROLLED_BACK" ]; }; then
    log "STALE_INSTALL_TRANSACTION=TERMINAL action=CLEANUP"
    rm -rf "$TXN" || return 1
    return 0
  fi
  if [ -f "$TXN/PREPARED" ]; then
    log "STALE_INSTALL_TRANSACTION=DETECTED action=ROLLBACK_PRE_INSTALL"
    TXN_READY=1
    rollback || return 1
    rm -rf "$TXN" || return 1
    TXN_READY=0
    log "STALE_INSTALL_TRANSACTION=RECOVERED persistent_state=PRE_INSTALL"
    return 0
  fi
  log "STALE_INSTALL_TRANSACTION=INCOMPLETE_PREPARE action=CLEANUP production_changed=NO"
  rm -rf "$TXN" || return 1
  return 0
}

verify_boot_backup(){
  b="$BACKUP/boot-diagnostics"
  [ -f "$b/COMPLETE" ] && [ -s "$b/startup.sh" ] && [ -f "$b/startup.cksum" ] && [ -f "$b/path" ] || return 1
  [ "$(cksum < "$b/startup.sh")" = "$(cat "$b/startup.cksum")" ] || return 1
  case "$(cat "$b/path" 2>/dev/null || true)" in
    /mnt/system/etc/boot/startup.sh|/etc/boot/startup.sh) return 0 ;;
    *) return 1 ;;
  esac
}

verify_installed(){
  [ -s "$JAR_SOURCE" ] && [ -s "$UNIVERSAL_SOURCE" ] ||
    { log "INSTALL_VERIFY=FAIL reason=PACKAGE_ARTIFACT_MISSING"; return 1; }
  same "$JAR_SOURCE" "$JAR" ||
    { log "INSTALL_VERIFY=FAIL reason=HMI_JAR_MISMATCH"; return 1; }
  same "$VOLUME/Toolbox/carplay_alt_screen/hmi/BUILD_INFO.txt" "$RUNTIME/state/hmi-build-info.txt" ||
    { log "INSTALL_VERIFY=FAIL reason=HMI_IDENTITY_METADATA_MISMATCH"; return 1; }
  case "${ALTS_INSTALL_RGI_MODE:-WITH}" in
    NO)
      [ -f "$RUNTIME/state/rgi.disabled" ] || { log "INSTALL_VERIFY=FAIL reason=NO_RGI_MARKER_MISSING"; return 1; }
      if awk -v query=/mnt/app/root/carplay-altscreen/lib/libcarplay_rgi_meta.so -f "$PRELOAD_AWK" "$SI" >/dev/null 2>&1; then
        log "INSTALL_VERIFY=FAIL reason=NO_RGI_PRELOAD_PRESENT"; return 1
      fi
      ;;
    WITH)
      [ ! -e "$RUNTIME/state/rgi.disabled" ] || { log "INSTALL_VERIFY=FAIL reason=WITH_RGI_DISABLED"; return 1; }
      awk -v query=/mnt/app/root/carplay-altscreen/lib/libcarplay_rgi_meta.so -f "$PRELOAD_AWK" "$SI" >/dev/null 2>&1 ||
        { log "INSTALL_VERIFY=FAIL reason=WITH_RGI_PRELOAD_MISSING"; return 1; }
      ;;
    *) log "INSTALL_VERIFY=FAIL reason=INVALID_RGI_MODE"; return 1 ;;
  esac
  same "$UNIVERSAL_SOURCE" "$UNIVERSAL_DST" ||
    { log "INSTALL_VERIFY=FAIL reason=UNIVERSAL_HOOK_MISMATCH"; return 1; }
  same "$VOLUME/Toolbox/carplay_alt_screen/rgi_meta/libcarplay_rgi_meta.so" "$RUNTIME/lib/libcarplay_rgi_meta.so" ||
    { log "INSTALL_VERIFY=FAIL reason=RGI_METADATA_MISMATCH"; return 1; }
  for rgi_file in maneuver_render flag_atlas.rgba BUILD_INFO.txt SHA256SUMS; do
    same "$VOLUME/Toolbox/carplay_alt_screen/rgi_renderer/release/$rgi_file" "$RUNTIME/bin/rgi/$rgi_file" ||
      { log "INSTALL_VERIFY=FAIL reason=RGI_RUNTIME_MISMATCH file=$rgi_file"; return 1; }
  done
  [ -x "$RUNTIME/bin/rgi/maneuver_render" ] ||
    { log "INSTALL_VERIFY=FAIL reason=RGI_NOT_EXECUTABLE"; return 1; }

  [ -f "$RUNTIME/.mmi-cockpit-carplay-runtime-owner" ] ||
    { log "INSTALL_VERIFY=FAIL reason=RUNTIME_OWNER_MISSING"; return 1; }
  [ -x "$RUNTIME/bin/mirror/carplay-alt111-mirror-display" ] ||
    { log "INSTALL_VERIFY=FAIL reason=MIRROR_BINARY_MISSING"; return 1; }
  [ -x "$RUNTIME/bin/mirror/start_vehicle.sh" ] ||
    { log "INSTALL_VERIFY=FAIL reason=MIRROR_START_MISSING"; return 1; }
  same "$VOLUME/Toolbox/carplay_alt_screen/mirror_display/release/rgi_supervisor.sh" "$RUNTIME/bin/mirror/rgi_supervisor.sh" ||
    { log "INSTALL_VERIFY=FAIL reason=RGI_SUPERVISOR_MISMATCH"; return 1; }
  [ -x "$RUNTIME/bin/mirror/rgi_supervisor.sh" ] ||
    { log "INSTALL_VERIFY=FAIL reason=RGI_SUPERVISOR_NOT_EXECUTABLE"; return 1; }
  [ -f "$RUNTIME/state/diagnostics.enabled" ] ||
    { log "INSTALL_VERIFY=FAIL reason=PERSISTENT_DIAGNOSTICS_NOT_ENABLED"; return 1; }

  [ -f "$STATE/INSTALLED" ] ||
    { log "INSTALL_VERIFY=FAIL reason=INSTALLED_MARKER_MISSING"; return 1; }
  [ "$(cat "$STATE/firmware_profile.txt" 2>/dev/null || true)" = UNIVERSAL ] ||
    { log "INSTALL_VERIFY=FAIL reason=FIRMWARE_PROFILE_NOT_UNIVERSAL"; return 1; }
  [ ! -e "$STATE/RESTORE_PENDING_REBOOT" ] ||
    { log "INSTALL_VERIFY=FAIL reason=STALE_RESTORE_PENDING_REBOOT"; return 1; }

  [ ! -e "$RUNTIME_STAGE" ] ||
    { log "INSTALL_VERIFY=FAIL reason=RUNTIME_STAGE_RESIDUE"; return 1; }
  [ ! -e "$RUNTIME_PREV" ] ||
    { log "INSTALL_VERIFY=FAIL reason=RUNTIME_PREVIOUS_RESIDUE"; return 1; }

  awk -v query="$UNIVERSAL_REL" -f "$PRELOAD_AWK" "$SI" >/dev/null 2>&1 ||
    { log "INSTALL_VERIFY=FAIL reason=SMARTPHONE_INTEGRATOR_NOT_ARMED"; return 1; }

  STARTUP=$(find_startup) ||
    { log "INSTALL_VERIFY=FAIL reason=STARTUP_NOT_FOUND"; return 1; }
  b=$(grep -c '^# BEGIN ALTSCREEN DIAGNOSTICS$' "$STARTUP" 2>/dev/null || true)
  e=$(grep -c '^# END ALTSCREEN DIAGNOSTICS$' "$STARTUP" 2>/dev/null || true)
  [ -n "$b" ] || b=0
  [ -n "$e" ] || e=0
  [ "$b" = 1 ] && [ "$e" = 1 ] ||
    { log "INSTALL_VERIFY=FAIL reason=DIAGNOSTICS_BLOCK_COUNT begin=$b end=$e"; return 1; }

  # The uninstall path depends on the native and boot recovery sets. The HMI
  # JAR is project-owned and is covered only by the INSTALL transaction snapshot.
  [ -f "$CONTROLLER" ] ||
    { log "INSTALL_VERIFY=FAIL reason=CONTROLLER_MISSING"; return 1; }
  ALTS_INSTALL_TXN_ACTIVE=1 /bin/sh "$CONTROLLER" restore-precheck ||
    { log "INSTALL_VERIFY=FAIL reason=NATIVE_RECOVERY_SET_INVALID"; return 1; }
  verify_boot_backup ||
    { log "INSTALL_VERIFY=FAIL reason=BOOT_DIAGNOSTICS_BACKUP_INVALID"; return 1; }

  log "INSTALL_VERIFY=PASS runtime=owned jar=project_owned_verified native_preload=verified diagnostics=verified sd_state=verified recovery_set=verified"
  return 0
}
fail(){
  msg=$1
  log "ERROR: $msg"
  finish_mounts >/dev/null 2>&1 || true
  if [ "$ROLLING_BACK" = 0 ] && [ "$TXN_READY" = 1 ]; then
    if rollback; then
      log "INSTALL=ABORTED rollback=PASS persistent_state=PRE_INSTALL"
    else
      log "INSTALL=FAILED rollback=INCOMPLETE recovery_required=YES"
    fi
  else
    log "INSTALL=REFUSED production_changed=NO"
  fi
  exit 1
}

ACTION=${1:-install}
case "$ACTION" in
  install|recover) ;;
  *) echo "usage: $0 {install|recover} [installer-args...]" >&2; exit 2 ;;
esac
if [ "$ACTION" = recover ]; then
  log "===== V3.1 transactional INSTALL recovery started ====="
  recover_stale || { log "INSTALL_RECOVERY=FAIL"; exit 1; }
  log "INSTALL_RECOVERY=PASS"
  log "===== V3.1 transactional INSTALL recovery finished ====="
  exit 0
fi

trap 'fail "install interrupted by signal"' 1 2 15
log "===== V3.1 transactional INSTALL started ====="
recover_stale || fail "previous install transaction could not be recovered"
[ ! -e "$RESTORE_TXN" ] || fail "restore transaction is active"
[ -f "$INSTALLER" ] || fail "installer missing"
sh -n "$INSTALLER" || fail "installer shell syntax invalid"

snapshot || { rm -rf "$TXN" 2>/dev/null || true; TXN_READY=0; fail "pre-install transaction snapshot failed"; }
touch "$TXN/APPLYING" || fail "cannot mark install transaction APPLYING"

if [ "$#" -gt 0 ]; then shift; fi
if [ "$#" -gt 0 ]; then
  ALTS_INSTALL_TXN_ACTIVE=1 ALTS_OPLOG_CAPTURED=1 /bin/sh "$INSTALLER" "$@" || fail "install APPLY step failed"
else
  ALTS_INSTALL_TXN_ACTIVE=1 ALTS_OPLOG_CAPTURED=1 /bin/sh "$INSTALLER" || fail "install APPLY step failed"
fi

verify_installed || fail "final installed-state verification failed"
sync >/dev/null 2>&1 || fail "sync failed before install commit"
touch "$TXN/COMMITTED" || fail "cannot commit install transaction"
sync >/dev/null 2>&1 || fail "cannot durably commit install transaction"
TXN_READY=0
log "INSTALL=PASS transaction=COMMITTED persistent_state=INSTALLED reboot_required=YES"
rm -rf "$TXN" 2>/dev/null || log "WARN: committed install transaction retained; next INSTALL/RESTORE will clean it"
sync >/dev/null 2>&1 || log "WARN: committed transaction cleanup was not durably synced; terminal COMMITTED residue is non-blocking"
trap - 1 2 15
log "===== V3.1 transactional INSTALL finished ====="
exit 0
