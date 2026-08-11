#!/usr/bin/env python3
"""Rename the internal symbol prefix and the user-facing product name.

The project keeps two names: an internal prefix used by C identifiers,
macros and the shared header, and a product name used by anything a user
sees (the binary, env vars, XDG directories, the project directory, help
text, docs). This script rewrites both, so a future rename is one command:

    python3 scripts/rename.py --product-to bar     # user-facing name only
    python3 scripts/rename.py --internal-to foo    # symbol prefix only

In C sources the split is contextual: code gets the internal name, while
string literals and comments get the product name unless the token is a
known identifier collected from the code itself. Every other text file is
product-only, because its `NAME_*` tokens are environment variables.

The script skips itself, so pass --internal-from/--product-from when the
current names are not the defaults below. A product rename also needs the
welcome art in `src/tui.c` redrawn and `python3 tests/run.py --update` run,
because the name changes column widths in the golden screens.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

SKIP_DIRS = {".git", "build", "bin", "__pycache__", ".pi", "node_modules"}
SKIP_PATHS = {"vendor/tree-sitter"}
SKIP_FILES = {"scripts/rename.py"}
TEXT_SUFFIXES = {
    ".c", ".h", ".py", ".md", ".toml", ".txt", ".tsv", ".sh", ".mk",
    ".json", ".yml", ".yaml", ".cfg", ".ini", "",
}
C_SUFFIXES = {".c", ".h"}


def variants(word):
    """(upper, title, lower) spellings of `word`."""
    return word.upper(), word[0].upper() + word[1:].lower(), word.lower()


class Names:
    def __init__(self, ifrom, ito, pfrom, pto, keep_internal=()):
        self.iu, self.it, self.il = variants(ifrom)
        self.iu2, self.it2, self.il2 = variants(ito)
        self.pu, self.pt, self.pl = variants(pfrom)
        self.pu2, self.pt2, self.pl2 = variants(pto)
        self.keep_internal = tuple(keep_internal)
        # Identifier shapes: PREFIX_NAME, prefix_name, PrefixName.
        self.ident_re = re.compile(
            r"\b(?:%s_[A-Za-z0-9_]*|%s_[A-Za-z0-9_]*|%s[A-Za-z0-9_]*)\b"
            % (self.iu, self.il, self.it))
        self.word_re = re.compile(r"%s|%s|%s" % (self.iu, self.it, self.il))
        self.header_re = re.compile(r"\b%s\.h\b" % self.il)

    def internal(self, text):
        return (text.replace(self.iu, self.iu2)
                    .replace(self.it, self.it2)
                    .replace(self.il, self.il2))

    def product(self, text):
        return (text.replace(self.pu, self.pu2)
                    .replace(self.pt, self.pt2)
                    .replace(self.pl, self.pl2))


def split_c(text):
    """Split C source into (kind, text) runs of code, string and comment."""
    out, i, n, start = [], 0, len(text), 0

    def flush(to, kind="code"):
        if to > start:
            out.append((kind, text[start:to]))

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] in "/*":
            flush(i)
            if text[i + 1] == "/":
                j = text.find("\n", i)
                j = n if j < 0 else j
            else:
                j = text.find("*/", i + 2)
                j = n if j < 0 else j + 2
            out.append(("comment", text[i:j]))
            i = start = j
            continue
        if c in "\"'":
            flush(i)
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(("str", text[i:j]))
            i = start = j
            continue
        i += 1
    flush(n)
    return out


def collect_idents(names, files):
    """Identifiers that appear in code, so text runs can spell them right."""
    found = set()
    for path in files:
        if path.suffix not in C_SUFFIXES:
            continue
        text = names.header_re.sub("", path.read_text(encoding="utf-8"))
        for kind, run in split_c(text):
            if kind == "code":
                found.update(names.ident_re.findall(run))
    return found


def rewrite_c(names, idents, text):
    def text_run(run):
        def one(m):
            tok = m.group(0)
            return names.internal(tok) if tok in idents else names.product(tok)
        run = names.ident_re.sub(one, run)
        return names.word_re.sub(lambda m: names.product(m.group(0)), run)

    parts = []
    for kind, run in split_c(text):
        parts.append(names.internal(run) if kind == "code" else text_run(run))
    return "".join(parts)


def rewrite_file(names, idents, path, dry):
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return False
    if "\0" in text:
        return False
    # The shared header keeps the internal name in every file type.
    new = names.header_re.sub("%s.h" % names.il2, text)
    if path.suffix in C_SUFFIXES:
        new = rewrite_c(names, idents, new)
    else:
        new = names.product(new)
        # Compile-time macros reach non-C files through -D flags.
        for tok in names.keep_internal:
            new = new.replace("%s_%s" % (names.pu2, tok),
                              "%s_%s" % (names.iu2, tok))
    if new == text:
        return False
    print("edit %s" % path)
    if not dry:
        path.write_text(new, encoding="utf-8")
    return True


def new_name(names, base):
    if base == "%s.h" % names.il:
        return "%s.h" % names.il2
    return names.product(base)


def rename_path(names, path, dry, tracked):
    base = path.name
    nb = new_name(names, base)
    if nb == base:
        return False
    dst = path.with_name(nb)
    print("move %s -> %s" % (path, dst))
    if dry:
        return True
    if str(path) in tracked:
        subprocess.run(["git", "mv", str(path), str(dst)], check=True)
    else:
        path.rename(dst)
    return True


def walk(root):
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        rel = "" if rel == "." else rel
        dirnames[:] = [d for d in sorted(dirnames)
                       if d not in SKIP_DIRS
                       and os.path.join(rel, d) not in SKIP_PATHS]
        for f in sorted(filenames):
            p = Path(dirpath) / f
            if (p.is_symlink() or p.suffix not in TEXT_SUFFIXES
                    or os.path.join(rel, f) in SKIP_FILES):
                continue
            yield p


def git_tracked(root):
    try:
        out = subprocess.run(["git", "ls-files"], cwd=root, check=True,
                             capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return set()
    return set(out.split("\n"))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--internal-from", default="agent")
    ap.add_argument("--internal-to", help="default: unchanged")
    ap.add_argument("--product-from", default="arqan")
    ap.add_argument("--product-to", help="default: unchanged")
    ap.add_argument("--internal-tokens", default="TESTING",
                    help="macro suffixes that stay internal in every file")
    ap.add_argument("--root", default=".")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    os.chdir(root)
    names = Names(args.internal_from, args.internal_to or args.internal_from,
                  args.product_from, args.product_to or args.product_from,
                  [t for t in args.internal_tokens.split(",") if t])
    files = list(walk(Path(".")))
    idents = collect_idents(names, files)
    print("known identifiers: %d" % len(idents), file=sys.stderr)

    edits = sum(rewrite_file(names, idents, p, args.dry_run) for p in files)
    tracked = git_tracked(root)
    moves = sum(rename_path(names, p, args.dry_run, tracked) for p in files)
    print("%d file(s) edited, %d renamed" % (edits, moves), file=sys.stderr)


if __name__ == "__main__":
    main()
