#!/bin/sh
set -eu

OUT=${OUT:-/repo}
KEYDIR=${KEYDIR:-/keys}
PROGRAM=${PROGRAM:-arqan}
ORIGIN=${ORIGIN:-$PROGRAM}
DEB_ARCH=amd64
PKG_ARCH=x86_64
SUITE=stable
COMPONENT=main

GNUPGHOME=$KEYDIR/gnupg
export GNUPGHOME

fail() {
    printf '%s\n' "$PROGRAM metadata: $*" >&2
    exit 1
}

import_key() {
    mkdir -p "$GNUPGHOME"
    chmod 700 "$GNUPGHOME"
    gpg --batch --quiet --import "$KEYDIR/signing-key.asc" ||
        fail 'cannot import the signing key'
    fingerprint=$(gpg --batch --with-colons --list-secret-keys |
        awk -F: '$1 == "fpr" { print $10; exit }')
    [ -n "$fingerprint" ] || fail 'the signing key holds no secret half'
}

sign_detached() {
    rm -f -- "$1.sig"
    gpg --batch --yes --quiet --pinentry-mode loopback \
        --local-user "$fingerprint" --detach-sign --output "$1.sig" "$1"
}

sign_armored() {
    rm -f -- "$1.asc"
    gpg --batch --yes --quiet --pinentry-mode loopback \
        --local-user "$fingerprint" --detach-sign --armor \
        --output "$1.asc" "$1"
}

sign_packages() {
    gpg --batch --armor --export "$fingerprint" \
        >"$OUT/$PROGRAM-archive-keyring.asc"
    printf '%s\n' "$fingerprint" >"$KEYDIR/fingerprint"

    for pkg in "$OUT/arch/$PKG_ARCH"/*.pkg.tar.zst; do
        [ -e "$pkg" ] || fail 'no pacman package to sign'
        sign_detached "$pkg"
    done
    for pkg in "$OUT/rpm/$PKG_ARCH"/*.rpm; do
        [ -e "$pkg" ] || fail 'no rpm package to sign'
        # Ubuntu ships gpg, not the gpg2 name rpm's %__gpg defaults to.
        rpmsign --define "__gpg $(command -v gpg)" \
            --define "_gpg_name $fingerprint" --addsign "$pkg" >/dev/null
    done
}

index_deb() {
    cd "$OUT/deb"
    dist=dists/$SUITE
    binary=$dist/$COMPONENT/binary-$DEB_ARCH
    rm -f "$dist/Release" "$dist/InRelease" "$dist/Release.gpg"
    apt-ftparchive --arch "$DEB_ARCH" packages pool >"$binary/Packages"
    gzip -9nc <"$binary/Packages" >"$binary/Packages.gz"
    xz -9c <"$binary/Packages" >"$binary/Packages.xz"
    apt-ftparchive \
        -o "APT::FTPArchive::Release::Origin=$ORIGIN" \
        -o "APT::FTPArchive::Release::Label=$ORIGIN" \
        -o "APT::FTPArchive::Release::Suite=$SUITE" \
        -o "APT::FTPArchive::Release::Codename=$SUITE" \
        -o "APT::FTPArchive::Release::Architectures=$DEB_ARCH" \
        -o "APT::FTPArchive::Release::Components=$COMPONENT" \
        -o "APT::FTPArchive::Release::Description=$PROGRAM releases" \
        release "$dist" >"$KEYDIR/Release"
    mv "$KEYDIR/Release" "$dist/Release"
    gpg --batch --yes --quiet --pinentry-mode loopback \
        --local-user "$fingerprint" --clearsign \
        --output "$dist/InRelease" "$dist/Release"
    gpg --batch --yes --quiet --pinentry-mode loopback \
        --local-user "$fingerprint" --detach-sign --armor \
        --output "$dist/Release.gpg" "$dist/Release"
}

index_rpm() {
    rm -rf -- "$OUT/rpm/$PKG_ARCH/repodata"
    createrepo_c --quiet "$OUT/rpm/$PKG_ARCH"
    sign_armored "$OUT/rpm/$PKG_ARCH/repodata/repomd.xml"
}

index_arch() {
    dir=$OUT/arch/$PKG_ARCH
    for kind in db files; do
        archive=$dir/$PROGRAM.$kind.tar.gz
        [ -f "$archive" ] || fail "repo-add wrote no $PROGRAM.$kind.tar.gz"
        sign_detached "$archive"
        # pacman fetches the extensionless names, which repo-add leaves as
        # symlinks the Pages artifact would not carry.
        rm -f -- "$dir/$PROGRAM.$kind" "$dir/$PROGRAM.$kind.sig"
        cp -- "$archive" "$dir/$PROGRAM.$kind"
        cp -- "$archive.sig" "$dir/$PROGRAM.$kind.sig"
    done
}

case ${1:-} in
    sign)
        import_key
        sign_packages
        ;;
    index)
        import_key
        index_deb
        index_rpm
        index_arch
        ;;
    *)
        fail 'usage: repo-metadata.sh sign|index'
        ;;
esac
