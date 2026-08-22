#!/bin/sh
set -eu

source_directory="${1:-../wsprd}"
destination_directory="${2:-core}"

files="
AUTHORS
COPYING
fano.c
fano.h
jelinek.c
jelinek.h
metric_tables.c
nhash.c
nhash.h
pffft.c
pffft.h
tab.c
wsprd.c
wsprd_utils.c
wsprd_utils.h
wsprsim_utils.c
wsprsim_utils.h
"

if [ ! -d "$source_directory" ]; then
    echo "wsprd source directory not found: $source_directory" >&2
    exit 1
fi

mkdir -p "$destination_directory"
for file in $files; do
    cp "$source_directory/$file" "$destination_directory/$file"
done

if git -C "$source_directory" rev-parse HEAD >/dev/null 2>&1; then
    git -C "$source_directory" rev-parse HEAD > "$destination_directory/UPSTREAM_COMMIT"
fi

echo "Synced wsprd core from $source_directory to $destination_directory"
