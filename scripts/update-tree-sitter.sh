#!/bin/sh
# Refresh the offline Tree-sitter runtime, parsers, queries and licenses.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCES="$ROOT/scripts/tree-sitter-sources.tsv"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/arqan-tree-sitter.XXXXXX")
OLD="$ROOT/vendor/tree-sitter"
BACKUP="$WORK/old-vendor"
COMMITTED=false
cleanup() {
    if [ "$COMMITTED" != true ] && [ -e "$BACKUP" ]; then
        rm -rf "$OLD"
        mv "$BACKUP" "$OLD"
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

GENERATOR_URL=https://github.com/tree-sitter/tree-sitter/releases/download/v0.26.11/tree-sitter-linux-x64.gz
GENERATOR_SHA=8dac3c89bb632eece700ea7a261ad963b251f2228c4aef3b58458ebea8dbe4eb

fetch() {
    role=$1 owner=$2 repo=$3 ref=$4 expected=$5
    archive="$WORK/$role.tar.gz"
    curl -L --fail --silent --show-error \
        "https://codeload.github.com/$owner/$repo/tar.gz/$ref" -o "$archive"
    actual=$(sha256sum "$archive" | awk '{print $1}')
    if [ "$actual" != "$expected" ]; then
        echo "$role: archive checksum mismatch" >&2
        exit 1
    fi
    mkdir -p "$WORK/src/$role"
    tar -xzf "$archive" -C "$WORK/src/$role" --strip-components=1
}

while IFS="	" read -r role owner repo ref checksum; do
    case "$role" in ''|'#'*) continue ;; esac
    fetch "$role" "$owner" "$repo" "$ref" "$checksum"
done < "$SOURCES"

curl -L --fail --silent --show-error "$GENERATOR_URL" -o "$WORK/generator.gz"
actual=$(sha256sum "$WORK/generator.gz" | awk '{print $1}')
if [ "$actual" != "$GENERATOR_SHA" ]; then
    echo "tree-sitter generator checksum mismatch" >&2
    exit 1
fi
gzip -dc "$WORK/generator.gz" > "$WORK/tree-sitter"
chmod 700 "$WORK/tree-sitter"

# Generate every parser at ABI 15. C++ is pinned to an already generated ABI
# 15 commit because its grammar imports C as a JavaScript module.
for role in c rust go python javascript bash json toml yaml; do
    (cd "$WORK/src/$role" && "$WORK/tree-sitter" generate --abi=15 \
        --js-runtime native)
done
mkdir -p "$WORK/src/typescript/node_modules"
ln -s "$WORK/src/typescript-base" \
    "$WORK/src/typescript/node_modules/tree-sitter-javascript"
(cd "$WORK/src/typescript" && \
    "$WORK/tree-sitter" generate --abi=15 --js-runtime node \
        --output typescript/src typescript/grammar.js && \
    "$WORK/tree-sitter" generate --abi=15 --js-runtime node \
        --output tsx/src tsx/grammar.js)

OUT="$WORK/vendor"
mkdir -p "$OUT/runtime" "$OUT/include/tree_sitter" "$OUT/grammars" \
    "$OUT/licenses" "$OUT/common"
cp -a "$WORK/src/runtime/lib/src/." "$OUT/runtime/"
cp -a "$WORK/src/runtime/lib/include/tree_sitter/api.h" \
    "$OUT/include/tree_sitter/api.h"
for header in parser.h alloc.h array.h; do
    cp -a "$WORK/src/rust/src/tree_sitter/$header" \
        "$OUT/include/tree_sitter/$header"
done
cp -a "$WORK/src/runtime/LICENSE" "$OUT/licenses/tree-sitter.txt"
cp -a "$WORK/src/typescript/common/scanner.h" "$OUT/common/scanner.h"

copy_language() {
    name=$1 role=$2 sub=$3
    src="$WORK/src/$role"
    [ "$sub" = . ] || src="$src/$sub"
    mkdir -p "$OUT/grammars/$name"
    cp -a "$src/src/parser.c" "$OUT/grammars/$name/parser.c"
    [ ! -f "$src/src/scanner.c" ] || \
        cp -a "$src/src/scanner.c" "$OUT/grammars/$name/scanner.c"
    cp -a "$WORK/src/$role/queries/highlights.scm" \
        "$OUT/grammars/$name/highlights.scm"
}

copy_language c c .
copy_language cpp cpp .
copy_language rust rust .
copy_language go go .
copy_language python python .
copy_language javascript javascript .
copy_language typescript typescript typescript
copy_language tsx typescript tsx
copy_language bash bash .
copy_language json json .
copy_language toml toml .
copy_language yaml yaml .
cp -a "$WORK/src/yaml/src/schema.core.c" "$OUT/grammars/yaml/schema.core.c"

for role in c cpp rust go python javascript typescript bash json toml yaml; do
    cp -a "$WORK/src/$role/LICENSE" "$OUT/licenses/$role.txt"
done

for parser in "$OUT"/grammars/*/parser.c; do
    grep -q '#define LANGUAGE_VERSION 15' "$parser" || {
        echo "$parser: parser is not ABI 15" >&2
        exit 1
    }
done

python3 "$ROOT/scripts/embed-highlights.py" "$OUT" "$WORK/queries.c"

{
    echo "Tree-sitter vendor lock"
    echo "======================="
    echo
    echo "Runtime and generator: v0.26.11; generated parser ABI: 15."
    echo "Generator sha256: $GENERATOR_SHA"
    echo
    echo "Source archives:"
    sed -n '/^[^#]/p' "$SOURCES"
    echo
    echo "Resolved embedded-query sha256:"
    sed -n 's/.*{ "\([^"]*\)".*"\([0-9a-f][0-9a-f]*\)" }.*/\1\t\2/p' \
        "$WORK/queries.c"
    echo
    echo "All runtime and grammar licenses are copied under licenses/."
} > "$OUT/LOCK"

[ ! -e "$OLD" ] || mv "$OLD" "$BACKUP"
mv "$OUT" "$OLD"
mv "$WORK/queries.c" "$ROOT/highlight/queries.c"
COMMITTED=true

echo "updated vendor/tree-sitter and highlight/queries.c"
