#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fqbn='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=4M,PartitionScheme=minimal,PSRAM=disabled'

mkdir -p "$script_directory/build"
arduino-cli compile \
    --fqbn "$fqbn" \
    --warnings all \
    --build-path "$script_directory/build/local-objects" \
    --output-dir "$script_directory/build" \
    "$script_directory/firmware"
