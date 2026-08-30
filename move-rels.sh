#!/usr/bin/env bash
# Move relation files from a ggnfs-distributed jobdir into a staging folder.
#
# Usage:
#   ./move-rels.sh --jobdir=AS276 --dest=AS276-relsBackup
#   ./move-rels.sh --jobdir=/tmp/myjob --dest=/tmp/myjob-relsBackup --dry-run
#   ./move-rels.sh --jobdir=AS276 --dest=AS276-relsBackup --verified-only
#
# --verified-only consults <jobdir>/job.db and only moves files for
# workunits currently in state 'verified', so we never race the verifier
# on files for submissions that are still 'pending' or 'failed'.

set -euo pipefail

jobdir=""
dest=""
dry_run=0
overwrite=0
verified_only=0

usage() {
    sed -n '2,12p' "$0"
}

for arg in "$@"; do
    case "$arg" in
        --jobdir=*)        jobdir="${arg#*=}" ;;
        --dest=*)          dest="${arg#*=}" ;;
        --dry-run)         dry_run=1 ;;
        --overwrite)       overwrite=1 ;;
        --verified-only)   verified_only=1 ;;
        -h|--help)         usage; exit 0 ;;
        *)
            echo "unknown arg: $arg" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[ -n "$jobdir" ] || { echo "missing --jobdir" >&2; usage >&2; exit 2; }
[ -n "$dest" ] || { echo "missing --dest" >&2; usage >&2; exit 2; }

src="$jobdir/rels"

[ -d "$jobdir" ] || { echo "jobdir does not exist: $jobdir" >&2; exit 1; }
[ -d "$src" ] || { echo "rels directory does not exist: $src" >&2; exit 1; }

src_abs=$(realpath -m -- "$src")
dest_abs=$(realpath -m -- "$dest")
case "$dest_abs" in
    "$src_abs"|"$src_abs"/*)
        echo "destination must not be inside the rels directory: $dest" >&2
        exit 1
        ;;
esac

if [ "$dry_run" -eq 0 ]; then
    mkdir -p "$dest"
elif [ ! -d "$dest" ]; then
    echo "dry-run: would create $dest"
fi

shopt -s dotglob nullglob
items=( "$src"/* )

if [ "$verified_only" -eq 1 ]; then
    db="$jobdir/job.db"
    [ -f "$db" ] || { echo "--verified-only: $db does not exist" >&2; exit 1; }
    command -v sqlite3 >/dev/null 2>&1 \
        || { echo "--verified-only: sqlite3 not on PATH" >&2; exit 1; }

    # Read verified ids into an associative set. -readonly keeps us safe even
    # if a writer is active; the dot-command is the cheapest way to dump a
    # single column.
    #
    # Two id spaces, because a relation file is named after whatever was
    # leased. A single workunit's file is <workunit_id>.dat[.zst]; a GPU
    # block's is <block_id>.dat[.zst] -- named after the block, not its anchor
    # workunit, so a re-sieved anchor cannot overwrite a block's relations.
    # Reading only `workunits` here would leave every block file behind as
    # unrecognised while quietly moving the CPU ones.
    declare -A verified
    while IFS= read -r id; do
        [ -n "$id" ] && verified["$id"]=1
    done < <(sqlite3 -readonly -batch "$db" \
        "SELECT id FROM workunits WHERE state = 'verified';")

    # gpu_blocks only exists on a jobdir served by a build that has blocks;
    # tolerate its absence rather than failing on an older one.
    while IFS= read -r id; do
        [ -n "$id" ] && verified["$id"]=1
    done < <(sqlite3 -readonly -batch "$db" \
        "SELECT id FROM gpu_blocks WHERE state = 'verified';" 2>/dev/null || true)

    if [ "${#verified[@]}" -eq 0 ]; then
        echo "no verified workunits or blocks found in $db"
        exit 0
    fi

    filtered=()
    skipped=0
    for item in "${items[@]}"; do
        base="${item##*/}"
        # Strip .dat.zst / .dat to recover the workunit or block id.
        wid="${base%.zst}"
        wid="${wid%.dat}"
        if [ -n "${verified[$wid]:-}" ]; then
            filtered+=( "$item" )
        else
            skipped=$(( skipped + 1 ))
        fi
    done
    items=( "${filtered[@]}" )
    [ "$skipped" -gt 0 ] && echo "skipping $skipped unverified file(s)"
fi

if [ "${#items[@]}" -eq 0 ]; then
    echo "no files to move under $src"
    exit 0
fi

conflicts=()
if [ "$overwrite" -eq 0 ]; then
    for item in "${items[@]}"; do
        base="${item##*/}"
        if [ -e "$dest/$base" ]; then
            conflicts+=( "$dest/$base" )
        fi
    done
fi

if [ "${#conflicts[@]}" -gt 0 ]; then
    echo "refusing to overwrite existing destination files:" >&2
    printf '  %s\n' "${conflicts[@]}" >&2
    echo "rerun with --overwrite if that is intentional" >&2
    exit 1
fi

echo "moving ${#items[@]} item(s) from $src to $dest"
if [ "$dry_run" -eq 1 ]; then
    printf '  %s\n' "${items[@]}"
    exit 0
fi

mv_args=()
if [ "$overwrite" -eq 0 ]; then
    mv_args+=( -n )
fi

for item in "${items[@]}"; do
    mv "${mv_args[@]}" -- "$item" "$dest/"
done
echo "done"
