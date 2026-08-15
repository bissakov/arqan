#!/bin/sh
# Build the relocatable musl binaries in the Alpine builder image and run the
# end-to-end suite against them. Output is bin/musl/, owned by the caller.
set -eu

PROGRAM=arqan
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DOCKERFILE=$ROOT/packaging/linux/Dockerfile.musl
IMAGE=${ARQAN_MUSL_IMAGE:-arqan-linux-musl:alpine3.22}

fail() {
    printf '%s\n' "$PROGRAM musl build: $*" >&2
    exit 1
}

command -v docker >/dev/null 2>&1 || fail 'Docker is required'
docker info >/dev/null 2>&1 || fail 'Docker daemon is unavailable'

target=static
[ "${1:-}" = --no-test ] || target=test-static

docker build --platform linux/amd64 -f "$DOCKERFILE" -t "$IMAGE" \
    "$ROOT/packaging/linux"
docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp/$PROGRAM-home \
    -v "$ROOT:/work" \
    -w /work \
    "$IMAGE" \
    make "$target"

# The point of the exercise: no interpreter, no NEEDED entry, still position
# independent. A dynamic binary here means the link silently fell back.
for exe in arqan arqan-highlight; do
    binary=$ROOT/bin/musl/$exe
    [ -x "$binary" ] || fail "missing bin/musl/$exe"
    file -L "$binary" | grep -q 'static-pie linked' || \
        fail "$exe is not statically linked as PIE"
    ! readelf -dW "$binary" 2>/dev/null | grep -q '(NEEDED)' || \
        fail "$exe still needs a shared library"
    "$binary" --version >/dev/null || fail "$exe does not run"
done

printf '%s\n' "Static binaries are in $ROOT/bin/musl."
