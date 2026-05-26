#!/usr/bin/env bash
# Move relation files from a ggnfs-distributed jobdir into a staging folder.
#
# Usage:
#   ./move-rels.sh --jobdir=AS276 --dest=AS276-relsBackup
#   ./move-rels.sh --jobdir=/tmp/myjob --dest=/tmp/myjob-relsBackup --dry-run

set -euo pipefail

jobdir=""
dest=""
dry_run=0
overwrite=0

usage() {
    sed -n '2,7p' "$0"
}

for arg in "$@"; do
    case "$arg" in
        --jobdir=*)    jobdir="${arg#*=}" ;;
        --dest=*)      dest="${arg#*=}" ;;
        --dry-run)     dry_run=1 ;;
        --overwrite)   overwrite=1 ;;
        -h|--help)     usage; exit 0 ;;
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
