#!/bin/sh
set -eu

PROGRAM=arqan
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DOCKERFILE=$ROOT/packaging/linux/Dockerfile.repo
IMAGE=${ARQAN_REPO_IMAGE:-arqan-linux-repo:ubuntu2404}
ARCH_IMAGE=archlinux:base@sha256:b0deabeb3d283da2c7f7dbf0eea051b7b2cd0554e0b737cc457fd21683bdcdd1
SLUG=${ARQAN_REPO_SLUG:-bissakov/arqan}
BASE_URL=${ARQAN_REPO_URL:-https://bissakov.github.io/arqan}
KEEP=${ARQAN_REPO_KEEP:-10}
OUT=${ARQAN_REPO_OUT:-$ROOT/dist/pages}
ASSETS=${ARQAN_REPO_ASSETS:-}
KEY=${ARQAN_REPO_SIGNING_KEY:-}
DEB_ARCH=amd64
PKG_ARCH=x86_64
SUITE=stable
COMPONENT=main

fail() {
    printf '%s\n' "$PROGRAM repos: $*" >&2
    exit 1
}

note() {
    printf '%s\n' "$PROGRAM repos: $*" >&2
}

command -v docker >/dev/null 2>&1 || fail 'Docker is required'
docker info >/dev/null 2>&1 || fail 'Docker daemon is unavailable'
command -v sha256sum >/dev/null 2>&1 || fail 'sha256sum is required'
[ -n "$KEY" ] || fail 'set ARQAN_REPO_SIGNING_KEY to an armored private key file'
[ -r "$KEY" ] || fail "cannot read signing key $KEY"
case $BASE_URL in
    https://*) ;;
    http://127.0.0.1*|http://localhost*) note 'plain http, for a dry run only' ;;
    *) fail "ARQAN_REPO_URL must be an https URL, got $BASE_URL" ;;
esac
BASE_URL=${BASE_URL%/}
uid=$(id -u)
gid=$(id -g)

work=$(mktemp -d "${TMPDIR:-/tmp}/arqan-repos.XXXXXX") || fail 'cannot create a work directory'
complete=false
cleanup() {
    rm -rf -- "$work"
    if ! $complete; then rm -rf -- "$OUT"; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# ---------------------------------------------------------------------------
# Collect the packages of the releases the repositories will carry
# ---------------------------------------------------------------------------

if [ -n "$ASSETS" ]; then
    [ -d "$ASSETS" ] || fail "ARQAN_REPO_ASSETS is not a directory: $ASSETS"
    tags=$(ls -1 "$ASSETS")
else
    command -v gh >/dev/null 2>&1 || fail 'the GitHub CLI is required to fetch releases'
    tags=$(gh release list --repo "$SLUG" --limit "$KEEP" \
        --exclude-drafts --exclude-pre-releases \
        --json tagName --jq '.[].tagName') ||
        fail "cannot list the releases of $SLUG"
fi
[ -n "$tags" ] || fail 'no releases to publish'

carried=0
mkdir -p "$work/assets"
for tag in $tags; do
    dir=$work/assets/$tag
    if [ -n "$ASSETS" ]; then
        cp -R -- "$ASSETS/$tag" "$dir"
    else
        mkdir -p "$dir"
        gh release download "$tag" --repo "$SLUG" --dir "$dir" --clobber \
            -p '*.deb' -p '*.rpm' -p '*.pkg.tar.zst' -p 'SHA256SUMS' \
            >/dev/null 2>&1 || {
            note "$tag carries no Linux packages, skipping"
            rm -rf -- "$dir"
            continue
        }
    fi
    if [ ! -f "$dir/SHA256SUMS" ]; then
        note "$tag carries no SHA256SUMS, skipping"
        rm -rf -- "$dir"
        continue
    fi
    (cd "$dir" && sha256sum --ignore-missing -c SHA256SUMS >/dev/null) ||
        fail "$tag does not match its SHA256SUMS"
    carried=$((carried + 1))
done
[ "$carried" -gt 0 ] || fail 'no release carries Linux packages'

# ---------------------------------------------------------------------------
# Lay the site out
# ---------------------------------------------------------------------------

pool=$OUT/deb/pool/$COMPONENT/a/$PROGRAM
binary=$OUT/deb/dists/$SUITE/$COMPONENT/binary-$DEB_ARCH
rpmdir=$OUT/rpm/$PKG_ARCH
archdir=$OUT/arch/$PKG_ARCH

rm -rf -- "$OUT"
mkdir -p "$pool" "$binary" "$rpmdir" "$archdir"
find "$work/assets" -name '*.deb' -exec cp -- {} "$pool/" \;
find "$work/assets" -name '*.rpm' -exec cp -- {} "$rpmdir/" \;
find "$work/assets" -name '*.pkg.tar.zst' -exec cp -- {} "$archdir/" \;

mkdir -p "$work/keys"
chmod 700 "$work/keys"
cp -- "$KEY" "$work/keys/signing-key.asc"
chmod 600 "$work/keys/signing-key.asc"

repo_in() {
    docker run --rm --platform linux/amd64 \
        --user "$uid:$gid" \
        -e HOME=/tmp/arqan-home \
        -e OUT=/repo \
        -e KEYDIR=/keys \
        -e PROGRAM="$PROGRAM" \
        -e ORIGIN="$PROGRAM" \
        -v "$OUT:/repo" \
        -v "$work/keys:/keys" \
        -v "$ROOT/packaging/linux/repo-metadata.sh:/repo-metadata.sh:ro" \
        "$IMAGE" \
        /repo-metadata.sh "$1"
}

docker build --platform linux/amd64 -f "$DOCKERFILE" -t "$IMAGE" "$ROOT/packaging/linux"
repo_in sign

# repo-add embeds the detached package signatures written above, so the pacman
# database is built between the two metadata phases.
docker run --rm --platform linux/amd64 \
    --user "$uid:$gid" \
    -e HOME=/tmp/arqan-home \
    -v "$archdir:/work" \
    -w /work \
    "$ARCH_IMAGE" \
    sh -ec "repo-add --quiet $PROGRAM.db.tar.gz *.pkg.tar.zst" >/dev/null

repo_in index

fingerprint=$(cat "$work/keys/fingerprint") || fail 'the signing key exposed no fingerprint'
keyring=$PROGRAM-archive-keyring.asc
[ -s "$OUT/$keyring" ] || fail 'the public key was not exported'

# ---------------------------------------------------------------------------
# Client configuration and the landing page
# ---------------------------------------------------------------------------

: >"$OUT/.nojekyll"

cat >"$OUT/deb/$PROGRAM.sources" <<EOF
Types: deb
URIs: $BASE_URL/deb
Suites: $SUITE
Components: $COMPONENT
Architectures: $DEB_ARCH
Signed-By: /etc/apt/keyrings/$keyring
EOF

cat >"$OUT/rpm/$PROGRAM.repo" <<EOF
[$PROGRAM]
name=$PROGRAM
baseurl=$BASE_URL/rpm/\$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=$BASE_URL/$keyring
EOF

cat >"$OUT/index.html" <<EOF
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>$PROGRAM package repositories</title>
<style>
body { max-width: 46rem; margin: 2rem auto; padding: 0 1rem;
       font-family: system-ui, sans-serif; line-height: 1.5; }
pre { background: #f4f4f4; padding: 0.75rem; overflow-x: auto; }
code { font-family: ui-monospace, monospace; }
</style>
</head>
<body>
<h1>$PROGRAM package repositories</h1>
<p>Signed apt, dnf and pacman repositories for
<a href="https://github.com/$SLUG">$PROGRAM</a>, x86_64 only. Every index is
signed by <code>$fingerprint</code>.</p>

<h2>Debian and Ubuntu</h2>
<pre><code>sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSLo /etc/apt/keyrings/$keyring $BASE_URL/$keyring
sudo curl -fsSLo /etc/apt/sources.list.d/$PROGRAM.sources $BASE_URL/deb/$PROGRAM.sources
sudo apt update
sudo apt install $PROGRAM</code></pre>

<h2>Fedora and RHEL</h2>
<pre><code>sudo rpm --import $BASE_URL/$keyring
sudo curl -fsSLo /etc/yum.repos.d/$PROGRAM.repo $BASE_URL/rpm/$PROGRAM.repo
sudo dnf install $PROGRAM</code></pre>

<h2>Arch</h2>
<pre><code>curl -fsSLo /tmp/$keyring $BASE_URL/$keyring
sudo pacman-key --add /tmp/$keyring
sudo pacman-key --lsign-key $fingerprint</code></pre>
<p>Append to <code>/etc/pacman.conf</code>:</p>
<pre><code>[$PROGRAM]
Server = $BASE_URL/arch/\$arch</code></pre>
<pre><code>sudo pacman -Sy $PROGRAM</code></pre>

<h2>Without a repository</h2>
<p>Single packages and the portable archive are attached to each
<a href="https://github.com/$SLUG/releases">release</a>.</p>
</body>
</html>
EOF

complete=true
printf 'published %s release(s) to %s for %s\n' "$carried" "$OUT" "$BASE_URL"
printf 'signing key %s\n' "$fingerprint"
