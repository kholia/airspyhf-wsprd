#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 REPOSITORY CHECKOUT [BRANCH]" >&2
    exit 1
fi

repository=$1
checkout=$2
branch=${3:-master}

if [ ! -d "$checkout/.git" ]; then
    echo "$checkout is not a Git checkout." >&2
    exit 1
fi

origin=$(git -C "$checkout" remote get-url origin 2>/dev/null || true)
if [ "$origin" != "$repository" ]; then
    echo "$checkout has unexpected origin '$origin'; expected '$repository'." >&2
    echo "Refusing to overwrite this checkout." >&2
    exit 1
fi

git -C "$checkout" fetch --depth 1 origin "$branch"

for required_path in Makefile airspyhf_wsprd.c scripts/install-systemd.sh; do
    if ! git -C "$checkout" cat-file -e "FETCH_HEAD:$required_path"; then
        echo "Fetched revision is not an airspyhf-wsprd source tree." >&2
        exit 1
    fi
done

# This is an installer-managed build checkout. Always match the fetched branch,
# even after its history was rewritten or the local branch diverged.
git -C "$checkout" reset --hard FETCH_HEAD
