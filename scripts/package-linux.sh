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

for command in awk file gzip readelf sha256sum strip tar; do
    command -v "$command" >/dev/null 2>&1 || fail "missing required command: $command"
done

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
top=$PROGRAM-$version-linux-x86_64
work=${TMPDIR:-/tmp}/$PROGRAM-package.$$
stage=$work/$top
trap 'rm -rf -- "$work"' EXIT
trap 'exit 1' HUP INT TERM
rm -rf -- "$work"
mkdir -p -- "$stage/bin" "$stage/vendor/lexbor" \
    "$stage/vendor/tree-sitter/licenses" "$stage/vendor/tree-sitter/runtime/unicode"

for exe in arqan arqan-highlight; do
    [ -x "bin/$exe" ] || fail "missing bin/$exe; run make first"
    cp -- "bin/$exe" "$stage/bin/$exe"
    chmod 0755 "$stage/bin/$exe"
    strip --strip-unneeded "$stage/bin/$exe"
done
for path in install.sh README.md CHANGELOG.md LICENSE THIRD_PARTY_NOTICES.md \
    vendor/lexbor/LICENSE vendor/lexbor/NOTICE \
    vendor/tree-sitter/licenses/*.txt vendor/tree-sitter/runtime/unicode/LICENSE; do
    [ -f "$path" ] || fail "missing payload file: $path"
    cp -- "$path" "$stage/$path"
    chmod 0644 "$stage/$path"
done
chmod 0755 "$stage/install.sh"

"$stage/bin/arqan" --version >/dev/null || fail 'staged arqan diagnostic failed'
"$stage/bin/arqan-highlight" --version >/dev/null || fail 'staged arqan-highlight diagnostic failed'

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
verify_elf "$stage/bin/arqan" arqan
verify_elf "$stage/bin/arqan-highlight" arqan-highlight

# GNU tar's archive metadata is fixed independently of the checkout and umask.
find "$stage" -type d -exec chmod 0755 {} +
mkdir -p -- "$DIST"
rm -f -- "$DIST/$archive" "$DIST/$archive.sha256"
TZ=UTC tar --sort=name --format=ustar --owner=0 --group=0 --numeric-owner \
    --mtime="@$SOURCE_DATE_EPOCH" --mode='u+rwX,go+rX,go-w' \
    -C "$work" -cf - "$top" | gzip -n >"$DIST/$archive"
(
    cd "$DIST"
    sha256sum "$archive" >"$archive.sha256"
)
printf '%s\n' "$DIST/$archive" "$DIST/$archive.sha256"
