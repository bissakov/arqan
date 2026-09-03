#!/bin/sh
# Build the Alpine builder image from packaging/linux/Dockerfile.musl, push it,
# and write the pushed digest into scripts/build-musl.sh. Every later portable
# build then uses that one toolchain, so the archive can be checked by
# rebuilding it. Run this when the toolchain has to move, not per release.
#
# NOTE: the push needs a registry login, and the image must stay readable
# without one, or a rebuild of an old tag cannot reach it.
set -eu

PROGRAM=arqan
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DOCKERFILE=$ROOT/packaging/linux/Dockerfile.musl
BUILD_SCRIPT=$ROOT/scripts/build-musl.sh
REPOSITORY=${ARQAN_MUSL_REPOSITORY:-ghcr.io/bissakov/arqan-linux-musl}
TAG=${ARQAN_MUSL_TAG:-alpine3.22}

fail() {
    printf '%s\n' "$PROGRAM musl image: $*" >&2
    exit 1
}

command -v docker >/dev/null 2>&1 || fail 'Docker is required'
docker info >/dev/null 2>&1 || fail 'Docker daemon is unavailable'
[ -f "$DOCKERFILE" ] || fail "missing $DOCKERFILE"
[ -f "$BUILD_SCRIPT" ] || fail "missing $BUILD_SCRIPT"
grep -q '^PINNED_IMAGE=' "$BUILD_SCRIPT" ||
    fail "$BUILD_SCRIPT has no PINNED_IMAGE line to write"
case $REPOSITORY in
    *@*|*' '*) fail "invalid repository: $REPOSITORY" ;;
esac

docker build --platform linux/amd64 --pull --no-cache \
    -f "$DOCKERFILE" -t "$REPOSITORY:$TAG" "$ROOT/packaging/linux"
docker push "$REPOSITORY:$TAG"

digest=$(docker image inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' \
    "$REPOSITORY:$TAG" | grep "^$REPOSITORY@sha256:" | head -n 1)
[ -n "$digest" ] || fail 'the push reported no digest'

tmp=$BUILD_SCRIPT.$$
trap 'rm -f -- "$tmp"' EXIT
sed "s|^PINNED_IMAGE=.*|PINNED_IMAGE=$digest|" "$BUILD_SCRIPT" >"$tmp"
grep -qx "PINNED_IMAGE=$digest" "$tmp" || fail 'cannot record the digest'
chmod 755 "$tmp"
mv -- "$tmp" "$BUILD_SCRIPT"
trap - EXIT

printf '%s\n' "Pinned $digest in scripts/build-musl.sh. Commit that line."
