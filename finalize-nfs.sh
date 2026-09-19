#!/usr/bin/env bash
# finalize-nfs.sh — assemble nfs.dat from a server jobdir or pull-rels archive
# and (optionally) invoke YAFU's filter -> LA -> sqrt pipeline.
#
# Usage:
#   finalize-nfs.sh --jobdir=./snfs301 --yafu-dir=/path/to/yafu
#                   [--job-file=PATH] [--jobdb=PATH] [--threads=8]
#                   [--run] [--phase=nc|nc1|nc2|nc3|ncr] [--check]
#
# Reads archive/ and rels/, using job.db or the newest usable incoming snapshot.
# --check validates inputs and reports selected files without writing output.
# nc/nc1 assemble relations; nc2/nc3/ncr reuse the existing nfs.dat because
# filtering artifacts and LA checkpoints depend on its exact relation order.
# Without --run, prints the YAFU command; --phase=nc1 runs only filtering.

set -euo pipefail
export LC_ALL=C

jobdir=""
yafu_dir=""
server_job=""
db=""
threads=1
do_run=0
check=0
phase="nc"

for arg in "$@"; do
    case "$arg" in
        --jobdir=*)   jobdir="${arg#*=}" ;;
        --yafu-dir=*) yafu_dir="${arg#*=}" ;;
        --job-file=*) server_job="${arg#*=}" ;;
        --jobdb=*)    db="${arg#*=}" ;;
        --threads=*)  threads="${arg#*=}" ;;
        --phase=*)    phase="${arg#*=}" ;;
        --run)        do_run=1 ;;
        --check)      check=1 ;;
        -h|--help)    sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

fail() { echo "$*" >&2; exit 1; }
[ -n "$jobdir" ]   || fail "missing --jobdir"
[ -n "$yafu_dir" ] || fail "missing --yafu-dir"
[[ "$threads" =~ ^[1-9][0-9]*$ ]] || fail "--threads must be a positive integer"
case "$phase" in nc|nc1|nc2|nc3|ncr) ;; *) fail "invalid --phase: $phase" ;; esac

# bash doesn't expand ~ inside a quoted --foo=~/bar.
jobdir="${jobdir/#\~/$HOME}"
yafu_dir="${yafu_dir/#\~/$HOME}"
server_job="${server_job/#\~/$HOME}"
db="${db/#\~/$HOME}"
[ -d "$jobdir" ] || fail "jobdir $jobdir does not exist"
[ -d "$yafu_dir" ] || fail "yafu dir $yafu_dir does not exist"
shopt -s nullglob
sqlite_read() { sqlite3 -init /dev/null -readonly -batch -noheader -list "$@"; }
tmp_dat=""
passed_list=""
cleanup() {
    [ -z "$tmp_dat" ] || rm -f -- "$tmp_dat"
    [ -z "$passed_list" ] || rm -f -- "$passed_list"
    return 0
}
trap cleanup EXIT

snapshot_usable() {
    local result
    result=$(sqlite_read "$1" "PRAGMA quick_check;
        SELECT value FROM meta WHERE key='job_sha256';
        SELECT file_path, verify_status FROM submissions LIMIT 0;" 2>/dev/null) || return 1
    [[ "$result" =~ ^ok$'\n'[0-9a-f]{64}$ ]]
}

# Pulls retain cumulative DB snapshots, named with sortable UTC timestamps.
# Never select relations from incoming/: validation may have failed there.
if [ -z "$db" ]; then
    if [ -f "$jobdir/job.db" ]; then
        db="$jobdir/job.db"
    else
        snapshots=( "$jobdir"/incoming/*/job.db )
        if [ ${#snapshots[@]} -gt 0 ]; then
            command -v sqlite3 >/dev/null || fail "sqlite3 is required to read snapshots"
            for ((i=${#snapshots[@]}-1; i>=0; i--)); do
                if snapshot_usable "${snapshots[i]}"; then
                    db="${snapshots[i]}"
                    break
                fi
                echo "warning: skipping unusable snapshot: ${snapshots[i]}" >&2
            done
            [ -n "$db" ] || fail "no usable database snapshot found; refusing unverified directory fallback"
        fi
    fi
fi
job_sha=""
if [ -n "$db" ]; then
    [ -f "$db" ] || fail "database does not exist: $db"
    command -v sqlite3 >/dev/null || fail "sqlite3 is required to read $db"
    # Capture query status directly: process substitution hides sqlite errors.
    job_sha=$(sqlite_read "$db" \
        "SELECT value FROM meta WHERE key='job_sha256';") || fail "cannot read job identity from $db"
    [[ "$job_sha" =~ ^[0-9a-f]{64}$ ]] || fail "missing or invalid job_sha256 in $db"
    echo "using database: $db"
fi

# New pulls include the original .job next to their snapshot. Older archives
# can use --job-file, a sibling <jobdir>.job, or an existing matching nfs.job.
if [ -z "$server_job" ]; then
    candidates=( "$jobdir"/files/*.job )
    if [ -n "$db" ]; then
        candidates+=( "${db%/*}"/files/*.job )
    fi
    candidates+=( "${jobdir%/}.job" )
    # An output job is a source only when the database independently proves
    # its identity. Without that, comparing nfs.job to itself proves nothing.
    [ -z "$job_sha" ] || candidates+=( "$yafu_dir/nfs.job" )
    for candidate in "${candidates[@]}"; do
        [ -f "$candidate" ] || continue
        candidate_sha=$(sha256sum "$candidate" | awk '{print $1}')
        if [ -z "$job_sha" ] || [ "$candidate_sha" = "$job_sha" ]; then
            server_job="$candidate"
            break
        fi
    done
fi
[ -f "$server_job" ] || fail "no matching .job found; pass --job-file=PATH or run an updated pull-rels.sh (expected SHA-256: ${job_sha:-unknown})"
serv_sha=$(sha256sum "$server_job" | awk '{print $1}')
if [ -n "$job_sha" ] && [ "$serv_sha" != "$job_sha" ]; then
    fail "$server_job does not match the job in $db (expected $job_sha, got $serv_sha)"
fi
if [ -f "$yafu_dir/nfs.job" ]; then
    yafu_sha=$(sha256sum "$yafu_dir/nfs.job" | awk '{print $1}')
    if [ "$yafu_sha" != "$serv_sha" ]; then
        echo "$yafu_dir/nfs.job differs from $server_job — refusing to overwrite" >&2
        echo "  yafu:   $yafu_sha" >&2
        echo "  source: $serv_sha" >&2
        fail "use a separate YAFU directory, or move the existing run aside before retrying"
    fi
fi
N=$(awk '/^n:/ { print $2 }' "$server_job" | tr -d '\r')
[[ "$N" =~ ^[0-9]+$ ]] || fail "could not parse one integer n: from $server_job"
echo "using job file: $server_job"

if [ "$do_run" -eq 1 ] && [ "$check" -eq 0 ]; then
    [ -x "$yafu_dir/yafu" ] || fail "no executable at $yafu_dir/yafu"
fi

if [[ "$phase" = nc || "$phase" = nc1 ]]; then
    dat_files=()
    zst_files=()
    declare -A seen=()
    add_file() {
        local f="$1" base="${1##*/}"
        [ -z "${seen[$base]:-}" ] || return 0
        seen["$base"]=1
        case "$f" in
            *.dat.zst) zst_files+=( "$f" ) ;;
            *.dat)     dat_files+=( "$f" ) ;;
            *) echo "warning: skipping unsupported relation file: $f" >&2 ;;
        esac
    }

    if [ -n "$db" ]; then
        # Spool once, checking sqlite's status before consuming any rows.
        # This avoids both a large shell string and hidden producer failures.
        passed_list=$(mktemp)
        sqlite_read "$db" \
            "SELECT DISTINCT file_path FROM submissions WHERE verify_status='passed' ORDER BY file_path;" > "$passed_list" \
            || fail "cannot select passed submissions from $db"
        missing=0
        while IFS= read -r fp; do
            [ -n "$fp" ] || continue
            base="${fp##*/}"
            # Archived files have passed pull's validation gate. Prefer them
            # over a duplicate copy still present in rels/.
            if [ -f "$jobdir/archive/$base" ]; then
                add_file "$jobdir/archive/$base"
            elif [ -f "$jobdir/rels/$base" ]; then
                add_file "$jobdir/rels/$base"
            else
                missing=$((missing + 1))
            fi
        done < "$passed_list"
        rm -f -- "$passed_list"
        passed_list=""
        if [ "$missing" -gt 0 ]; then
            echo "warning: $missing passed submission file(s) are absent from archive/ and rels/; assembling only locally available files" >&2
        fi
        echo "selecting relation files from: passed submissions in database"
    else
        echo "warning: no database; selecting directory files without verification status" >&2
        for f in "$jobdir"/{archive,rels}/{wu,blk}-*.dat{,.zst}; do
            [ -f "$f" ] && add_file "$f"
        done
    fi
    total=$(( ${#dat_files[@]} + ${#zst_files[@]} ))
    [ "$total" -gt 0 ] || fail "no eligible relation files under $jobdir/archive or $jobdir/rels"
    echo "selected $total submission files: ${#dat_files[@]} raw + ${#zst_files[@]} zstd"
    if [ ${#zst_files[@]} -gt 0 ]; then
        command -v zstd >/dev/null || fail "zstd not found in PATH"
    fi

    if [ "$check" -eq 0 ]; then
        # Publish only a complete assembly. In particular a corrupt compressed
        # file must not replace a previously usable nfs.dat with a partial one.
        tmp_dat=$(mktemp "$yafu_dir/.nfs.dat.XXXXXX")
        {
            echo "N $N"
            if [ ${#dat_files[@]} -gt 0 ]; then
                printf '%s\0' "${dat_files[@]}" | xargs -0 cat -- || exit 1
            fi
            if [ ${#zst_files[@]} -gt 0 ]; then
                printf '%s\0' "${zst_files[@]}" | xargs -0 zstd -dcq -- || exit 1
            fi
        } | awk '
            # The target YAFU reader stores b in uint32_t. Other consumers can
            # use wider b: do not impose this limit in sieving or verification.
            # GPU output can contain larger
            # values; passing those through can hit YAFU\047s 10,000-error
            # abort threshold. Keep the archives intact, omit only these
            # unusable relations from the assembled filtering input.
            /^-?[0-9]+,[0-9]+:/ {
                comma = index($0, ",")
                colon = index($0, ":")
                b = substr($0, comma + 1, colon - comma - 1) + 0
                if (b > 4294967295) { dropped++; next }
            }
            { print }
            END {
                if (dropped)
                    printf "YAFU compatibility: omitted %.0f relations with b > 4294967295 (originals retained in source files)\n", dropped > "/dev/stderr"
            }
        ' > "$tmp_dat"
        if [ -f "$yafu_dir/nfs.dat" ]; then
            chmod --reference="$yafu_dir/nfs.dat" "$tmp_dat"
            backup=$(mktemp "$yafu_dir/nfs.dat.prev.XXXXXX")
            # Same filesystem: preserve the old file without copying its bytes.
            ln -f -- "$yafu_dir/nfs.dat" "$backup"
            echo "preserved previous relations: $backup"
        else
            file_umask=$(umask)
            printf -v output_mode '%03o' "$((0666 & ~file_umask))"
            chmod "$output_mode" "$tmp_dat"
        fi
        [ -f "$yafu_dir/nfs.job" ] || cp -- "$server_job" "$yafu_dir/nfs.job"
        mv -- "$tmp_dat" "$yafu_dir/nfs.dat"
        tmp_dat=""
        bytes=$(stat -c %s "$yafu_dir/nfs.dat")
        lines=$(wc -l < "$yafu_dir/nfs.dat")
        echo "wrote $yafu_dir/nfs.dat ($bytes bytes, $lines lines including header)"
        echo "  first relation: $(sed -n '2{p;q;}' "$yafu_dir/nfs.dat" | cut -c 1-80)"
    fi
else
    [ -f "$yafu_dir/nfs.dat" ] || fail "--phase=$phase requires existing nfs.dat from filtering"
    [ -f "$yafu_dir/nfs.job" ] || fail "--phase=$phase requires existing nfs.job from filtering"
    IFS= read -r header < "$yafu_dir/nfs.dat" || fail "existing nfs.dat has an empty or incomplete header"
    [ "${header%$'\r'}" = "N $N" ] || fail "existing nfs.dat has a different N"
    echo "keeping existing nfs.dat for --phase=$phase"
fi

cmd=( ./yafu "factor($N)" "-$phase" -R -v -threads "$threads" )
printf -v printable '%q ' "${cmd[@]}"
printf -v quoted_dir '%q' "$yafu_dir"
if [ "$check" -eq 1 ]; then
    echo "check complete; no output files changed"
elif [ "$do_run" -eq 1 ]; then
    echo "running: cd $quoted_dir && $printable"
    cd "$yafu_dir"
    "${cmd[@]}"
else
    echo "next: cd $quoted_dir && $printable"
fi
