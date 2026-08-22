#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 /dev/cu.usbmodemXXXX  (Linux: /dev/ttyACM0)" >&2
    exit 1
fi

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fqbn='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=4M,PartitionScheme=minimal,PSRAM=disabled'

arduino-cli upload \
    --fqbn "$fqbn" \
    --port "$1" \
    --input-dir "$script_directory/build"
