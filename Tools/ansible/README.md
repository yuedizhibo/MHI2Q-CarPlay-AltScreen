# Tools/ansible — install an AltScreen release over SSH (no SD card)

Deploys a released `V3.x.zip` package to the head unit over SSH onto the
internal staging volume `/mnt/ota/carplay-altscreen`, then drives the
upstream Toolbox scripts from there. It is the SSH equivalent of the
green-menu **INSTALL / RESTORE ORIGINAL** entries: the same scripts run,
with the same RESULT vocabulary, backup handling and reboot policy.

The head unit has no Python interpreter. Every remote step is Ansible
`raw` (POSIX/QNX shell); file transfers use `scp` launched on the laptop.

## Prerequisites

- **SSH access to the unit** — enabled via
  [jilleb/mib2-toolbox](https://github.com/jilleb/mib2-toolbox)
  (`custom Toolbox → sshd_install` or an equivalent sshd install).
- The playbook does **not** need the green menu or `Update Toolbox`; it
  runs the scripts straight from the staged volume. But if you never did
  the Update Toolbox step, the on-screen INSTALL / RESTORE ORIGINAL
  entries will not exist and `altscreen_action=restore` over SSH is your
  recovery path — so keep SSH working and keep the fetched
  `MMI-Cockpit-Carplay` copy.
- **Ansible ≥ 2.14** on the laptop, plus `tar`, `find`, `cksum`,
  `shasum`/`sha256sum` — all stock on macOS and Linux.
- **sshpass** (optional): only needed when authenticating with
  `ansible_password`. `scp` cannot answer an interactive password
  prompt, so the playbook fails fast with a clear message if a password
  is configured and `sshpass` is not installed. Key/agent auth needs
  nothing extra.

## Inventory

```sh
cp inventory/hosts.example.yml inventory/hosts.yml   # gitignored
# edit ansible_host / ansible_password (or use a key)
```

The unit's `qnx_path` group var provides the PATH every `raw` task
exports before running — do not trim it.

## How the staging volume works — the `ALTS_VOLUME` model

Upstream Toolbox scripts normally discover the package by scanning SD
card mount points for a `Toolbox/` folder. The companion scripts change
adds an **explicit override**: when the environment variable
`ALTS_VOLUME` is set, the scripts use exactly that directory and never
scan; when it is unset, discovery is unchanged and only SD cards are
considered.

Consequences:

- **The internal volume is never auto-selected.** A green-menu
  INSTALL / RESTORE ORIGINAL run (which has no `ALTS_VOLUME` in its
  environment) always uses the SD card, even while
  `/mnt/ota/carplay-altscreen` exists — an internal copy can never be
  picked up by accident.
- The playbook exports `ALTS_VOLUME=/mnt/ota/carplay-altscreen` before
  invoking any upstream script. **The staged release must therefore
  contain `ALTS_VOLUME`-aware scripts** (a release built from a tree that
  includes the companion scripts change). Staging an older package is
  harmless but useless: its scripts ignore `ALTS_VOLUME`, scan the SD
  slots instead, and the installer stops at its own package check with
  "Nothing was changed" (observed with the stock V3.8 ZIP).
- `stage` copies a *verified* release into
  `/mnt/ota/carplay-altscreen/Toolbox`; `install`/`restore` run from
  there.
- The upstream installer writes the stock backup and its operation logs
  to `/mnt/ota/carplay-altscreen/MMI-Cockpit-Carplay` — the playbook
  **never** deletes that directory (restage removes `Toolbox/` only).
- **Diagnostics:** the boot-time diagnostics scripts are deliberately
  unchanged — they only ever write to a discovered *SD card*. On an
  SSH-installed unit with no card inserted they simply idle; nothing is
  written to `/mnt/ota` at boot. The only writes to the staging volume
  are the staged `Toolbox/` tree and the small per-operation logs under
  `MMI-Cockpit-Carplay/logs/operations` (fetched by
  `altscreen_action=logs`).

## Disk space

`/mnt/ota` is shared with stock OTA data and core dumps. `stage` runs a
free-space gate before uploading: it needs `2 × tar size + 32 MB` free
on `/mnt/ota` (checked with `df -k`, fail-closed — an unparseable `df`
output aborts with the raw output; bypass with
`-e altscreen_skip_space_check=true` if your unit's `df` differs).

**Remove the staging volume when it is not needed** to return the space:

```sh
# on the unit, AFTER fetching MMI-Cockpit-Carplay (altscreen_action=logs
# or a successful install/restore already fetches it):
rm -rf /mnt/ota/carplay-altscreen
```

Removing it also deletes the on-unit stock backup — keep the fetched
`MMI-Cockpit-Carplay` copy safe. If you later need `restore`, re-stage
the release first and copy the backup back in one run:

```sh
ansible-playbook playbooks/altscreen.yml -e altscreen_action=stage \
  -e altscreen_release_dir=/path/to/V3.8 \
  -e altscreen_backup_dir=/path/to/fetched/MMI-Cockpit-Carplay
```

(`altscreen_backup_dir` is uploaded to the volume only when the remote
`MMI-Cockpit-Carplay` is absent; the playbook refuses to overwrite an
existing one.)

## Usage

All commands run from `Tools/ansible/`:

```sh
# Status: identity (train/MU), staged?, installed status (read-only)
ansible-playbook playbooks/altscreen.yml -e altscreen_action=status

# Stage a release (extracted V3.8.zip directory)
ansible-playbook playbooks/altscreen.yml -e altscreen_action=stage \
  -e altscreen_release_dir=/path/to/V3.8

# Install (the defaults below correspond to the green-menu entry
# "INSTALL WITH RGI"; the train gate is an exact match against the
# unit's reported train)
ansible-playbook playbooks/altscreen.yml -e altscreen_action=install \
  -e altscreen_expected_train=MHI2Q_US_AUG22_P4246 \
  -e altscreen_rgi_mode=WITH -e altscreen_card_mode=NO

# Restore original (same console as the green-menu RESTORE ORIGINAL)
ansible-playbook playbooks/altscreen.yml -e altscreen_action=restore \
  -e altscreen_expected_train=MHI2Q_US_AUG22_P4246

# Fetch logs + staged backup/log dir
ansible-playbook playbooks/altscreen.yml -e altscreen_action=logs
```

`altscreen_expected_train` is required for `install`/`restore`: the
playbook reads `Current train = '...'` / `MainUnit version = '...'` from
`/net/rcc/dev/shmem/version.txt` and refuses to mutate on any mismatch.

The `rgi`/`card` mode pair maps to the green-menu install entries:

| Green-menu entry          | altscreen_rgi_mode | altscreen_card_mode |
| ------------------------- | ------------------ | ------------------- |
| INSTALL NOTHING           | `NO`               | `NO`                |
| INSTALL WITH RGI          | `WITH`             | `NO`                |
| INSTALL WITH CARD         | `NO`               | `WITH`              |
| INSTALL WITH RGI CARD     | `WITH`             | `WITH`              |

If a volume is already staged and its files differ from the release you
are staging, `stage` fails; pass `-e altscreen_force_restage=true` to
replace `Toolbox/` (the `MMI-Cockpit-Carplay` backup dir is never
removed).

`ProxyJump` and other ssh options work via
`ansible_ssh_common_args: '-o ProxyJump=user@jumphost'` or `~/.ssh/config`;
the playbook builds its `scp` commands from the same variables.

## Integrity model

- **Local:** `SHA256SUMS-SD.txt` from the release must verify inside the
  release dir before anything uploads.
- **Transport:** a POSIX-`cksum` manifest (`crc size path`) of every
  file under `Toolbox/` is generated on the laptop and verified on the
  unit after extraction. `cksum` is POSIX and uses the same CRC
  algorithm on QNX, Linux and macOS — verified on-device that
  laptop-generated checksums match.
  **QNX quirk:** `cksum < file` prints padded fields plus a `STDIN`
  name token (`   5104849      40956 STDIN`), so the reader normalises
  with `set -- $(cksum < "$1")` before comparing.
- **Run:** the installer prints ` RESULT: SUCCESS` on completion; the
  playbook fails on any other outcome. Restore matches the console's
  RESULT states (`SUCCESS`, `RECOVERED TO STOCK`, `NOTHING TO RESTORE`,
  `RESCUE SUCCESS`).

## Reboot policy

**No reboot is ever automated.** The playbook prints `NO REBOOT
PERFORMED` after install/restore; do a full head-unit power cycle
yourself to activate or to return to stock.

## Recovery notes

- `altscreen_action=restore` runs the same transactional restore as the
  green-menu **RESTORE ORIGINAL** entry.
- On a unit installed via this playbook there is no SD copy: SSH restore
  is the recovery path. If the green-menu entries were installed too
  (via Update Toolbox from an SD card), a RESTORE ORIGINAL run **from a
  card** needs the `MMI-Cockpit-Carplay` backup directory copied onto
  that card — the card's own backup must come from the fetched copy you
  kept.
- If a restore reports `RESCUE REQUIRED`/`RESCUE FAILED`, follow the
  on-screen instructions and keep all backup copies.

## Tested

Run with this exact playbook on 2026-10-21 on a 2018 Audi Q5 (USA /
North America), `MHI2Q_US_AUG22_P4246` / MU 1320, V3.8 scripts with the
companion `ALTS_VOLUME` change, over SSH through a jump host with
password auth (`sshpass`). Sequence and results:

| step | result |
|---|---|
| `status` | `train=MHI2Q_US_AUG22_P4246 mu=1320`, `STAGED=no` |
| `stage` (+ `altscreen_backup_dir`) | `SPACE_CHECK` passed on QNX `df -k`; 185/185 Toolbox files and 44/44 backup files verified by `cksum` on the unit; `STAGE=EMPTY` |
| `install` (RGI WITH, card WITH) | installer: `Previous version found - removing it first` → `RESULT: SUCCESS`; recovery copy fetched |
| `restore` | `RESULT: SUCCESS` — no `ALT111`/`ALTSCREEN` startup blocks, no `carplay_hook.jar`, no runtime dir left on the unit |
| `install` again | `OK - none found` → `RESULT: SUCCESS` |

Two defects were found by this run and fixed in the same change: `scp -r`
cannot create a missing remote directory (the backup dir now travels as
a tar + manifest like the Toolbox payload), and macOS `tar` silently
added AppleDouble `._*` sidecar files to the upload (now suppressed with
`COPYFILE_DISABLE=1`, and the on-unit verify fails on any extra file).
