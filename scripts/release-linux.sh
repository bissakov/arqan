#!/bin/sh
set -eu

PROGRAM=arqan
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DOCKERFILE=$ROOT/packaging/linux/Dockerfile
IMAGE=${ARQAN_RELEASE_IMAGE:-arqan-linux-release:debian11}
DEBIAN_IMAGE=debian:11-slim@sha256:4a2e40d0d34f8f86f60ef0d79c14d3b6b3d2620825dcdf93152535b5efbaf490
UBUNTU_IMAGE=ubuntu:24.04@sha256:561618e2c15bf2397621dd04f96926663a3b5616c189cf7e38db7e82f5c538ea
FEDORA_IMAGE=fedora:43@sha256:762d73ba1c455232b0272c5d445a34f36c4b9f421cbc05ce8102552325b6a222
ALPINE_IMAGE=alpine:3.22@sha256:14358309a308569c32bdc37e2e0e9694be33a9d99e68afb0f5ff33cc1f695dce
ARCH_IMAGE=archlinux:base@sha256:b0deabeb3d283da2c7f7dbf0eea051b7b2cd0554e0b737cc457fd21683bdcdd1

fail() {
    printf '%s\n' "$PROGRAM release: $*" >&2
    exit 1
}

command -v docker >/dev/null 2>&1 || fail 'Docker is required'
docker info >/dev/null 2>&1 || fail 'Docker daemon is unavailable'
command -v git >/dev/null 2>&1 || fail 'git is required to derive the commit timestamp'
epoch=$(git -C "$ROOT" show -s --format=%ct HEAD) || fail 'cannot derive commit timestamp'
version=$(awk '/^[[:space:]]*#[[:space:]]*define[[:space:]]+AGENT_VERSION[[:space:]]+"/ {
    value=$0; sub(/^[^"]*"/, "", value); sub(/".*$/, "", value); print value; count++
} END { if (count != 1) exit 1 }' "$ROOT/src/agent.h") || fail 'cannot derive version'
uid=$(id -u)
gid=$(id -g)
deb=arqan_${version}-1_amd64.deb
rpm=arqan-${version}-1.x86_64.rpm
pkg=arqan-${version}-1-x86_64.pkg.tar.zst
archive=arqan-${version}-linux-x86_64.tar.gz
top=arqan-${version}-linux-x86_64

rm -rf -- "$ROOT/dist"
complete=false
cleanup() {
    if ! $complete; then rm -rf -- "$ROOT/dist"; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

release_in() {
    docker run --rm --platform linux/amd64 \
        --user "$uid:$gid" \
        -e HOME=/tmp/arqan-home \
        -e SOURCE_DATE_EPOCH="$epoch" \
        -v "$ROOT:/work" \
        -w /work \
        "$IMAGE" \
        sh -ec "$1"
}

docker build --platform linux/amd64 -f "$DOCKERFILE" -t "$IMAGE" "$ROOT/packaging/linux"
release_in 'make clean && make test'
# The deb comes from the Debian image, the rpm from the EL9 one and the
# portable archive from the musl one. Build those two pairs between the runs:
# `make clean` above would otherwise remove them, and packaging below requires
# them.
"$ROOT/scripts/build-el9.sh"
"$ROOT/scripts/build-musl.sh"
release_in 'make package-linux && make test-package-linux'

smoke_deb() {
    image=$1
    docker run --rm --platform linux/amd64 \
        -v "$ROOT/dist:/packages:ro" \
        "$image" sh -ec '
            sentinel=${ARQAN_SMOKE_SENTINEL:-arqan-package-sentinel}
            export DEBIAN_FRONTEND=noninteractive
            apt-get update >/dev/null
            apt-get install -y "/packages/'"$deb"'" >/dev/null
            arqan --version
            arqan-highlight --version
            mkdir -p /root/.config/arqan /root/.local/state/arqan \
                /root/.local/share/arqan/sessions /project/.arqan
            printf "%s\n" "$sentinel" >/root/.config/arqan/config.toml
            printf "%s\n" "$sentinel" >/root/.local/state/arqan/state.toml
            printf "%s\n" "$sentinel" >/root/.local/state/arqan/credentials.toml
            printf "%s\n" "$sentinel" >/root/.local/share/arqan/sessions/keep.json
            printf "%s\n" "$sentinel" >/project/.arqan/config.toml
            apt-get install -y --reinstall "/packages/'"$deb"'" >/dev/null
            arqan --version
            arqan-highlight --version
            apt-get remove -y arqan >/dev/null
            test ! -e /usr/bin/arqan && test ! -e /usr/bin/arqan-highlight
            test -z "$(find /usr/share/doc/arqan -type f -print -quit 2>/dev/null)"
            grep -qx "$sentinel" /root/.config/arqan/config.toml
            grep -qx "$sentinel" /root/.local/state/arqan/state.toml
            grep -qx "$sentinel" /root/.local/state/arqan/credentials.toml
            grep -qx "$sentinel" /root/.local/share/arqan/sessions/keep.json
            grep -qx "$sentinel" /project/.arqan/config.toml
        '
}

smoke_deb "$DEBIAN_IMAGE"
smoke_deb "$UBUNTU_IMAGE"
docker run --rm --platform linux/amd64 \
    -v "$ROOT/dist:/packages:ro" \
    "$FEDORA_IMAGE" sh -ec '
        sentinel=${ARQAN_SMOKE_SENTINEL:-arqan-package-sentinel}
        dnf install -y "/packages/'"$rpm"'" >/dev/null
        # A Debian-linked binary asks for versioned libcurl symbols this host
        # does not have, and the loader says so here. Nothing may reach stderr.
        for exe in arqan arqan-highlight; do
            noise=$($exe --version 2>&1 >/dev/null)
            [ -z "$noise" ] || { printf "%s: %s\n" "$exe" "$noise" >&2; exit 1; }
        done
        mkdir -p /root/.config/arqan /root/.local/state/arqan \
            /root/.local/share/arqan/sessions /project/.arqan
        printf "%s\n" "$sentinel" >/root/.config/arqan/config.toml
        printf "%s\n" "$sentinel" >/root/.local/state/arqan/state.toml
        printf "%s\n" "$sentinel" >/root/.local/state/arqan/credentials.toml
        printf "%s\n" "$sentinel" >/root/.local/share/arqan/sessions/keep.json
        printf "%s\n" "$sentinel" >/project/.arqan/config.toml
        dnf reinstall -y "/packages/'"$rpm"'" >/dev/null
        arqan --version
        arqan-highlight --version
        dnf remove -y arqan >/dev/null
        test ! -e /usr/bin/arqan && test ! -e /usr/bin/arqan-highlight
        test -z "$(find /usr/share/doc/arqan /usr/share/licenses/arqan \
            -type f -print -quit 2>/dev/null)"
        grep -qx "$sentinel" /root/.config/arqan/config.toml
        grep -qx "$sentinel" /root/.local/state/arqan/state.toml
        grep -qx "$sentinel" /root/.local/state/arqan/credentials.toml
        grep -qx "$sentinel" /root/.local/share/arqan/sessions/keep.json
        grep -qx "$sentinel" /project/.arqan/config.toml
    '

# The pacman package carries its own file manifest, so this run also asks
# pacman to verify every installed file against it. The image trims what it
# unpacks, and its NoExtract list would drop the documentation and report it
# as altered, so the smoke test installs the package whole.
docker run --rm --platform linux/amd64 \
    -v "$ROOT/dist:/packages:ro" \
    "$ARCH_IMAGE" sh -ec '
        sentinel=${ARQAN_SMOKE_SENTINEL:-arqan-package-sentinel}
        sed -i "/^NoExtract/d" /etc/pacman.conf
        pacman -U --noconfirm "/packages/'"$pkg"'" >/dev/null
        for exe in arqan arqan-highlight; do
            noise=$($exe --version 2>&1 >/dev/null)
            [ -z "$noise" ] || { printf "%s: %s\n" "$exe" "$noise" >&2; exit 1; }
        done
        arqan --version
        arqan-highlight --version
        pacman -Qkk arqan
        mkdir -p /root/.config/arqan /root/.local/state/arqan \
            /root/.local/share/arqan/sessions /project/.arqan
        printf "%s\n" "$sentinel" >/root/.config/arqan/config.toml
        printf "%s\n" "$sentinel" >/root/.local/state/arqan/state.toml
        printf "%s\n" "$sentinel" >/root/.local/state/arqan/credentials.toml
        printf "%s\n" "$sentinel" >/root/.local/share/arqan/sessions/keep.json
        printf "%s\n" "$sentinel" >/project/.arqan/config.toml
        pacman -U --noconfirm "/packages/'"$pkg"'" >/dev/null
        arqan --version
        arqan-highlight --version
        pacman -R --noconfirm arqan >/dev/null
        test ! -e /usr/bin/arqan && test ! -e /usr/bin/arqan-highlight
        test -z "$(find /usr/share/doc/arqan /usr/share/licenses/arqan \
            -type f -print -quit 2>/dev/null)"
        grep -qx "$sentinel" /root/.config/arqan/config.toml
        grep -qx "$sentinel" /root/.local/state/arqan/state.toml
        grep -qx "$sentinel" /root/.local/state/arqan/credentials.toml
        grep -qx "$sentinel" /root/.local/share/arqan/sessions/keep.json
        grep -qx "$sentinel" /project/.arqan/config.toml
    '

# The archive claims to need nothing, so prove it on a musl distribution and
# on the oldest glibc the packages target.
smoke_archive() {
    docker run --rm --platform linux/amd64 \
        -v "$ROOT/dist:/packages:ro" \
        "$1" sh -ec '
            tar -xzf "/packages/'"$archive"'" -C /tmp
            /tmp/'"$top"'/bin/arqan --version
            /tmp/'"$top"'/bin/arqan-highlight --version
        '
}

smoke_archive "$ALPINE_IMAGE"
smoke_archive "$DEBIAN_IMAGE"

set -- "$ROOT"/dist/*
[ "$#" -eq 5 ] || fail 'release build did not produce exactly five assets'
[ -f "$ROOT/dist/arqan-$version-linux-x86_64.tar.gz" ] || fail 'missing portable archive'
[ -f "$ROOT/dist/$deb" ] || fail 'missing Debian package'
[ -f "$ROOT/dist/$rpm" ] || fail 'missing RPM package'
[ -f "$ROOT/dist/$pkg" ] || fail 'missing pacman package'
[ -f "$ROOT/dist/SHA256SUMS" ] || fail 'missing checksum manifest'
(
    cd "$ROOT/dist"
    sha256sum --ignore-missing -c SHA256SUMS
)
complete=true
printf '%s\n' "Release files are in $ROOT/dist."
