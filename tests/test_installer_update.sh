#!/bin/sh
set -eu

project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary_directory=$(mktemp -d /tmp/airspyhf-update-test.XXXXXX)
trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM
origin=$temporary_directory/origin
checkout=$temporary_directory/checkout

git init -q -b master "$origin"
git -C "$origin" config user.name test
git -C "$origin" config user.email test@example.invalid
mkdir -p "$origin/scripts"
printf 'initial\n' >"$origin/Makefile"
printf 'initial\n' >"$origin/airspyhf_wsprd.c"
printf 'initial\n' >"$origin/scripts/install-systemd.sh"
git -C "$origin" add Makefile airspyhf_wsprd.c scripts/install-systemd.sh
git -C "$origin" commit -q -m initial

git clone -q "$origin" "$checkout"
git -C "$checkout" config user.name test
git -C "$checkout" config user.email test@example.invalid
printf 'local divergence\n' >"$checkout/airspyhf_wsprd.c"
git -C "$checkout" add airspyhf_wsprd.c
git -C "$checkout" commit -q -m local-divergence

printf 'upstream revision\n' >"$origin/airspyhf_wsprd.c"
git -C "$origin" add airspyhf_wsprd.c
git -C "$origin" commit -q -m upstream-revision

"$project_directory/scripts/update-checkout.sh" "$origin" "$checkout"

test "$(sed -n '1p' "$checkout/airspyhf_wsprd.c")" = "upstream revision"
test "$(git -C "$checkout" log -1 --format=%s)" = "upstream-revision"
test -z "$(git -C "$checkout" status --short)"

echo "installer update: divergent checkout reset to validated origin/master"
