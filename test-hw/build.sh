#!/bin/sh
set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
image=airspyhf-wsprd-hw-builder:arduino-cli-1.5.1-esp32-3.3.11
fqbn='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=4M,PartitionScheme=minimal,PSRAM=disabled'

docker build --pull --tag "$image" "$script_directory"
mkdir -p "$script_directory/build"
docker run --rm \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --volume "$script_directory:/work" \
    --workdir /work \
    "$image" \
    arduino-cli --config-file /etc/arduino-cli.yaml compile \
      --fqbn "$fqbn" \
      --warnings all \
      --build-path /work/build/docker-objects \
      --output-dir /work/build \
      /work/firmware
