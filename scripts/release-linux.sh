#!/bin/sh
set -eu

PROGRAM=arqan
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DOCKERFILE=$ROOT/packaging/linux/Dockerfile
IMAGE=${ARQAN_RELEASE_IMAGE:-arqan-linux-release:debian11}

fail() {
    printf '%s\n' "$PROGRAM release: $*" >&2
    exit 1
}

command -v docker >/dev/null 2>&1 || fail 'Docker is required'
docker info >/dev/null 2>&1 || fail 'Docker daemon is unavailable'
command -v git >/dev/null 2>&1 || fail 'git is required to derive the commit timestamp'
epoch=$(git -C "$ROOT" show -s --format=%ct HEAD) || fail 'cannot derive commit timestamp'
uid=$(id -u)
gid=$(id -g)

rm -rf -- "$ROOT/dist"
complete=false
cleanup() {
    if ! $complete; then rm -rf -- "$ROOT/dist"; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
docker build --platform linux/amd64 -f "$DOCKERFILE" -t "$IMAGE" "$ROOT/packaging/linux"
docker run --rm --platform linux/amd64 \
    --user "$uid:$gid" \
    -e HOME=/tmp/arqan-home \
    -e SOURCE_DATE_EPOCH="$epoch" \
    -e ARQAN_RELEASE_CLEAN=1 \
    -v "$ROOT:/work" \
    -w /work \
    "$IMAGE" \
    sh -ec 'make clean && make test && make package-linux && make test-package-linux'

set -- "$ROOT"/dist/*.tar.gz
[ "$#" -eq 1 ] && [ -f "$1" ] || fail 'release build did not produce exactly one archive'
[ -f "$1.sha256" ] || fail 'release build did not produce the checksum'
find "$ROOT/dist" -mindepth 1 -maxdepth 1 -type f \
    ! -name "$(basename -- "$1")" ! -name "$(basename -- "$1.sha256")" -delete
complete=true
printf '%s\n' "Release files are in $ROOT/dist."
