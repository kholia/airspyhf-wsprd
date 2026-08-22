#!/bin/sh
set -eu

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ubuntu_image=${UBUNTU_IMAGE:-ubuntu:latest}
builder_image=${UBUNTU_BUILDER_IMAGE:-airspyhf-wsprd-ubuntu-test}

if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is required for the Ubuntu/GCC build check." >&2
    exit 1
fi

docker build --pull \
    --build-arg "UBUNTU_IMAGE=$ubuntu_image" \
    --tag "$builder_image" \
    --file "$project_directory/scripts/Dockerfile.ubuntu-test" \
    "$project_directory"

docker run --rm \
    --mount "type=bind,source=$project_directory,target=/source,readonly" \
    --tmpfs /work:exec,size=2g \
    --workdir /work \
    "$builder_image" \
    bash -euc '
        tar -C /source \
            --exclude=.git \
            --exclude=./test-hw/build \
            --exclude=./test-hw/firmware/build \
            -cf - . | tar -C /work -xf -
        make clean all test
    '
