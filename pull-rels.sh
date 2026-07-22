#!/usr/bin/env bash
# pull-rels.sh — pull verified relation files off the sieve coordinator.
#
# Workflow per run:
#   1. flock on a local lockfile so two invocations can't collide.
#   2. SSH in, pipe move-rels.sh up over stdin, run it with --verified-only
#      against a fresh per-run staging dir: <remote-jobdir>-staging/<ts>/
#   3. Take a consistent snapshot of job.db (sqlite3 .backup) into the same
#      staging dir.
#   4. rsync the staging dir down to <local-dir>/incoming/<ts>/.
#   5. Run ggnfs-verify against the downloaded files (parse + q-range +
#      full GMP norm on every relation), batched through xargs so a large
#      file count can't overflow the exec argument limit. Validation is a
#      hard gate: if any file fails, or fewer files were checked than we
#      downloaded, we stop here — the files stay in incoming/ for inspection
#      and the remote staging dir is left untouched. (--no-validate opts out
#      of the gate.)
#   6. Only if validation passed: move files from incoming/ to archive/.
#   7. Only after step 6 succeeds, ssh in and rm -rf the remote staging dir.
#      Any failure earlier leaves the staging dir on the server untouched.
#
# Usage:
#   ./pull-rels.sh \
#       --ssh-host=user@host \
#       --ssh-key=~/.ssh/key \
#       --remote-jobdir=/srv/ggnfs/AS276 \
#       --local-dir=~/sieve-archive/AS276
#
# Optional:
#   --verify-bin=PATH        path to ggnfs-verify (default: same dir as this script)
#   --no-validate            skip local validation
#   --validate-no-norm       parse + q-range only (no GMP) for speed
#   --keep-staging           don't delete the remote staging dir at the end
#   --dry-run                go through the motions; touch nothing destructive
#   --interactive            allow ssh to prompt for a key passphrase. Sets up
#                            an SSH ControlMaster so you only type it once per
#                            run instead of for every remote call.
#   --ssh-opt=OPT            extra ssh option (repeatable), e.g. --ssh-opt=-p2222

set -euo pipefail

ssh_host=""
ssh_key=""
remote_jobdir=""
local_dir=""
verify_bin=""
no_validate=0
validate_no_norm=0
keep_staging=0
dry_run=0
interactive=0
ssh_extra=()

usage() {
    sed -n '2,36p' "$0"
}

for arg in "$@"; do
    case "$arg" in
        --ssh-host=*)        ssh_host="${arg#*=}" ;;
        --ssh-key=*)         ssh_key="${arg#*=}" ;;
        --remote-jobdir=*)   remote_jobdir="${arg#*=}" ;;
        --local-dir=*)       local_dir="${arg#*=}" ;;
        --verify-bin=*)      verify_bin="${arg#*=}" ;;
        --no-validate)       no_validate=1 ;;
        --validate-no-norm)  validate_no_norm=1 ;;
        --keep-staging)      keep_staging=1 ;;
        --dry-run)           dry_run=1 ;;
        --interactive)       interactive=1 ;;
        --ssh-opt=*)         ssh_extra+=( "${arg#*=}" ) ;;
        -h|--help)           usage; exit 0 ;;
        *)
            echo "unknown arg: $arg" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[ -n "$ssh_host" ]      || { echo "missing --ssh-host" >&2; exit 2; }
[ -n "$remote_jobdir" ] || { echo "missing --remote-jobdir" >&2; exit 2; }
[ -n "$local_dir" ]     || { echo "missing --local-dir" >&2; exit 2; }

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
move_rels_sh="$script_dir/move-rels.sh"
[ -f "$move_rels_sh" ] || {
    echo "move-rels.sh not found at $move_rels_sh" >&2; exit 1;
}

if [ "$no_validate" -eq 0 ]; then
    if [ -z "$verify_bin" ]; then
        verify_bin="$script_dir/ggnfs-verify"
    fi
    [ -x "$verify_bin" ] || {
        echo "ggnfs-verify not executable at $verify_bin" >&2
        echo "(run 'make verify' or pass --verify-bin=PATH or --no-validate)" >&2
        exit 1
    }
fi

# Expand a leading ~ ourselves — shells only do tilde-expansion on
# unquoted text, and these came in via --opt= which is quoted.
case "$local_dir" in
    \~|\~/*) local_dir="$HOME${local_dir#\~}" ;;
esac
case "$ssh_key" in
    \~|\~/*) ssh_key="$HOME${ssh_key#\~}" ;;
esac

mkdir -p "$local_dir/incoming" "$local_dir/archive" "$local_dir/.locks"

# --- SSH setup ----------------------------------------------------------
# Default: BatchMode=yes, which refuses to prompt for password / passphrase
# (so the script never silently hangs in cron). --interactive drops that
# and additionally opens an SSH ControlMaster — the first call prompts for
# your key passphrase, every subsequent ssh and rsync reuses that one auth.
ssh_args=( -o ConnectTimeout=15 )
[ -n "$ssh_key" ] && ssh_args+=( -i "$ssh_key" )

cm_socket_dir=""
cleanup_controlmaster() {
    if [ -n "$cm_socket_dir" ]; then
        # Best-effort: tell the master to exit, then nuke the dir.
        ssh "${ssh_args[@]}" -O exit "$ssh_host" >/dev/null 2>&1 || true
        rm -rf -- "$cm_socket_dir"
    fi
}

if [ "$interactive" -eq 1 ]; then
    cm_socket_dir=$(mktemp -d -t pull-rels-cm.XXXXXX)
    # ControlPersist=yes keeps the master alive until our EXIT trap kills
    # it explicitly. Any finite value (we tried 60) can expire during the
    # long local validation step, which means the final cleanup ssh call
    # re-prompts for the passphrase. %C in ControlPath is a hash that
    # keeps the socket name short enough for sun_path's ~100-byte limit.
    ssh_args+=( -o "ControlMaster=auto" \
                -o "ControlPath=$cm_socket_dir/cm-%C" \
                -o "ControlPersist=yes" )
    trap cleanup_controlmaster EXIT
else
    ssh_args+=( -o BatchMode=yes )
fi

for opt in "${ssh_extra[@]+"${ssh_extra[@]}"}"; do
    ssh_args+=( "$opt" )
done

# SC2029: we intentionally let arguments expand on the local side — the
# caller constructs ready-to-run remote shell using rq() for path quoting.
# shellcheck disable=SC2029
ssh_run()  { ssh "${ssh_args[@]}" "$ssh_host" "$@"; }
# shellcheck disable=SC2029
ssh_pipe() { ssh "${ssh_args[@]}" "$ssh_host" "$@"; }

# Shell-quote one path so it survives a round-trip through ssh -> sh -c.
rq() { printf '%q' "$1"; }

# --- Lock so two pulls don't trample each other -------------------------
lockfile="$local_dir/.locks/pull-rels.lock"
exec 9>"$lockfile"
if ! flock -n 9; then
    echo "another pull-rels.sh is running (lock: $lockfile)" >&2
    exit 1
fi

# --- Per-run timestamp identifies the staging dir on both sides ---------
ts=$(date -u +%Y-%m-%dT%H-%M-%SZ)
remote_staging="${remote_jobdir%/}-staging/$ts"
local_incoming="$local_dir/incoming/$ts"
local_archive="$local_dir/archive"

if [ -e "$local_incoming" ]; then
    echo "local incoming dir already exists: $local_incoming" >&2
    exit 1
fi

echo "[$(date -u +%FT%TZ)] pull-rels start"
echo "  ssh_host=$ssh_host"
echo "  remote_jobdir=$remote_jobdir"
echo "  remote_staging=$remote_staging"
echo "  local_incoming=$local_incoming"
echo "  validate=$([ "$no_validate" -eq 1 ] && echo no || echo yes)"

# --- Probe remote: jobdir + job.db must exist, sqlite3 must be on PATH --
ssh_run "set -e
    test -d $(rq "$remote_jobdir") || { echo 'remote jobdir missing' >&2; exit 1; }
    test -f $(rq "$remote_jobdir/job.db") || { echo 'remote job.db missing' >&2; exit 1; }
    command -v sqlite3 >/dev/null 2>&1 || { echo 'sqlite3 missing on remote' >&2; exit 1; }"

if [ "$dry_run" -eq 1 ]; then
    echo "dry-run: would create $remote_staging and stage verified files"
    echo "dry-run: would rsync to $local_incoming, validate, archive, and rm remote staging"
    exit 0
fi

# --- Step 1: pipe move-rels.sh up and run --verified-only ---------------
echo "[move] running move-rels.sh --verified-only on remote ..."
ssh_pipe "bash -s -- --jobdir=$(rq "$remote_jobdir") --dest=$(rq "$remote_staging") --verified-only" \
    < "$move_rels_sh"

# --- Step 2: sqlite3 .backup of job.db into the staging dir -------------
# sqlite3's dot-command needs the destination single-quoted inside the
# command string, and the whole second arg double-quoted at the shell level.
echo "[snap] taking job.db snapshot into staging ..."
ssh_run "sqlite3 $(rq "$remote_jobdir/job.db") \".backup '$remote_staging/job.db'\""

# How many .dat.zst files did we end up with?
remote_count=$(ssh_run "find $(rq "$remote_staging") -maxdepth 1 -name '*.dat*' -type f | wc -l" | tr -d ' \n')
remote_count=${remote_count:-0}

if [ "$remote_count" -eq 0 ]; then
    echo "[skip] no verified relation files in staging; cleaning up empty staging dir"
    if [ "$keep_staging" -eq 0 ]; then
        ssh_run "rm -rf -- $(rq "$remote_staging")"
    fi
    exit 0
fi

# --- Step 3: rsync staging dir down -------------------------------------
echo "[xfer] rsyncing $remote_count file(s) to $local_incoming ..."
mkdir -p "$local_incoming"
# --partial + --append-verify make this resumable. -t preserves mtime so
# repeat runs don't redo work (not that we should ever re-pull, but cheap).
# Use the same ssh args as the rest of the script via -e.
rsync_ssh="ssh"
for a in "${ssh_args[@]}"; do
    rsync_ssh="$rsync_ssh $(printf %q "$a")"
done
rsync -av --partial --append-verify -e "$rsync_ssh" \
    "$ssh_host:$remote_staging/" "$local_incoming/"

local_count=$(find "$local_incoming" -maxdepth 1 -name '*.dat*' -type f | wc -l)
if [ "$local_count" -ne "$remote_count" ]; then
    echo "[ERR] file count mismatch: remote=$remote_count local=$local_count" >&2
    echo "      remote staging left in place at $remote_staging for inspection" >&2
    exit 1
fi

# --- Step 4: local validation ------------------------------------------
# validate_passed gates archiving + remote GC below. It stays 1 when
# --no-validate is given (the caller explicitly opted out of the check).
validate_passed=1
if [ "$no_validate" -eq 0 ]; then
    echo "[chk ] running ggnfs-verify on $local_count file(s) ..."
    verify_args=( --jobdb="$local_incoming/job.db" --quiet )
    [ "$validate_no_norm" -eq 1 ] && verify_args+=( --no-norm )
    verify_log="$local_incoming/verify.log"

    # Batch through xargs. Passing tens of thousands of paths as a single
    # argv overflows the kernel exec limit (E2BIG / "Argument list too
    # long") before ggnfs-verify even starts — that is exactly the failure
    # that silently skipped validation on a 40k-file pull. find -print0 |
    # xargs -0 splits the work into as many invocations as needed; each
    # prints its own "summary:" line, which we aggregate below.
    #
    # ggnfs-verify exits 0 even when files fail validation (failures show up
    # only in the summary line), so the log — not the exit code — is what
    # tells us whether the pull is good.
    set +e +o pipefail
    find "$local_incoming" -maxdepth 1 -name '*.dat*' -type f -print0 \
        | xargs -0 -r "$verify_bin" "${verify_args[@]}" >"$verify_log" 2>&1
    verify_rc=${PIPESTATUS[1]}
    set -e -o pipefail

    # Echo any per-file FAIL / I-O lines and each batch summary to the console.
    grep -E ': FAIL |: I/O error|^summary:' "$verify_log" >&2 || true

    # Sum "N file(s)" and "M with failures" across every batch summary.
    files_checked=$(awk '/^summary:/{for(i=1;i<=NF;i++) if($i=="file(s),") s+=$(i-1)} END{print s+0}' "$verify_log")
    files_failed=$(awk  '/^summary:/{for(i=1;i<=NF;i++) if($i=="with")      s+=$(i-1)} END{print s+0}' "$verify_log")

    echo "[chk ] verify: checked=$files_checked/$local_count with_failures=$files_failed (rc=$verify_rc, log: $verify_log)"
    if [ "$verify_rc" -ne 0 ] || [ "$files_checked" -ne "$local_count" ] || [ "$files_failed" -ne 0 ]; then
        validate_passed=0
    fi
else
    echo "[chk ] --no-validate: skipping ggnfs-verify"
fi

# Hard gate: nothing destructive happens unless validation passed. On
# failure the downloaded files stay in incoming/ AND the remote staging dir
# is left in place, so nothing is lost and the pull can be inspected/re-run.
if [ "$validate_passed" -ne 1 ]; then
    echo "[FAIL] validation did not pass — NOT archiving, NOT deleting remote." >&2
    echo "       downloaded files remain in $local_incoming" >&2
    echo "       remote staging preserved at  $remote_staging" >&2
    exit 1
fi

# --- Step 5: move files from incoming/<ts>/ to archive/ -----------------
# Atomic per-file rename on same filesystem. Job.db snapshot stays in
# incoming/<ts>/ (we keep one snapshot per pull alongside the files).
# If you'd rather not retain old snapshots, gc them out-of-band.
echo "[arch] moving files to $local_archive/ ..."
shopt -s nullglob
moved=0
for f in "$local_incoming"/*.dat*; do
    base="${f##*/}"
    if [ -e "$local_archive/$base" ]; then
        echo "[WARN] $local_archive/$base already exists; leaving in incoming" >&2
        continue
    fi
    mv -- "$f" "$local_archive/$base"
    moved=$((moved + 1))
done
shopt -u nullglob

echo "[arch] moved $moved file(s) to archive"

# --- Step 6: clean up remote staging (only if we got everything) --------
remaining=$(find "$local_incoming" -maxdepth 1 -name '*.dat*' -type f | wc -l)
if [ "$remaining" -ne 0 ]; then
    echo "[keep] $remaining file(s) still in $local_incoming (archive collisions); leaving remote staging in place" >&2
    exit 1
fi

if [ "$keep_staging" -eq 1 ]; then
    echo "[done] --keep-staging set; remote $remote_staging preserved"
else
    echo "[gc  ] rm -rf remote staging: $remote_staging"
    ssh_run "rm -rf -- $(rq "$remote_staging")"
fi

echo "[$(date -u +%FT%TZ)] pull-rels OK"
