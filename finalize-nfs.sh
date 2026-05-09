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
[ -d "$jobdir/rels" ] || { echo "no $jobdir/rels — did sieving run?" >&2; exit 1; }
[ -f "$yafu_dir/nfs.job" ] || { echo "no $yafu_dir/nfs.job" >&2; exit 1; }

# Sanity: yafu's nfs.job should match the one the server distributed.
server_job=$(ls "$jobdir"/files/*.job 2>/dev/null | head -1 || true)
if [ -n "$server_job" ]; then
    yafu_sha=$(sha256sum "$yafu_dir/nfs.job"  | awk '{print $1}')
    serv_sha=$(sha256sum "$server_job"        | awk '{print $1}')
    if [ "$yafu_sha" != "$serv_sha" ]; then
        echo "WARNING: $yafu_dir/nfs.job differs from $server_job" >&2
        echo "  yafu : $yafu_sha" >&2
        echo "  serv : $serv_sha" >&2
        echo "  Filtering may use the wrong factor base. Aborting — sync them and rerun." >&2
        exit 1
    fi
fi

# Pull N out of the .job file.
N=$(awk '/^n:/ { print $2 }' "$yafu_dir/nfs.job")
[ -n "$N" ] || { echo "could not parse n: from $yafu_dir/nfs.job" >&2; exit 1; }

shopt -s nullglob
rels_files=( "$jobdir"/rels/wu-*.dat )
[ ${#rels_files[@]} -gt 0 ] || { echo "no wu-*.dat under $jobdir/rels" >&2; exit 1; }

# Move aside any prior nfs.dat — yafu's auto-rewrite would clobber relations.
if [ -f "$yafu_dir/nfs.dat" ]; then
    mv "$yafu_dir/nfs.dat" "$yafu_dir/nfs.dat.prev.$(date +%s)"
fi

# Assemble: header + per-workunit .dat files.
{
    echo "N $N"
    cat "${rels_files[@]}"
} > "$yafu_dir/nfs.dat"

bytes=$(stat -c %s "$yafu_dir/nfs.dat")
lines=$(wc -l < "$yafu_dir/nfs.dat")
echo "wrote $yafu_dir/nfs.dat  ($bytes bytes, $lines lines, ${#rels_files[@]} workunits)"
echo "  N: ${N:0:20}...${N: -8}"
echo "  first relation: $(sed -n '2p' "$yafu_dir/nfs.dat" | head -c 80)"

cmd=( ./yafu "factor($N)" "-$phase" -R -v -threads "$threads" )

if [ "$do_run" -eq 1 ]; then
    echo "running: cd $yafu_dir && ${cmd[*]}"
    cd "$yafu_dir"
    "${cmd[@]}"
else
    echo
    echo "next: cd $yafu_dir && ${cmd[*]}"
fi
