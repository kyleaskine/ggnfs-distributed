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
dat_files=( "$jobdir"/rels/wu-*.dat )
zst_files=( "$jobdir"/rels/wu-*.dat.zst )
total=$(( ${#dat_files[@]} + ${#zst_files[@]} ))
[ "$total" -gt 0 ] || { echo "no wu-*.dat or wu-*.dat.zst under $jobdir/rels" >&2; exit 1; }

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
