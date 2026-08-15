#!/bin/sh
set -eu

PROGRAM=arqan
HEADER=src/agent.h
DIST=dist

fail() {
    printf '%s\n' "$PROGRAM package: $*" >&2
    exit 1
}

[ "$(uname -s)" = Linux ] || fail 'Linux host required'
case $(uname -m) in
    x86_64|amd64) ;;
    *) fail 'x86_64 host required' ;;
esac

for command in awk bsdtar cpio dpkg-deb du file find gzip readelf rpmbuild \
    rpm sha256sum strip tar zstd; do
    command -v "$command" >/dev/null 2>&1 || fail "missing required command: $command"
done

case $(dpkg-deb --version 2>/dev/null | head -1) in
    *'Debian'*'package archive'*) ;;
    *) fail 'GNU/Linux Debian dpkg-deb is required' ;;
esac

version=$(awk '/^[[:space:]]*#[[:space:]]*define[[:space:]]+AGENT_VERSION[[:space:]]+"/ {
    value=$0; sub(/^[^"]*"/, "", value); sub(/".*$/, "", value); print value; count++
} END { if (count != 1) exit 1 }' "$HEADER") || fail 'expected exactly one AGENT_VERSION'
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || fail "invalid AGENT_VERSION: $version"

if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    command -v git >/dev/null 2>&1 || fail 'SOURCE_DATE_EPOCH is unset and git is unavailable'
    SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD) || fail 'cannot derive commit timestamp'
fi
case $SOURCE_DATE_EPOCH in
    ''|*[!0-9]*) fail 'SOURCE_DATE_EPOCH must be a non-negative integer' ;;
esac
export SOURCE_DATE_EPOCH

archive=$PROGRAM-$version-linux-x86_64.tar.gz
deb=${PROGRAM}_${version}-1_amd64.deb
rpm=$PROGRAM-$version-1.x86_64.rpm
pkg=$PROGRAM-$version-1-x86_64.pkg.tar.zst
top=$PROGRAM-$version-linux-x86_64
work=${TMPDIR:-/tmp}/$PROGRAM-package.$$
payload=$work/payload
tar_stage=$work/$top
deb_stage=$work/deb
arch_stage=$work/arch
rpm_top=$PROGRAM-$version-package
rpm_source=$work/rpmbuild/SOURCES/$rpm_top
rpm_source_archive=$work/rpmbuild/SOURCES/v$version.tar.gz
trap 'rm -rf -- "$work"' EXIT
trap 'exit 1' HUP INT TERM
rm -rf -- "$work"
mkdir -p -- "$payload/bin" "$payload/static" "$payload/el9" \
    "$payload/doc/vendor/lexbor" \
    "$payload/doc/vendor/tree-sitter/licenses" \
    "$payload/doc/vendor/tree-sitter/runtime/unicode"

for exe in arqan arqan-highlight; do
    [ -x "bin/$exe" ] || fail "missing bin/$exe; run make first"
    cp -- "bin/$exe" "$payload/bin/$exe"
    chmod 0755 "$payload/bin/$exe"
    strip --strip-unneeded "$payload/bin/$exe"
done
# The rpm links the libcurl its own family ships. Debian versions libcurl's
# symbols and the rpm distributions do not, so the deb's binaries would ask
# every rpm host for a CURL_OPENSSL_4 it cannot provide. scripts/build-el9.sh
# produces bin/el9 from the EL9 builder image.
for exe in arqan arqan-highlight; do
    [ -x "bin/el9/$exe" ] || \
        fail "missing bin/el9/$exe; run scripts/build-el9.sh first"
    cp -- "bin/el9/$exe" "$payload/el9/$exe"
    chmod 0755 "$payload/el9/$exe"
    strip --strip-unneeded "$payload/el9/$exe"
done
# The native packages link against the distribution's libc and libcurl; the
# portable archive cannot, so it ships the musl static-pie build instead.
# scripts/build-musl.sh produces bin/musl from the Alpine builder image.
for exe in arqan arqan-highlight; do
    [ -x "bin/musl/$exe" ] || \
        fail "missing bin/musl/$exe; run scripts/build-musl.sh first"
    cp -- "bin/musl/$exe" "$payload/static/$exe"
    chmod 0755 "$payload/static/$exe"
    strip --strip-unneeded "$payload/static/$exe"
done
for path in README.md CHANGELOG.md LICENSE THIRD_PARTY_NOTICES.md \
    vendor/lexbor/LICENSE vendor/lexbor/NOTICE \
    vendor/tree-sitter/licenses/*.txt vendor/tree-sitter/runtime/unicode/LICENSE; do
    [ -f "$path" ] || fail "missing payload file: $path"
    mkdir -p -- "$payload/doc/$(dirname -- "$path")"
    cp -- "$path" "$payload/doc/$path"
    chmod 0644 "$payload/doc/$path"
done

"$payload/bin/arqan" --version >/dev/null || fail 'staged arqan diagnostic failed'
"$payload/bin/arqan-highlight" --version >/dev/null || fail 'staged arqan-highlight diagnostic failed'
"$payload/static/arqan" --version >/dev/null || fail 'staged static arqan diagnostic failed'
"$payload/static/arqan-highlight" --version >/dev/null || \
    fail 'staged static arqan-highlight diagnostic failed'
# The rpm's pair is not run here: it wants a newer glibc than this image has.
# The Fedora smoke test in scripts/release-linux.sh proves it runs.

verify_arch() {
    binary=$1
    label=$2
    file -L "$binary" | grep -Eq 'ELF 64-bit LSB.*x86-64' || fail "$label is not an x86_64 ELF"
    readelf -hW "$binary" | grep -Eq 'Machine:[[:space:]]+Advanced Micro Devices X86-64$' || \
        fail "$label has unexpected ELF architecture"
    if readelf -dW "$binary" | grep -Eq '\((RPATH|RUNPATH)\)'; then
        fail "$label contains RPATH or RUNPATH"
    fi
}

# A native package's binaries: needs_curl says whether this one links libcurl,
# max_allowed is the newest glibc symbol version the target distribution has,
# and resolvable says whether this image can load the result at all. The rpm's
# pair comes from EL9 and is neither loadable nor lddable here.
verify_elf() {
    binary=$1
    label=$2
    needs_curl=$3
    max_allowed=$4
    resolvable=$5
    verify_arch "$binary" "$label"
    needed=$(readelf -dW "$binary" | awk '/\(NEEDED\)/ { line=$0; sub(/^.*\[/, "", line); sub(/\].*$/, "", line); print line }')
    [ -n "$needed" ] || fail "$label has no shared-library dependencies"
    printf '%s\n' "$needed" | while IFS= read -r library; do
        case $library in
            libc.so.6) ;;
            libcurl.so.4) [ "$needs_curl" = yes ] || fail "$label unexpectedly needs $library" ;;
            *) fail "$label unexpectedly needs $library" ;;
        esac
    done
    [ "$(printf '%s\n' "$needed" | grep -c '^libc\.so\.6$')" -eq 1 ] || fail "$label must directly need libc.so.6"
    if [ "$needs_curl" = yes ]; then
        [ "$(printf '%s\n' "$needed" | grep -c '^libcurl\.so\.4$')" -eq 1 ] || \
            fail "$label must directly need libcurl.so.4"
    fi

    max_glibc=$(readelf --version-info -W "$binary" | \
        grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -Vu | tail -1)
    [ -n "$max_glibc" ] || fail "$label has no GLIBC symbol requirements"
    newest=$(printf '%s\n%s\n' "$max_glibc" "$max_allowed" | sort -Vu | tail -1)
    [ "$newest" = "$max_allowed" ] || \
        fail "$label requires GLIBC_$max_glibc (maximum is GLIBC_$max_allowed)"

    if [ "$resolvable" = yes ] && command -v ldd >/dev/null 2>&1; then
        if ! ldd "$binary" >"$work/ldd" 2>&1; then
            cat "$work/ldd" >&2
            fail "$label has unresolved dynamic libraries"
        fi
        ! grep -q 'not found' "$work/ldd" || fail "$label has unresolved dynamic libraries"
    fi
}

# The whole reason the rpm has its own builder: an rpm host's libcurl exports
# no versioned symbols, so a binary that asks for one makes its loader complain
# on every startup even though the call resolves.
verify_unversioned_curl() {
    binary=$1
    label=$2
    if readelf --version-info -W "$binary" | grep -q 'CURL_'; then
        fail "$label requires versioned libcurl symbols"
    fi
}

# The portable archive must run on any x86_64 Linux: no interpreter, no
# NEEDED entry, no versioned symbol floor, and still position independent.
# Ask readelf, not file: file 5.39 calls a static-pie binary "dynamically
# linked" because it is an ET_DYN object.
verify_static_elf() {
    binary=$1
    label=$2
    verify_arch "$binary" "$label"
    readelf -hW "$binary" | grep -Eq '^[[:space:]]*Type:[[:space:]]+DYN' || \
        fail "$label is not position independent"
    if readelf -dW "$binary" | grep -q '(NEEDED)'; then
        fail "$label still needs a shared library"
    fi
    if readelf -lW "$binary" | grep -q 'INTERP'; then
        fail "$label still names a program interpreter"
    fi
    if readelf --version-info -W "$binary" | grep -q 'GLIBC_'; then
        fail "$label carries glibc versioned symbols"
    fi
}
verify_elf "$payload/bin/arqan" 'deb arqan' yes 2.31 yes
verify_elf "$payload/bin/arqan-highlight" 'deb arqan-highlight' no 2.31 yes
verify_elf "$payload/el9/arqan" 'rpm arqan' yes 2.34 no
verify_elf "$payload/el9/arqan-highlight" 'rpm arqan-highlight' no 2.34 no
verify_unversioned_curl "$payload/el9/arqan" 'rpm arqan'
verify_unversioned_curl "$payload/el9/arqan-highlight" 'rpm arqan-highlight'
verify_static_elf "$payload/static/arqan" 'static arqan'
verify_static_elf "$payload/static/arqan-highlight" 'static arqan-highlight'

# Derive every format from the same stripped binaries and authoritative texts.
mkdir -p -- "$tar_stage/bin"
cp -a -- "$payload/static/." "$tar_stage/bin/"
cp -a -- "$payload/doc/." "$tar_stage/"

mkdir -p -- "$deb_stage/DEBIAN" "$deb_stage/usr/bin" "$deb_stage/usr/share/doc/$PROGRAM"
cp -a -- "$payload/bin/." "$deb_stage/usr/bin/"
cp -a -- "$payload/doc/." "$deb_stage/usr/share/doc/$PROGRAM/"
mv -- "$deb_stage/usr/share/doc/$PROGRAM/LICENSE" \
    "$deb_stage/usr/share/doc/$PROGRAM/copyright"
sed "s/@VERSION@/$version/g" packaging/linux/debian/control.in >"$deb_stage/DEBIAN/control"
chmod 0644 "$deb_stage/DEBIAN/control"

# The rpm and the pacman package ship the same tree: the same EL9 pair, whose
# libcurl symbols are unversioned the way every non-Debian family's are, and
# the same split of documentation from licence texts.
stage_usr_layout() {
    root=$1
    mkdir -p -- "$root/usr/bin" "$root/usr/share/doc/$PROGRAM" \
        "$root/usr/share/licenses/$PROGRAM"
    cp -a -- "$payload/el9/." "$root/usr/bin/"
    for name in README.md CHANGELOG.md THIRD_PARTY_NOTICES.md; do
        cp -a -- "$payload/doc/$name" "$root/usr/share/doc/$PROGRAM/$name"
    done
    mkdir -p -- "$root/usr/share/doc/$PROGRAM/vendor/lexbor"
    cp -a -- "$payload/doc/vendor/lexbor/NOTICE" \
        "$root/usr/share/doc/$PROGRAM/vendor/lexbor/NOTICE"
    cp -a -- "$payload/doc/LICENSE" "$root/usr/share/licenses/$PROGRAM/LICENSE"
    mkdir -p -- "$root/usr/share/licenses/$PROGRAM/vendor"
    cp -a -- "$payload/doc/vendor/lexbor" \
        "$root/usr/share/licenses/$PROGRAM/vendor/lexbor"
    rm -- "$root/usr/share/licenses/$PROGRAM/vendor/lexbor/NOTICE"
    cp -a -- "$payload/doc/vendor/tree-sitter" \
        "$root/usr/share/licenses/$PROGRAM/vendor/tree-sitter"
}

stage_usr_layout "$rpm_source"
stage_usr_layout "$arch_stage"

# pacman reads a package's metadata from leading dot-files in a plain tar, so
# libarchive writes one here and this image needs none of Arch's tooling. The
# installed size is what makepkg records: apparent size rounded to whole KiB.
arch_size=$(cd "$arch_stage" && du --apparent-size --block-size=1024 -s . | \
    awk '{ print $1 * 1024 }')
sed -e "s/@VERSION@/$version/g" -e "s/@BUILDDATE@/$SOURCE_DATE_EPOCH/g" \
    -e "s/@SIZE@/$arch_size/g" \
    packaging/linux/arch/PKGINFO.in >"$arch_stage/.PKGINFO"

# Fix all input metadata before invoking the format-specific archivers.
find "$work" -type d -exec chmod 0755 {} +
find "$work" -type f ! -path '*/bin/arqan' ! -path '*/bin/arqan-highlight' -exec chmod 0644 {} +
find "$work" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +

mkdir -p -- "$DIST"
rm -f -- "$DIST"/*.tar.gz "$DIST"/*.deb "$DIST"/*.rpm "$DIST"/*.pkg.tar.zst \
    "$DIST"/*.sha256 "$DIST/SHA256SUMS"
TZ=UTC tar --sort=name --format=ustar --owner=0 --group=0 --numeric-owner \
    --mtime="@$SOURCE_DATE_EPOCH" --mode='u+rwX,go+rX,go-w' \
    -C "$work" -cf - "$top" | gzip -n >"$DIST/$archive"

dpkg-deb --root-owner-group -Zgzip -z9 --build "$deb_stage" "$DIST/$deb" >/dev/null

mkdir -p -- "$work/rpmbuild/BUILD" "$work/rpmbuild/BUILDROOT" \
    "$work/rpmbuild/RPMS" "$work/rpmbuild/SPECS" "$work/rpmbuild/SRPMS"
TZ=UTC tar --sort=name --format=gnu --owner=0 --group=0 --numeric-owner \
    --mtime="@$SOURCE_DATE_EPOCH" --mode='u+rwX,go+rX,go-w' \
    -C "$work/rpmbuild/SOURCES" -cf - "$rpm_top" | gzip -n \
    >"$rpm_source_archive"
sed "s/@VERSION@/$version/g" packaging/linux/arqan.spec.in >"$work/rpmbuild/SPECS/arqan.spec"
TZ=UTC rpmbuild -bb "$work/rpmbuild/SPECS/arqan.spec" \
    --define "_topdir $work/rpmbuild" \
    --define "_buildhost reproducible.invalid" \
    --define "_binary_payload w9.gzdio" \
    --define "_source_payload w9.gzdio" \
    --define "clamp_mtime_to_source_date_epoch 1" \
    --define "use_source_date_epoch_as_buildtime 1" \
    --define "source_date_epoch_from_changelog 0" \
    --define "_build_id_links none" >/dev/null
built_rpm=$(find "$work/rpmbuild/RPMS" -type f -name '*.rpm')
[ "$(printf '%s\n' "$built_rpm" | wc -l)" -eq 1 ] || fail 'rpmbuild produced an unexpected artifact set'
cp -- "$built_rpm" "$DIST/$rpm"

# .MTREE describes the payload and .PKGINFO alongside it, so it is written
# once both are final. pacman stops reading metadata at the first entry whose
# name does not begin with a dot, and C-sorted names put those entries first.
(
    cd "$arch_stage"
    LC_ALL=C bsdtar -cf - --format=mtree --uid 0 --gid 0 \
        --options='!all,use-set,type,uid,gid,mode,time,size,md5,sha256,link' \
        .PKGINFO usr | gzip -n >.MTREE
    touch -h -d "@$SOURCE_DATE_EPOCH" .MTREE
    LC_ALL=C TZ=UTC tar --sort=name --format=gnu --owner=0 --group=0 \
        --numeric-owner --mtime="@$SOURCE_DATE_EPOCH" \
        --mode='u+rwX,go+rX,go-w' -cf - .PKGINFO .MTREE usr | \
        zstd -19 -T1 -q -c
) >"$DIST/$pkg"

(
    cd "$DIST"
    sha256sum "$archive" "$deb" "$rpm" "$pkg" | LC_ALL=C sort -k2 >SHA256SUMS
)
printf '%s\n' "$DIST/$archive" "$DIST/$deb" "$DIST/$rpm" "$DIST/$pkg" \
    "$DIST/SHA256SUMS"
