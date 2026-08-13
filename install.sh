#!/bin/sh
set -eu

PROGRAM=arqan
if [ "${PREFIX+x}" = x ]; then
    prefix_set=true
else
    PREFIX=
    prefix_set=false
fi
DESTDIR=${DESTDIR:-}
uninstall=false

usage() {
    cat <<'EOF'
usage: ./install.sh [--prefix PATH] [--destdir PATH] [--uninstall] [--help]

Install arqan and arqan-highlight under PATH (default: $HOME/.local).
DESTDIR stages the same prefix-relative tree without changing the prefix.
The installer never invokes sudo; use sudo explicitly for a system install.
EOF
}

need_value() {
    if [ "$#" -lt 2 ]; then
        printf '%s\n' "$PROGRAM: option '$1' requires a path" >&2
        exit 2
    fi
}

while [ "$#" -gt 0 ]; do
    case $1 in
        --prefix)
            need_value "$@"
            PREFIX=$2
            prefix_set=true
            shift 2
            ;;
        --prefix=*) PREFIX=${1#*=}; prefix_set=true; shift ;;
        --destdir)
            need_value "$@"
            DESTDIR=$2
            shift 2
            ;;
        --destdir=*) DESTDIR=${1#*=}; shift ;;
        --uninstall) uninstall=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *)
            printf '%s\n' "$PROGRAM: unknown option '$1'" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if ! $prefix_set; then
    if [ -z "${HOME:-}" ]; then
        printf '%s\n' "$PROGRAM: HOME is not set; use --prefix with an absolute path" >&2
        exit 2
    fi
    PREFIX=$HOME/.local
fi

case $PREFIX in
    /*) ;;
    *) printf '%s\n' "$PROGRAM: prefix must be an absolute path: $PREFIX" >&2; exit 2 ;;
esac
if [ -n "$DESTDIR" ]; then
    case $DESTDIR in
        /*) ;;
        *) printf '%s\n' "$PROGRAM: destdir must be an absolute path: $DESTDIR" >&2; exit 2 ;;
    esac
fi

root=${DESTDIR}${PREFIX}
bindir=$root/bin
docdir=$root/share/doc/$PROGRAM

payload='README.md
CHANGELOG.md
LICENSE
THIRD_PARTY_NOTICES.md
vendor/lexbor/LICENSE
vendor/lexbor/NOTICE
vendor/tree-sitter/licenses/bash.txt
vendor/tree-sitter/licenses/c.txt
vendor/tree-sitter/licenses/cpp.txt
vendor/tree-sitter/licenses/go.txt
vendor/tree-sitter/licenses/javascript.txt
vendor/tree-sitter/licenses/json.txt
vendor/tree-sitter/licenses/python.txt
vendor/tree-sitter/licenses/rust.txt
vendor/tree-sitter/licenses/toml.txt
vendor/tree-sitter/licenses/tree-sitter.txt
vendor/tree-sitter/licenses/typescript.txt
vendor/tree-sitter/licenses/yaml.txt
vendor/tree-sitter/runtime/unicode/LICENSE'

if $uninstall; then
    rm -f -- "$bindir/arqan" "$bindir/arqan-highlight"
    for path in $payload; do
        rm -f -- "$docdir/$path"
    done

    rmdir "$docdir/vendor/tree-sitter/runtime/unicode" 2>/dev/null || :
    rmdir "$docdir/vendor/tree-sitter/runtime" 2>/dev/null || :
    rmdir "$docdir/vendor/tree-sitter/licenses" 2>/dev/null || :
    rmdir "$docdir/vendor/tree-sitter" 2>/dev/null || :
    rmdir "$docdir/vendor/lexbor" 2>/dev/null || :
    rmdir "$docdir/vendor" 2>/dev/null || :
    rmdir "$docdir" 2>/dev/null || :
    rmdir "$root/share/doc" 2>/dev/null || :
    rmdir "$root/share" 2>/dev/null || :
    rmdir "$bindir" 2>/dev/null || :
    printf '%s\n' "Uninstalled $PROGRAM from $PREFIX."
    exit 0
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for exe in arqan arqan-highlight; do
    source_file=$script_dir/bin/$exe
    if [ ! -f "$source_file" ] || [ ! -x "$source_file" ]; then
        printf '%s\n' "$PROGRAM: missing executable $source_file; run 'make' first" >&2
        exit 1
    fi
    if ! "$source_file" --version >/dev/null; then
        printf '%s\n' "$PROGRAM: diagnostic failed: $source_file --version" >&2
        exit 1
    fi
done
for path in $payload; do
    if [ ! -f "$script_dir/$path" ]; then
        printf '%s\n' "$PROGRAM: missing payload file $script_dir/$path" >&2
        exit 1
    fi
done

current_tmp=
cleanup() {
    if [ -n "$current_tmp" ]; then rm -f -- "$current_tmp"; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

atomic_install() {
    source_file=$1
    target_file=$2
    mode=$3
    target_dir=$(dirname -- "$target_file")
    mkdir -p -- "$target_dir"
    current_tmp=$target_dir/.arqan-install.$$
    rm -f -- "$current_tmp"
    cp -- "$source_file" "$current_tmp"
    chmod "$mode" "$current_tmp"
    mv -f -- "$current_tmp" "$target_file"
    current_tmp=
}

atomic_install "$script_dir/bin/arqan" "$bindir/arqan" 0755
atomic_install "$script_dir/bin/arqan-highlight" "$bindir/arqan-highlight" 0755
for path in $payload; do
    atomic_install "$script_dir/$path" "$docdir/$path" 0644
done

printf '%s\n' "Installed $PROGRAM and arqan-highlight under $PREFIX."
if [ -z "$DESTDIR" ] && [ "${HOME:-}" ] && [ "$PREFIX" = "$HOME/.local" ]; then
    case :${PATH:-}: in
        *:"$bindir":*) ;;
        *)
            printf '%s\n' "Add $bindir to PATH to run $PROGRAM by name."
            ;;
    esac
fi
