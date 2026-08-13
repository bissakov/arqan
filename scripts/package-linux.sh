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

for command in awk cpio dpkg-deb file find gzip readelf rpmbuild rpm sha256sum strip tar; do
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
top=$PROGRAM-$version-linux-x86_64
work=${TMPDIR:-/tmp}/$PROGRAM-package.$$
payload=$work/payload
tar_stage=$work/$top
deb_stage=$work/deb
rpm_top=$PROGRAM-$version-package
rpm_source=$work/rpmbuild/SOURCES/$rpm_top
rpm_source_archive=$work/rpmbuild/SOURCES/v$version.tar.gz
trap 'rm -rf -- "$work"' EXIT
trap 'exit 1' HUP INT TERM
rm -rf -- "$work"
mkdir -p -- "$payload/bin" "$payload/doc/vendor/lexbor" \
    "$payload/doc/vendor/tree-sitter/licenses" \
    "$payload/doc/vendor/tree-sitter/runtime/unicode"

for exe in arqan arqan-highlight; do
    [ -x "bin/$exe" ] || fail "missing bin/$exe; run make first"
    cp -- "bin/$exe" "$payload/bin/$exe"
    chmod 0755 "$payload/bin/$exe"
    strip --strip-unneeded "$payload/bin/$exe"
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

verify_elf() {
    binary=$1
    label=$2
    file -L "$binary" | grep -Eq 'ELF 64-bit LSB.*x86-64' || fail "$label is not an x86_64 ELF"
    readelf -hW "$binary" | grep -Eq 'Machine:[[:space:]]+Advanced Micro Devices X86-64$' || \
        fail "$label has unexpected ELF architecture"
    if readelf -dW "$binary" | grep -Eq '\((RPATH|RUNPATH)\)'; then
        fail "$label contains RPATH or RUNPATH"
    fi
    needed=$(readelf -dW "$binary" | awk '/\(NEEDED\)/ { line=$0; sub(/^.*\[/, "", line); sub(/\].*$/, "", line); print line }')
    [ -n "$needed" ] || fail "$label has no shared-library dependencies"
    printf '%s\n' "$needed" | while IFS= read -r library; do
        case $library in
            libc.so.6) ;;
            libcurl.so.4) [ "$label" = arqan ] || fail "$label unexpectedly needs $library" ;;
            *) fail "$label unexpectedly needs $library" ;;
        esac
    done
    [ "$(printf '%s\n' "$needed" | grep -c '^libc\.so\.6$')" -eq 1 ] || fail "$label must directly need libc.so.6"
    if [ "$label" = arqan ]; then
        [ "$(printf '%s\n' "$needed" | grep -c '^libcurl\.so\.4$')" -eq 1 ] || fail 'arqan must directly need libcurl.so.4'
    fi

    max_glibc=$(readelf --version-info -W "$binary" | \
        grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -Vu | tail -1)
    [ -n "$max_glibc" ] || fail "$label has no GLIBC symbol requirements"
    newest=$(printf '%s\n%s\n' "$max_glibc" 2.31 | sort -Vu | tail -1)
    [ "$newest" = 2.31 ] || fail "$label requires GLIBC_$max_glibc (maximum is GLIBC_2.31)"

    if command -v ldd >/dev/null 2>&1; then
        if ! ldd "$binary" >"$work/ldd" 2>&1; then
            cat "$work/ldd" >&2
            fail "$label has unresolved dynamic libraries"
        fi
        ! grep -q 'not found' "$work/ldd" || fail "$label has unresolved dynamic libraries"
    fi
}
verify_elf "$payload/bin/arqan" arqan
verify_elf "$payload/bin/arqan-highlight" arqan-highlight

# Derive every format from the same stripped binaries and authoritative texts.
mkdir -p -- "$tar_stage/bin"
cp -a -- "$payload/bin/." "$tar_stage/bin/"
cp -a -- "$payload/doc/." "$tar_stage/"

mkdir -p -- "$deb_stage/DEBIAN" "$deb_stage/usr/bin" "$deb_stage/usr/share/doc/$PROGRAM"
cp -a -- "$payload/bin/." "$deb_stage/usr/bin/"
cp -a -- "$payload/doc/." "$deb_stage/usr/share/doc/$PROGRAM/"
mv -- "$deb_stage/usr/share/doc/$PROGRAM/LICENSE" \
    "$deb_stage/usr/share/doc/$PROGRAM/copyright"
sed "s/@VERSION@/$version/g" packaging/linux/debian/control.in >"$deb_stage/DEBIAN/control"
chmod 0644 "$deb_stage/DEBIAN/control"

mkdir -p -- "$rpm_source/usr/bin" "$rpm_source/usr/share/doc/$PROGRAM" \
    "$rpm_source/usr/share/licenses/$PROGRAM"
cp -a -- "$payload/bin/." "$rpm_source/usr/bin/"
for name in README.md CHANGELOG.md THIRD_PARTY_NOTICES.md; do
    cp -a -- "$payload/doc/$name" "$rpm_source/usr/share/doc/$PROGRAM/$name"
done
mkdir -p -- "$rpm_source/usr/share/doc/$PROGRAM/vendor/lexbor"
cp -a -- "$payload/doc/vendor/lexbor/NOTICE" \
    "$rpm_source/usr/share/doc/$PROGRAM/vendor/lexbor/NOTICE"
cp -a -- "$payload/doc/LICENSE" "$rpm_source/usr/share/licenses/$PROGRAM/LICENSE"
mkdir -p -- "$rpm_source/usr/share/licenses/$PROGRAM/vendor"
cp -a -- "$payload/doc/vendor/lexbor" \
    "$rpm_source/usr/share/licenses/$PROGRAM/vendor/lexbor"
rm -- "$rpm_source/usr/share/licenses/$PROGRAM/vendor/lexbor/NOTICE"
cp -a -- "$payload/doc/vendor/tree-sitter" \
    "$rpm_source/usr/share/licenses/$PROGRAM/vendor/tree-sitter"

# Fix all input metadata before invoking the format-specific archivers.
find "$work" -type d -exec chmod 0755 {} +
find "$work" -type f ! -path '*/bin/arqan' ! -path '*/bin/arqan-highlight' -exec chmod 0644 {} +
find "$work" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +

mkdir -p -- "$DIST"
rm -f -- "$DIST"/*.tar.gz "$DIST"/*.deb "$DIST"/*.rpm \
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

(
    cd "$DIST"
    sha256sum "$archive" "$deb" "$rpm" | LC_ALL=C sort -k2 >SHA256SUMS
)
printf '%s\n' "$DIST/$archive" "$DIST/$deb" "$DIST/$rpm" "$DIST/SHA256SUMS"
