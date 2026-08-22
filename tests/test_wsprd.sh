#!/bin/sh
set -eu

fixture="${1:-150426_0918.wav}"
if [ ! -f "$fixture" ]; then
    echo "missing WSPR regression fixture: $fixture" >&2
    exit 1
fi

test_directory=$(mktemp -d "${TMPDIR:-/tmp}/airspyhf-wsprd-test.XXXXXX")
trap 'rm -rf "$test_directory"' EXIT HUP INT TERM

./airspyhf-wsprd-core -a "$test_directory" -f 0.0 "$fixture" > "$test_directory/output.txt"
awk '/^[0-9][0-9][0-9][0-9] / { print $4, $6, $7, $8 }' \
    "$test_directory/output.txt" > "$test_directory/actual.txt"

diff -u tests/150426_0918.expected "$test_directory/actual.txt"
echo "airspyhf-wsprd-core: decoded all 9 expected spots from $fixture"

project_directory=$(pwd)
fixture_path="$project_directory/$fixture"
(
    cd "$test_directory"
    "$project_directory/airspyhf-wsprd-core" -a "$test_directory" -c -s -H -f 0.0 \
        "$fixture_path" > /dev/null
    AIRSPYHF_WSPRD_CORE="$project_directory/airspyhf-wsprd-core" \
        "$project_directory/tests/test_decoder_bridge" 000000_0001.c2
)
