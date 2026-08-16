#!/bin/sh
# Build the rpm's binaries in the EL9 builder image and run the end-to-end
# suite against them. Output is bin/el9/, owned by the caller.
set -eu

PROGRAM=arqan
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DOCKERFILE=$ROOT/packaging/linux/Dockerfile.el9
IMAGE=${ARQAN_EL9_IMAGE:-arqan-linux-el9:almalinux9}

fail() {
    printf '%s\n' "$PROGRAM el9 build: $*" >&2
    exit 1
}

command -v docker >/dev/null 2>&1 || fail 'Docker is required'
docker info >/dev/null 2>&1 || fail 'Docker daemon is unavailable'

target=el9
[ "${1:-}" = --no-test ] || target=test-el9

docker build --platform linux/amd64 -f "$DOCKERFILE" -t "$IMAGE" \
    "$ROOT/packaging/linux"
docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp/$PROGRAM-home \
    -v "$ROOT:/work" \
    -w /work \
    "$IMAGE" \
    make "$target"

# The point of the exercise: the binaries ask no rpm host for a libcurl it
# does not have. A CURL_ version reference means the link happened on Debian,
# whose versioned symbols this family lacks, and every host would warn about
# it at startup.
for exe in arqan arqan-highlight; do
    binary=$ROOT/bin/el9/$exe
    [ -x "$binary" ] || fail "missing bin/el9/$exe"
    ! readelf --version-info -W "$binary" 2>/dev/null | grep -q 'CURL_' || \
        fail "$exe requires versioned libcurl symbols"
done
# libcurl is opened at the first request, so nothing may name it as NEEDED.
# The packages carry the dependency by hand instead; a NEEDED entry here would
# mean the rpm's automatic requires and the spec's explicit one had silently
# swapped places.
! readelf -dW "$ROOT/bin/el9/arqan" | grep -q 'NEEDED.*libcurl' || \
    fail 'arqan links libcurl instead of opening it on demand'

printf '%s\n' "The rpm binaries are in $ROOT/bin/el9."
