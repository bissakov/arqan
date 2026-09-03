#!/bin/sh
# Build the relocatable musl binaries in the Alpine builder image and run the
# end-to-end suite against them. Output is bin/musl/, owned by the caller.
#
# NOTE: the toolchain and the static archives come from the builder image, so
# the binaries are only reproducible when every host builds them in the same
# image. Alpine serves one build per package per branch, so resolving the
# packages at image build time gives a different toolchain on a different day.
# PINNED_IMAGE names the published image by digest; scripts/publish-musl-image.sh
# builds, pushes and records it.
set -eu

PROGRAM=arqan
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DOCKERFILE=$ROOT/packaging/linux/Dockerfile.musl
PINNED_IMAGE=ghcr.io/bissakov/arqan-linux-musl@sha256:902f5f86a48b2584fd89e46e28cbc0c19f4156fe4da181933acfea1d04a58ada
LOCAL_IMAGE=arqan-linux-musl:alpine3.22
IMAGE=${ARQAN_MUSL_IMAGE:-}

fail() {
    printf '%s\n' "$PROGRAM musl build: $*" >&2
    exit 1
}

warn() {
    printf '%s\n' "$PROGRAM musl build: $*" >&2
}

command -v docker >/dev/null 2>&1 || fail 'Docker is required'
docker info >/dev/null 2>&1 || fail 'Docker daemon is unavailable'

target=static
[ "${1:-}" = --no-test ] || target=test-static

if [ -n "$IMAGE" ]; then
    :
elif [ -n "$PINNED_IMAGE" ]; then
    IMAGE=$PINNED_IMAGE
    docker pull --platform linux/amd64 "$IMAGE" >/dev/null ||
        fail "cannot pull $IMAGE"
else
    IMAGE=$LOCAL_IMAGE
    warn 'no builder image is pinned. This build resolves the Alpine toolchain
now, so the binaries will not match those built on another host or another day.
Run scripts/publish-musl-image.sh to publish an image and pin it by digest.'
    docker build --platform linux/amd64 -f "$DOCKERFILE" -t "$IMAGE" \
        "$ROOT/packaging/linux"
fi

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
    readelf -hW "$binary" | grep -Eq '^[[:space:]]*Type:[[:space:]]+DYN' || \
        fail "$exe is not position independent"
    ! readelf -dW "$binary" 2>/dev/null | grep -q '(NEEDED)' || \
        fail "$exe still needs a shared library"
    ! readelf -lW "$binary" 2>/dev/null | grep -q 'INTERP' || \
        fail "$exe still names a program interpreter"
    "$binary" --version >/dev/null || fail "$exe does not run"
done

printf '%s\n' "Static binaries are in $ROOT/bin/musl."
