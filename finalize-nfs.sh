#!/usr/bin/env bash
# finalize-nfs.sh — assemble nfs.dat from a distributed sieving jobdir
# and (optionally) invoke YAFU's filter -> LA -> sqrt pipeline.
#
# Usage:
#   finalize-nfs.sh --jobdir=/tmp/yafu-job-real --yafu-dir=/home/kylea/yafu
#                   [--threads=8] [--run] [--phase=nc|nc1|nc2|nc3|ncr]
#
# Without --run: writes <yafu-dir>/nfs.dat and prints the yafu command.
# With    --run: also invokes ./yafu "factor(N)" -<phase>.

set -euo pipefail

jobdir=""
yafu_dir=""
threads=1
do_run=0
phase="nc"

for arg in "$@"; do
    case "$arg" in
        --jobdir=*)   jobdir="${arg#*=}" ;;
        --yafu-dir=*) yafu_dir="${arg#*=}" ;;
        --threads=*)  threads="${arg#*=}" ;;
        --phase=*)    phase="${arg#*=}" ;;
        --run)        do_run=1 ;;
        -h|--help)
            sed -n '2,12p' "$0"; exit 0 ;;
        *)
            echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

[ -n "$jobdir" ]   || { echo "missing --jobdir"   >&2; exit 2; }
[ -n "$yafu_dir" ] || { echo "missing --yafu-dir" >&2; exit 2; }

# bash doesn't expand ~ inside --foo=~/bar; handle it ourselves.
jobdir="${jobdir/#\~/$HOME}"
yafu_dir="${yafu_dir/#\~/$HOME}"

[ -d "$jobdir/rels" ] || { echo "no $jobdir/rels — did sieving run?" >&2; exit 1; }
[ -d "$yafu_dir" ] || { echo "yafu dir $yafu_dir does not exist" >&2; exit 1; }

server_job=$(ls "$jobdir"/files/*.job 2>/dev/null | head -1 || true)
[ -n "$server_job" ] || { echo "no .job under $jobdir/files" >&2; exit 1; }

# Seed yafu's nfs.job from the server's job. If one's already there and matches,
# leave it alone (re-running finalize is fine). If it differs, bail — we don't
# want to clobber an unrelated yafu run.
if [ -f "$yafu_dir/nfs.job" ]; then
    yafu_sha=$(sha256sum "$yafu_dir/nfs.job" | awk '{print $1}')
    serv_sha=$(sha256sum "$server_job"       | awk '{print $1}')
    if [ "$yafu_sha" = "$serv_sha" ]; then
        echo "$yafu_dir/nfs.job already matches $server_job — keeping it"
    else
        echo "$yafu_dir/nfs.job exists and differs from $server_job — refusing to overwrite" >&2
        echo "  yafu : $yafu_sha" >&2
        echo "  serv : $serv_sha" >&2
        echo "  move it aside if you want finalize-nfs to copy a fresh one." >&2
        exit 1
    fi
else
    cp "$server_job" "$yafu_dir/nfs.job"
    echo "copied $server_job -> $yafu_dir/nfs.job"
fi

# Pull N out of the .job file. Strip CR in case the .job has CRLF endings
# — a trailing \r in N would silently break yafu invocation and make the
# printed command look truncated when copied from the terminal.
N=$(awk '/^n:/ { print $2 }' "$yafu_dir/nfs.job" | tr -d '\r')
[ -n "$N" ] || { echo "could not parse n: from $yafu_dir/nfs.job" >&2; exit 1; }

shopt -s nullglob

# Which relation files go into nfs.dat.
#
# Prefer the database: `SELECT ... WHERE verify_status='passed'` is the only
# source that knows which submissions actually passed. Globbing the directory
# assembles failed submissions too (their files are left on disk), and it
# cannot tell a superseded file from a current one. It also has to know every
# filename shape the server has ever used -- a block's file is named after its
# block, not a workunit, precisely so a re-sieved anchor cannot overwrite it.
#
# Paths in the DB are absolute and recorded on the SERVER, so a jobdir that was
# rsynced here (pull-rels.sh) will not match them. Re-root every basename under
# this jobdir's rels/ instead of trusting the stored directory.
dat_files=()
zst_files=()
selected_from="database"
db="$jobdir/job.db"

if [ -f "$db" ] && command -v sqlite3 >/dev/null 2>&1; then
    missing=0
    while IFS= read -r fp; do
        [ -n "$fp" ] || continue
        f="$jobdir/rels/${fp##*/}"
        if [ ! -f "$f" ]; then
            missing=$(( missing + 1 ))
            continue
        fi
        case "$f" in
            *.zst) zst_files+=( "$f" ) ;;
            *)     dat_files+=( "$f" ) ;;
        esac
    done < <(sqlite3 "$db" \
        "SELECT file_path FROM submissions WHERE verify_status='passed' ORDER BY id;")

    if [ "$missing" -gt 0 ]; then
        echo "warning: $missing passed submission(s) have no file under $jobdir/rels" >&2
        echo "         (moved away by move-rels.sh? assembling without them)" >&2
    fi
fi

# Fallback: no DB, no sqlite3, or a DB that recorded nothing. Match BOTH
# shapes -- omitting blk-* here would silently drop every GPU block while
# still finding CPU files, so `total` would stay non-zero and nothing would
# look wrong until filtering came up short.
if [ "$(( ${#dat_files[@]} + ${#zst_files[@]} ))" -eq 0 ]; then
    selected_from="directory glob (no verify status available)"
    dat_files=( "$jobdir"/rels/wu-*.dat "$jobdir"/rels/blk-*.dat )
    zst_files=( "$jobdir"/rels/wu-*.dat.zst "$jobdir"/rels/blk-*.dat.zst )
fi

total=$(( ${#dat_files[@]} + ${#zst_files[@]} ))
[ "$total" -gt 0 ] || { echo "no relation files under $jobdir/rels" >&2; exit 1; }
echo "selecting relation files from: $selected_from"

if [ ${#zst_files[@]} -gt 0 ]; then
    command -v zstd >/dev/null || { echo "zstd not found in PATH, needed to decompress .dat.zst" >&2; exit 1; }
fi

# Move aside any prior nfs.dat — yafu's auto-rewrite would clobber relations.
if [ -f "$yafu_dir/nfs.dat" ]; then
    mv "$yafu_dir/nfs.dat" "$yafu_dir/nfs.dat.prev.$(date +%s)"
fi

# Assemble: header + per-workunit relations. xargs avoids ARG_MAX with ~38k files.
# Serial concatenation — relation order doesn't matter to msieve, but parallel
# writers to one stdout would interleave bytes and corrupt lines.
{
    echo "N $N"
    if [ ${#dat_files[@]} -gt 0 ]; then
        printf '%s\0' "${dat_files[@]}" | xargs -0 cat
    fi
    if [ ${#zst_files[@]} -gt 0 ]; then
        printf '%s\0' "${zst_files[@]}" | xargs -0 zstd -dcq
    fi
} > "$yafu_dir/nfs.dat"

bytes=$(stat -c %s "$yafu_dir/nfs.dat")
lines=$(wc -l < "$yafu_dir/nfs.dat")
echo "wrote $yafu_dir/nfs.dat  ($bytes bytes, $lines lines, $total workunits: ${#dat_files[@]} raw + ${#zst_files[@]} zstd)"
echo "  N: ${N:0:20}...${N: -8}"
echo "  first relation: $(sed -n '2p' "$yafu_dir/nfs.dat" | head -c 80)"

cmd=( ./yafu "factor($N)" "-$phase" -R -v -threads "$threads" )
# Printable form: bash drops the quotes around factor($N) when joining the
# array with spaces, and the parens break copy-paste without them.
printable="./yafu \"factor($N)\" -$phase -R -v -threads $threads"

if [ "$do_run" -eq 1 ]; then
    echo "running: cd $yafu_dir && $printable"
    cd "$yafu_dir"
    "${cmd[@]}"
else
    echo
    echo "next: cd $yafu_dir && $printable"
fi
