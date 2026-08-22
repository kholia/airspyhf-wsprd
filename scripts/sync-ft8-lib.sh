#!/bin/sh
set -eu

source_directory="${1:-../ft8_lib}"
destination_directory="${2:-ft8_lib}"

if [ ! -d "$source_directory/ft8" ] || [ ! -d "$source_directory/fft" ]; then
    echo "ft8_lib source directory not found: $source_directory" >&2
    exit 1
fi

mkdir -p "$destination_directory/ft8" \
    "$destination_directory/fft" \
    "$destination_directory/common" \
    "$destination_directory/test/wav"

cp "$source_directory/LICENSE" "$destination_directory/LICENSE"
cp "$source_directory/README.md" "$destination_directory/README.upstream.md"
cp "$source_directory/Makefile" "$destination_directory/Makefile.upstream"
cp "$source_directory/ft8/"*.c "$source_directory/ft8/"*.h \
    "$destination_directory/ft8/"
cp "$source_directory/fft/"*.c "$source_directory/fft/"*.h \
    "$destination_directory/fft/"
cp "$source_directory/common/common.h" \
    "$source_directory/common/monitor.c" \
    "$source_directory/common/monitor.h" \
    "$source_directory/common/wave.c" \
    "$source_directory/common/wave.h" \
    "$destination_directory/common/"
cp "$source_directory/test/wav/191111_110130.wav" \
    "$source_directory/test/wav/191111_110130.txt" \
    "$destination_directory/test/wav/"

git -C "$source_directory" rev-parse HEAD > "$destination_directory/UPSTREAM_COMMIT"

echo "Synced ft8_lib runtime from $source_directory to $destination_directory"
