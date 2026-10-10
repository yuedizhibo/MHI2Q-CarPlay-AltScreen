#!/bin/sh
# V3 transactional RESTORE ORIGINAL wrapper.
# Preflight first, snapshot the complete project-owned persistent state, run the
# proven restore APPLY step, verify, and roll back on any handled failure.
set -u

ensure_dirs(){ for d in "$@"; do [ -d "$d" ] || mkdir -p "$d" || return 1; done; }

SD_CANDIDATES="/net/mmx/fs/sda0 /net/mmx/fs/sda1 /net/mmx/fs/sdb0 /net/mmx/fs/sdb1 /fs/sda0 /fs/sda1 /fs/sdb0 /fs/sdb1"

sd_probe_write(){
  base=$1; label=$2
  [ -d "$base" ] || { echo "SD_WRITE_PROBE scope=$label path=$base result=SKIP reason=DIR_ABSENT"; return 0; }
  probe="$base/.altscreen-rw-probe.$$"
  err="/tmp/altscreen_sd_probe_$$.err"
  rm -f "$err" "$probe" 2>/dev/null || true
  if ( umask 077; printf '%s\n' "altscreen-write-probe" > "$probe" ) 2>"$err"; then
    rm -f "$probe" 2>/dev/null || true
    echo "SD_WRITE_PROBE scope=$label path=$base result=PASS"
  else
    msg=$(sed -n '1p' "$err" 2>/dev/null || true)
    rm -f "$probe" "$err" 2>/dev/null || true
    [ -n "$msg" ] || msg=write_failed_without_stderr
    echo "SD_WRITE_PROBE scope=$label path=$base result=FAIL error=$msg"
    return 1
  fi
  rm -f "$err" 2>/dev/null || true
  return 0
}

emit_sd_diagnostics(){
  echo "SD_DIAGNOSTICS_BEGIN selected=${VOLUME:-none}"
  if command -v mount >/dev/null 2>&1; then
    echo "SD_MOUNT_TABLE_BEGIN"
    mount 2>&1 || true
    echo "SD_MOUNT_TABLE_END"
  else
    echo "SD_MOUNT_TABLE=UNAVAILABLE"
  fi
  if command -v df >/dev/null 2>&1; then
    echo "SD_DF_BEGIN"
    df 2>&1 || true
    echo "SD_DF_END"
  else
    echo "SD_DF=UNAVAILABLE"
  fi
  for cand in $SD_CANDIDATES; do
    if [ ! -d "$cand" ]; then
      echo "SD_CANDIDATE path=$cand present=NO"
      continue
    fi
    toolbox=NO; project=NO
    [ ! -d "$cand/Toolbox" ] || toolbox=YES
    [ ! -d "$cand/MMI-Cockpit-Carplay" ] || project=YES
    echo "SD_CANDIDATE path=$cand present=YES toolbox=$toolbox project=$project"
    ls -ld "$cand" "$cand/Toolbox" "$cand/MMI-Cockpit-Carplay" 2>/dev/null |
      while IFS= read -r line; do echo "SD_PATH_META path=$cand line=$line"; done
    if command -v mount >/dev/null 2>&1; then
      mount 2>/dev/null | grep -F "$cand" |
        while IFS= read -r line; do echo "SD_MOUNT_MATCH path=$cand line=$line"; done
    fi
    if [ "$toolbox" = YES ]; then
      sd_probe_write "$cand" ROOT || true
      sd_probe_write "$cand/MMI-Cockpit-Carplay" PROJECT || true
    fi
  done
  echo "SD_FORMAT_HINT=FAT32_MBR_SINGLE_PRIMARY recommended_for_MHI2_toolbox; avoid_NTFS_or_exFAT_for_recovery_media"
  echo "SD_DIAGNOSTICS_END"
}

TESTING=${ALTSCREEN_CHAIN_TESTING:-0}
ROOT=""; VOLUME=""; TXN_READY=0; ROLLING_BACK=0; APP_RW=0; SYS_RW=0; MIXED_RECOVERY=0; PRE_RECOVERY_CHANGED=0
if [ "$TESTING" = 1 ]; then
  ROOT=${ALTSCREEN_CHAIN_ROOT:-}; VOLUME=${ALTSCREEN_CHAIN_VOLUME:-}
  case "$ROOT" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_ROOT" >&2; exit 2;; esac
  case "$VOLUME" in /tmp/*|/var/tmp/*) ;; *) echo "FAIL: invalid ALTSCREEN_CHAIN_VOLUME" >&2; exit 2;; esac
else
  for d in $SD_CANDIDATES; do
    [ -d "$d/Toolbox" ] && { VOLUME=$d; break; }
  done
fi
[ -n "$VOLUME" ] && [ -d "$VOLUME/Toolbox" ] || { echo "RESTORE=REFUSED reason=SD_NOT_FOUND production_changed=NO"; exit 1; }
SD_RW_HELPER="$VOLUME/Toolbox/scripts/altscreen_sd_writable.sh"
[ -f "$SD_RW_HELPER" ] || { echo "RESTORE=REFUSED reason=SD_WRITABLE_HELPER_MISSING production_changed=NO"; exit 127; }
. "$SD_RW_HELPER"
if ! altscreen_sd_ensure_writable "$VOLUME" RESTORE_TRANSACTION; then
  echo "RESTORE=REFUSED reason=SD_NOT_WRITABLE production_changed=NO"
  emit_sd_diagnostics
  exit 1
fi
p(){ printf '%s%s\n' "$ROOT" "$1"; }
mount_app_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/app; }
mount_app_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/app; }
mount_system_rw(){ [ "$TESTING" = 1 ] || mount -uw /mnt/system; }
mount_system_ro(){ [ "$TESTING" = 1 ] || mount -ur /mnt/system; }

SD="$VOLUME/MMI-Cockpit-Carplay"; STATE="$SD/state"; BACKUP="$SD/backup"
TXN="$SD/restore-transaction/active"; LOG="$SD/logs/restore-transaction.log"
CONTROLLER="$VOLUME/Toolbox/scripts/altscreen_chain_test.sh"
APPLY="$VOLUME/Toolbox/scripts/altscreen_restore_apply.sh"
INSTALL_TXN_HELPER="$VOLUME/Toolbox/scripts/altscreen_install_transaction.sh"
INSTALL_TXN="$SD/install-transaction/active"
NATIVE="$BACKUP/original"
RUNTIME="$(p /mnt/app/root/carplay-altscreen)"; STAGE="$(p /mnt/app/root/.carplay-altscreen.new)"; PREV="$(p /mnt/app/root/.carplay-altscreen.previous)"
SI="$(p /mnt/system/etc/eso/production/smartphone_integrator.json)"; DIO="$(p /mnt/system/etc/eso/production/dio_manager.json)"; PF="$(p /mnt/system/etc/pf.conf)"
JAR="$(p /mnt/app/eso/hmi/lsd/jars/carplay_hook.jar)"; LEGACY_HOOK="$(p /mnt/app/root/hooks/libcarplay_altscreen.so)"; LIBTARGET="$(p /mnt/app/root/lib-target)"
if ! ensure_dirs "$SD/logs" "$SD/restore-transaction"; then
  echo "RESTORE=REFUSED reason=SD_NOT_WRITABLE production_changed=NO"
  emit_sd_diagnostics
  exit 1
fi
if ! (: >> "$LOG") 2>/dev/null; then
  echo "RESTORE=REFUSED reason=SD_LOG_NOT_WRITABLE production_changed=NO"
  emit_sd_diagnostics
  exit 1
fi
exec 3>&1; exec >> "$LOG" 2>&1
log(){ echo "$*"; echo "$*" >&3; }

size(){ n=$(wc -c < "$1" 2>/dev/null || echo 0); set -- $n; echo "${1:-0}"; }
same(){ [ -f "$1" ] && [ -f "$2" ] && [ "$(size "$1")" = "$(size "$2")" ] && [ "$(cksum < "$1")" = "$(cksum < "$2")" ]; }
find_startup(){ for f in "$(p /mnt/system/etc/boot/startup.sh)" "$(p /etc/boot/startup.sh)"; do [ -f "$f" ] && { echo "$f"; return 0; }; done; return 1; }


strip_startup_blocks_preflight(){
  awk '
    {
      key=$0
      sub(/\r$/, "", key)
      trimmed=key
      gsub(/^[ \t]+/, "", trimmed)
      gsub(/[ \t]+$/, "", trimmed)
      if (trimmed == "# BEGIN ALT111 MIRROR AUTOSTART") {
        if (block != "") bad=8
        block="old"; next
      }
      if (trimmed == "# END ALT111 MIRROR AUTOSTART") {
        if (block != "old") bad=8
        block=""; next
      }
      if (trimmed == "# BEGIN ALT111 BASEVIDEO3 AUTOSTART") {
        if (block != "") bad=8
        block="new"; next
      }
      if (trimmed == "# END ALT111 BASEVIDEO3 AUTOSTART") {
        if (block != "new") bad=8
        block=""; next
      }
      if (block == "") print
    }
    END {
      if (bad) exit bad
      if (block != "") exit 9
    }
  ' "$1"
}

preflight_startup(){
  startup=$(find_startup) || {
    log "RESTORE_PREFLIGHT_STARTUP=FAIL reason=STARTUP_NOT_FOUND production_changed=NO"
    return 1
  }
  tmp="$SD/restore-transaction/.startup-preflight.tmp"
  rm -f "$tmp" 2>/dev/null || true
  if ! strip_startup_blocks_preflight "$startup" > "$tmp"; then
    rm -f "$tmp" 2>/dev/null || true
    log "RESTORE_PREFLIGHT_STARTUP=FAIL reason=AUTOSTART_BLOCK_INVALID production_changed=NO"
    return 1
  fi
  if ! sh -n "$tmp" >/dev/null 2>&1; then
    rm -f "$tmp" 2>/dev/null || true
    log "RESTORE_PREFLIGHT_STARTUP=FAIL reason=CLEANED_STARTUP_SYNTAX_INVALID production_changed=NO"
    return 1
  fi
  rm -f "$tmp" 2>/dev/null || true
  log "RESTORE_PREFLIGHT_STARTUP=PASS production_changed=NO"
  return 0
}

dir_manifest(){
  src=$1; out=$2
  if [ ! -d "$src" ]; then : > "$out"; return 0; fi
  (
    cd "$src" || exit 1
    find . -type f -print 2>/dev/null | sort | while IFS= read -r rel; do
      [ -f "$rel" ] || continue
      sum=$(cksum < "$rel") || exit 1
      printf '%s|%s\n' "$rel" "$sum"
    done
  ) > "$out"
}
same_dir_exact(){
  a=$1; b=$2
  ma="$TXN/.restore_manifest_a"; mb="$TXN/.restore_manifest_b"
  dir_manifest "$a" "$ma" && dir_manifest "$b" "$mb" && cmp -s "$ma" "$mb"
  rc=$?
  rm -f "$ma" "$mb" 2>/dev/null || true
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
  base=$1; p=0; a=0
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
      log "RESTORE_SNAPSHOT_INTEGRITY=LEGACY_WEAK kind=file name=$name reason=missing_cksum_metadata"
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
    tmp="${dst}.restore.new"
    rm -f "$tmp" 2>/dev/null || true
    cp "$src" "$tmp" && chmod "$mode" "$tmp" && same "$src" "$tmp" && mv "$tmp" "$dst"
  else
    rm -f "$dst"
  fi
}
verify_file_snapshot(){
  dst=$1; name=$2; src="$TXN/files/$name"
  kind=$(snapshot_marker_kind "$src") || return 1
  if [ "$kind" = present ]; then same "$src" "$dst"; else [ ! -e "$dst" ]; fi
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
      log "RESTORE_SNAPSHOT_INTEGRITY=LEGACY_WEAK kind=dir name=$name reason=missing_manifest_metadata"
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
    tmp="${dst}.restore.new"
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
  if [ "$kind" = present ]; then [ -d "$dst" ] && same_dir_exact "$src" "$dst"; else [ ! -e "$dst" ]; fi
}

validate_restore_snapshot(){
  s=$(cat "$TXN/startup.path" 2>/dev/null || true)
  case "$s" in
    "$(p /mnt/system/etc/boot/startup.sh)"|"$(p /etc/boot/startup.sh)") ;;
    *) log "RESTORE_SNAPSHOT_INTEGRITY=FAIL reason=invalid_startup_path"; return 1 ;;
  esac
  for n in startup.sh smartphone_integrator.json dio_manager.json pf.conf carplay_hook.jar legacy_hook libtarget_libairplay.so libtarget_libairplax.so libtarget_libNmeBaseClasses.so; do
    validate_file_snapshot "$n" || { log "RESTORE_SNAPSHOT_INTEGRITY=FAIL kind=file name=$n"; return 1; }
  done
  lp=0; la=0
  [ ! -f "$TXN/libtarget.dir_present" ] || lp=1
  [ ! -f "$TXN/libtarget.dir_absent" ] || la=1
  [ $((lp + la)) -eq 1 ] || { log "RESTORE_SNAPSHOT_INTEGRITY=FAIL kind=libtarget_presence"; return 1; }
  for n in runtime state; do
    validate_dir_snapshot "$n" || { log "RESTORE_SNAPSHOT_INTEGRITY=FAIL kind=dir name=$n"; return 1; }
  done
  log "RESTORE_SNAPSHOT_INTEGRITY=PASS"
  return 0
}

verify_restore_rollback(){
  s=$(cat "$TXN/startup.path" 2>/dev/null || true)
  [ -n "$s" ] || return 1
  verify_file_snapshot "$s" startup.sh || return 1
  verify_file_snapshot "$SI" smartphone_integrator.json || return 1
  verify_file_snapshot "$DIO" dio_manager.json || return 1
  verify_file_snapshot "$PF" pf.conf || return 1
  verify_file_snapshot "$JAR" carplay_hook.jar || return 1
  verify_file_snapshot "$LEGACY_HOOK" legacy_hook || return 1
  for n in libairplay.so libairplax.so libNmeBaseClasses.so; do
    verify_file_snapshot "$LIBTARGET/$n" "libtarget_$n" || return 1
  done
  if [ -f "$TXN/libtarget.dir_present" ]; then [ -d "$LIBTARGET" ] || return 1; else [ ! -d "$LIBTARGET" ] || return 1; fi
  verify_dir_snapshot "$RUNTIME" runtime || return 1
  verify_dir_snapshot "$STATE" state || return 1
  [ ! -e "$STAGE" ] && [ ! -e "$PREV" ] || return 1
  return 0
}



detect_mixed_restore_state(){
  if grep -Fq 'libcarplay_altscreen.so' "$SI" 2>/dev/null; then
    if [ ! -e "$RUNTIME" ]; then
      MIXED_RECOVERY=1
      log "RESTORE_RECOVERY_MODE=MIXED_PRELOAD_RUNTIME_MISSING action=RESTORE_FROM_TRUSTED_BACKUPS"
    elif [ -d "$RUNTIME" ] && [ ! -f "$RUNTIME/.mmi-cockpit-carplay-runtime-owner" ]; then
      MIXED_RECOVERY=1
      log "RESTORE_RECOVERY_MODE=MIXED_PRELOAD_UNOWNED_RUNTIME_RESIDUE action=RESTORE_FROM_TRUSTED_BACKUPS"
    fi
  fi
}

snapshot(){
  [ ! -e "$STAGE" ] || { log "RESTORE=REFUSED reason=RUNTIME_STAGE_PRESENT production_changed=NO"; return 1; }
  [ ! -e "$PREV" ] || { log "RESTORE=REFUSED reason=RUNTIME_PREVIOUS_PRESENT production_changed=NO"; return 1; }
  [ ! -e "$STATE/.chain_test.lock" ] || { log "RESTORE=REFUSED reason=CHAIN_LOCK_PRESENT_AFTER_PRECHECK production_changed=NO"; return 1; }
  rm -rf "$TXN" 2>/dev/null || return 1
  ensure_dirs "$TXN/files" "$TXN/dirs" "$TXN/meta" || return 1
  printf '%s\n' 2 > "$TXN/FORMAT" || return 1
  STARTUP=$(find_startup) || return 1
  echo "$STARTUP" > "$TXN/startup.path" || return 1
  snap_file "$STARTUP" startup.sh || return 1
  snap_file "$SI" smartphone_integrator.json || return 1
  snap_file "$DIO" dio_manager.json || return 1
  snap_file "$PF" pf.conf || return 1
  snap_file "$JAR" carplay_hook.jar || return 1
  snap_file "$LEGACY_HOOK" legacy_hook || return 1
  if [ -d "$LIBTARGET" ]; then touch "$TXN/libtarget.dir_present" || return 1; else touch "$TXN/libtarget.dir_absent" || return 1; fi
  for n in libairplay.so libairplax.so libNmeBaseClasses.so; do snap_file "$LIBTARGET/$n" "libtarget_$n" || return 1; done
  snap_dir "$RUNTIME" runtime || return 1
  snap_dir "$STATE" state || return 1
  validate_restore_snapshot || return 1
  touch "$TXN/PREPARED" || return 1
  sync >/dev/null 2>&1 || {
    rm -f "$TXN/PREPARED" 2>/dev/null || true
    log "RESTORE=REFUSED reason=PREPARED_SYNC_FAILED production_changed=NO"
    return 1
  }
  TXN_READY=1
  log "RESTORE_TRANSACTION=PREPARED durable=YES"
}
rollback(){
  [ -f "$TXN/PREPARED" ] || return 1
  if [ -f "$TXN/COMMITTED" ]; then
    touch "$TXN/ROLLBACK_INCOMPLETE" || return 1
    rm -f "$TXN/COMMITTED" && sync || {
      log "ROLLBACK=REFUSED reason=COMMIT_INVALIDATION_FAILED recovery_required=YES"
      return 1
    }
  fi
  if ! validate_restore_snapshot; then
    touch "$TXN/ROLLBACK_INCOMPLETE" 2>/dev/null || true
    log "ROLLBACK=REFUSED reason=SNAPSHOT_INTEGRITY_FAILED production_changed=NO_BY_ROLLBACK recovery_required=YES"
    return 1
  fi
  ROLLING_BACK=1
  trap - 1 2 15
  log "ROLLBACK=STARTED"
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
    ensure_dirs "$LIBTARGET" || r=1
    for n in libairplay.so libairplax.so libNmeBaseClasses.so; do restore_file "$LIBTARGET/$n" "libtarget_$n" 755 || r=1; done
    if [ -f "$TXN/libtarget.dir_absent" ]; then rmdir "$LIBTARGET" 2>/dev/null || true; fi
    restore_dir "$RUNTIME" runtime || r=1
    rm -rf "$STAGE" "$PREV" 2>/dev/null || true
  fi
  finish_mounts || r=1
  restore_dir "$STATE" state || r=1
  sync >/dev/null 2>&1 || r=1
  if [ "$r" = 0 ] && verify_restore_rollback; then
    if rm -f "$TXN/APPLYING" "$TXN/ROLLBACK_INCOMPLETE" &&
       touch "$TXN/ROLLED_BACK" && sync; then
      log "ROLLBACK_VERIFY=PASS"
      log "ROLLBACK=PASS persistent_state=PRE_RESTORE reboot_required=YES"
      ROLLING_BACK=0
      return 0
    fi
  fi
  rm -f "$TXN/ROLLED_BACK" 2>/dev/null || true
  touch "$TXN/ROLLBACK_INCOMPLETE" 2>/dev/null || true
  log "ROLLBACK=FAIL recovery_required=YES transaction_retained=$TXN"
  ROLLING_BACK=0
  return 1
}
recover_stale(){
  [ -d "$TXN" ] || return 0
  if [ ! -f "$TXN/ROLLBACK_INCOMPLETE" ] &&
     { [ -f "$TXN/COMMITTED" ] || [ -f "$TXN/ROLLED_BACK" ]; }; then rm -rf "$TXN"; return $?; fi
  if [ -f "$TXN/PREPARED" ]; then
    log "STALE_RESTORE_TRANSACTION=DETECTED action=ROLLBACK_FIRST"
    TXN_READY=1
    rollback || return 1
    PRE_RECOVERY_CHANGED=1
    rm -rf "$TXN" || return 1
    TXN_READY=0
    log "STALE_RESTORE_TRANSACTION=RECOVERED production_changed=RECOVERY_TO_PRE_RESTORE"
    return 0
  fi
  # No PREPARED marker means the previous run never reached the first production
  # mutation. It is safe to discard this incomplete SD-only snapshot.
  log "STALE_RESTORE_TRANSACTION=INCOMPLETE_PREPARE action=CLEANUP production_changed=NO"
  rm -rf "$TXN" || return 1
  return 0
}

fail(){
  msg=$1
  log "ERROR: $msg"
  finish_mounts >/dev/null 2>&1 || true
  if [ "$ROLLING_BACK" = 0 ] && [ "$TXN_READY" = 1 ]; then
    rollback && log "RESTORE=ABORTED rollback=PASS" ||
      log "RESTORE=FAILED rollback=INCOMPLETE recovery_required=YES"
  elif [ "$PRE_RECOVERY_CHANGED" = 1 ]; then
    log "RESTORE=REFUSED production_changed=RECOVERY_ONLY current_restore_mutation=NO"
  else
    log "RESTORE=REFUSED production_changed=NO"
  fi
  exit 1
}
trap 'fail "restore interrupted by signal"' 1 2 15

log "===== V3 transactional RESTORE ORIGINAL started ====="
if [ -d "$INSTALL_TXN" ]; then
  [ -f "$INSTALL_TXN_HELPER" ] || fail "active install transaction exists but recovery helper is missing"
  log "ACTIVE_INSTALL_TRANSACTION=DETECTED action=ROLLBACK_PRE_INSTALL_BEFORE_RESTORE"
  PRE_RECOVERY_CHANGED=1
  /bin/sh "$INSTALL_TXN_HELPER" recover || fail "active install transaction could not be recovered before restore"
  log "PRE_RESTORE_RECOVERY=PASS kind=INSTALL_TRANSACTION production_changed=RECOVERY_TO_PRE_INSTALL"
fi
recover_stale || fail "previous restore transaction could not be recovered"
[ -f "$CONTROLLER" ] && [ -f "$APPLY" ] || fail "restore controller/apply helper missing"
preflight_startup || fail "startup restore preflight failed"
/bin/sh "$CONTROLLER" restore-precheck || fail "native/runtime restore precheck failed"
log "RESTORE_PREFLIGHT=PASS hmi=PROJECT_OWNED_DELETE startup=SAFE native_runtime=SAFE production_changed=NO"
detect_mixed_restore_state
snapshot || { rm -rf "$TXN" 2>/dev/null || true; fail "pre-restore transaction snapshot failed"; }
touch "$TXN/APPLYING" || fail "cannot mark transaction APPLYING"
/bin/sh "$APPLY" || fail "restore APPLY step failed"

# Re-verify every file in the trusted native original manifest, not only the
# two JSONs. The universal restore may also restore dio_manager/libairplay/NME.
[ -f "$NATIVE/manifest.txt" ] || fail "native original manifest missing after restore"
NATIVE_COUNT=0
while IFS= read -r rel; do
  [ -n "$rel" ] || continue
  case "$rel" in
    /eso/bin/apps/dio_manager|/mnt/app/eso/bin/apps/dio_manager|/eso/lib/libairplay.so|/armle/usr/lib/libNmeBaseClasses.so|/mnt/app/armle/usr/lib/libNmeBaseClasses.so|/eso/lib/libNmeBaseClasses.so|/mnt/system/etc/eso/production/smartphone_integrator.json|/mnt/system/etc/eso/production/dio_manager.json) ;;
    *) fail "unexpected path in trusted native manifest: $rel" ;;
  esac
  src="$NATIVE/files/$(echo "$rel" | tr '/' '_')"
  dst="$(p "$rel")"
  [ -f "$src" ] && same "$src" "$dst" || fail "trusted native file verification failed: $rel"
  NATIVE_COUNT=$((NATIVE_COUNT + 1))
done < "$NATIVE/manifest.txt"
[ "$NATIVE_COUNT" = 5 ] || fail "trusted native manifest count mismatch"
! grep -Fq 'libcarplay_altscreen.so' "$SI" 2>/dev/null ||
  fail "managed AltScreen preload remains in smartphone_integrator.json after restore"

[ -f "$BACKUP/firewall-original/COMPLETE" ] ||
  fail "firewall original backup missing after restore"
[ -f "$BACKUP/firewall-original/pf.conf" ] &&
  same "$BACKUP/firewall-original/pf.conf" "$PF" ||
  fail "pf.conf verification failed"

# Exact overlay baseline verification.  The original manifest records whether
# each stock overlay file existed and which directory owned that baseline.
OVERLAY_DIR=$(cat "$NATIVE/overlay_dir.txt" 2>/dev/null || true)
case "$OVERLAY_DIR" in
  /mnt/app/root/carplay-altscreen/lib|/mnt/app/root/lib-target) ;;
  *) fail "invalid overlay baseline directory" ;;
esac
for n in libairplay.so libairplax.so libNmeBaseClasses.so; do
  overlay_dst="$(p "$OVERLAY_DIR")/$n"
  if grep -q "^$n$" "$NATIVE/overlay_present.txt" 2>/dev/null; then
    [ -f "$NATIVE/files/overlay_$n" ] && same "$NATIVE/files/overlay_$n" "$overlay_dst" ||
      fail "overlay baseline verification failed: $n"
  else
    [ ! -e "$overlay_dst" ] || fail "overlay should be absent after restore: $n"
  fi
done

if [ -f "$BACKUP/universal-hook-original/COMPLETE" ]; then
  hook_present=$(cat "$BACKUP/universal-hook-original/present" 2>/dev/null || echo invalid)
  hook_rel=$(cat "$BACKUP/universal-hook-original/path" 2>/dev/null || echo /mnt/app/root/hooks/libcarplay_altscreen.so)
  case "$hook_rel" in
    /mnt/app/root/carplay-altscreen/lib/libcarplay_altscreen.so|/mnt/app/root/hooks/libcarplay_altscreen.so) ;;
    *) fail "invalid universal-hook recovery path" ;;
  esac
  hook_dst="$(p "$hook_rel")"
  case "$hook_present" in
    0) [ ! -e "$hook_dst" ] || fail "managed universal hook should be absent after restore" ;;
    1) [ -f "$BACKUP/universal-hook-original/libcarplay_altscreen.so" ] &&
       same "$BACKUP/universal-hook-original/libcarplay_altscreen.so" "$hook_dst" ||
       fail "original universal hook verification failed" ;;
    *) fail "invalid universal-hook backup state" ;;
  esac
fi
[ ! -e "$JAR" ] || fail "project-owned carplay_hook.jar remains after restore"
[ ! -e "$RUNTIME" ] && [ ! -e "$STAGE" ] && [ ! -e "$PREV" ] || fail "managed runtime residue remains after restore"
STARTUP=$(find_startup) || fail "startup.sh missing after restore"
! grep -E 'BEGIN ALT111 (MIRROR|BASEVIDEO3) AUTOSTART|BEGIN ALTSCREEN DIAGNOSTICS' "$STARTUP" >/dev/null 2>&1 || fail "AltScreen autostart/diagnostic block remains"
[ -f "$STATE/RESTORE_PENDING_REBOOT" ] || fail "RESTORE_PENDING_REBOOT marker missing"

sync >/dev/null 2>&1 || fail "sync failed before restore commit"
touch "$TXN/COMMITTED" || fail "cannot commit restore transaction"
sync >/dev/null 2>&1 || fail "cannot durably commit restore transaction"
TXN_READY=0
if [ "$MIXED_RECOVERY" = 1 ]; then
  log "RESTORE_MIXED_STATE_RECOVERY=PASS trusted_backups=YES runtime=ABSENT preload=REMOVED"
fi
log "RESTORE_VERIFY=PASS"
log "RESTORE=PASS transaction=COMMITTED persistent_state=PRE_INSTALL reboot_required=YES"
log "IMPORTANT=DO_NOT_TEST_CARPLAY_BEFORE_FULL_MMI_REBOOT"
rm -rf "$TXN" 2>/dev/null || log "WARN: committed transaction retained; next run will clean it"
sync >/dev/null 2>&1 || log "WARN: committed restore transaction cleanup was not durably synced; terminal COMMITTED residue is safe"
trap - 1 2 15
log "===== V3 transactional RESTORE ORIGINAL finished ====="
exit 0
