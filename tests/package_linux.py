#!/usr/bin/env python3
"""Regression tests for Linux release archives and installation."""

from __future__ import annotations

import hashlib
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "src/agent.h"
LICENSES = {
    "bash.txt",
    "c.txt",
    "cpp.txt",
    "go.txt",
    "javascript.txt",
    "json.txt",
    "python.txt",
    "rust.txt",
    "toml.txt",
    "tree-sitter.txt",
    "typescript.txt",
    "yaml.txt",
}


def fail(message: str) -> None:
    raise AssertionError(message)


def run(*args, cwd=ROOT, env=None, check=True):
    result = subprocess.run(
        [str(arg) for arg in args], cwd=cwd, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if check and result.returncode:
        fail(
            f"command failed ({result.returncode}): {' '.join(map(str, args))}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def extract_version(text: str) -> str:
    matches = re.findall(
        r'^\s*#\s*define\s+AGENT_VERSION\s+"([^"]+)"', text, re.MULTILINE,
    )
    if len(matches) != 1 or not re.fullmatch(r"\d+\.\d+\.\d+", matches[0]):
        fail("source must contain exactly one semantic AGENT_VERSION")
    return matches[0]


def version() -> str:
    return extract_version(HEADER.read_text())


def expected_payload(top: str) -> set[str]:
    files = {
        "bin/arqan",
        "bin/arqan-highlight",
        "install.sh",
        "README.md",
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
        "vendor/lexbor/LICENSE",
        "vendor/lexbor/NOTICE",
        "vendor/tree-sitter/runtime/unicode/LICENSE",
    }
    files.update(f"vendor/tree-sitter/licenses/{name}" for name in LICENSES)
    dirs = {""}
    for name in files:
        parent = PurePosixPath(name).parent
        while str(parent) != ".":
            dirs.add(str(parent))
            parent = parent.parent
    return {top if not name else f"{top}/{name}" for name in files | dirs}


def glibc_versions(binary: Path) -> list[tuple[int, ...]]:
    text = run("readelf", "--version-info", "-W", binary).stdout
    found = re.findall(r"GLIBC_(\d+(?:\.\d+)*)", text)
    return [tuple(map(int, item.split("."))) for item in found]


def needed(binary: Path) -> set[str]:
    text = run("readelf", "-dW", binary).stdout
    return set(re.findall(r"\(NEEDED\).*\[([^]]+)\]", text))


def check_elf(binary: Path, expected: set[str]) -> None:
    file_out = run("file", "-L", binary).stdout
    if not re.search(r"ELF 64-bit LSB.*x86-64", file_out):
        fail(f"wrong ELF architecture: {file_out.strip()}")
    header = run("readelf", "-hW", binary).stdout
    if not re.search(r"Machine:\s+Advanced Micro Devices X86-64", header):
        fail(f"wrong ELF machine: {binary}")
    dynamic = run("readelf", "-dW", binary).stdout
    if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
        fail(f"RPATH/RUNPATH found in {binary}")
    if needed(binary) != expected:
        fail(f"unexpected dependencies for {binary}: {needed(binary)}")
    versions = glibc_versions(binary)
    if not versions or max(versions) > (2, 31):
        fail(f"GLIBC ceiling exceeded by {binary}: {max(versions, default=())}")
    ldd = run("ldd", binary)
    if "not found" in ldd.stdout + ldd.stderr:
        fail(f"missing shared library for {binary}")


def install(archive_root: Path, *args: str, env: dict[str, str]) -> None:
    run(archive_root / "install.sh", *args, cwd=archive_root, env=env)


def check_installation(archive_root: Path, temp: Path) -> None:
    home = temp / "home with spaces"
    home.mkdir()
    env = os.environ.copy()
    env.update({
        "HOME": str(home),
        "PATH": "/usr/bin:/bin",
        "XDG_CONFIG_HOME": str(home / "cfg"),
        "XDG_STATE_HOME": str(home / "state"),
        "XDG_DATA_HOME": str(home / "data"),
    })
    install(archive_root, env=env)
    prefix = home / ".local"
    docs = prefix / "share/doc/arqan"
    for binary in (prefix / "bin/arqan", prefix / "bin/arqan-highlight"):
        if stat.S_IMODE(binary.stat().st_mode) != 0o755:
            fail(f"wrong installed executable mode: {binary}")
        run(binary, "--version", env=env)
    for name in ("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md"):
        path = docs / name
        if stat.S_IMODE(path.stat().st_mode) != 0o644:
            fail(f"wrong installed documentation mode: {path}")

    no_home = env.copy()
    no_home.pop("HOME")
    missing_home = run(
        archive_root / "install.sh", cwd=archive_root, env=no_home, check=False,
    )
    if missing_home.returncode == 0 or "HOME is not set" not in missing_home.stderr:
        fail("default installation must refuse an unset HOME")

    unrelated = prefix / "bin/keep-me"
    unrelated.write_text("sentinel")
    old_program = prefix / "bin/arqan"
    old_program.write_text("incomplete upgrade")
    install(archive_root, env=env)
    run(old_program, "--version", env=env)
    if unrelated.read_text() != "sentinel":
        fail("reinstall damaged an unrelated file")

    explicit = temp / "explicit prefix"
    install(archive_root, "--prefix", str(explicit), env=env)
    if not (explicit / "bin/arqan-highlight").is_file():
        fail("--prefix did not install both binaries")

    staging = temp / "staging root"
    staged_prefix = "/opt/arqan test"
    install(
        archive_root, "--prefix", staged_prefix, "--destdir", str(staging),
        env=env,
    )
    if not (staging / staged_prefix.lstrip("/") / "bin/arqan").is_file():
        fail("--destdir did not preserve the prefix-relative tree")

    project = temp / "project/.arqan"
    project.mkdir(parents=True)
    sentinels = [
        unrelated,
        Path(env["XDG_CONFIG_HOME"]) / "arqan/config.toml",
        Path(env["XDG_STATE_HOME"]) / "arqan/state.toml",
        Path(env["XDG_STATE_HOME"]) / "arqan/credentials.toml",
        Path(env["XDG_DATA_HOME"]) / "arqan/sessions/keep.json",
        project / "config.toml",
        docs / "unrelated.txt",
    ]
    for sentinel in sentinels[1:]:
        sentinel.parent.mkdir(parents=True, exist_ok=True)
        sentinel.write_text("sentinel")
    install(archive_root, "--uninstall", env=env)
    if (prefix / "bin/arqan").exists() or (prefix / "bin/arqan-highlight").exists():
        fail("uninstall left an installed executable")
    for sentinel in sentinels:
        if sentinel.read_text() != "sentinel":
            fail(f"uninstall damaged sentinel {sentinel}")


def main() -> int:
    if platform.system() != "Linux" or platform.machine() not in {"x86_64", "amd64"}:
        print("package-linux tests require Linux x86_64", file=sys.stderr)
        return 2
    for command in ("file", "readelf", "sha256sum", "tar"):
        if shutil.which(command) is None:
            print(f"missing test requirement: {command}", file=sys.stderr)
            return 2

    ver = version()
    workflow = ROOT / ".github/workflows/release-linux.yml"
    if not workflow.is_file() or "GITHUB_REF_NAME" not in workflow.read_text():
        fail("release workflow does not validate its tag")
    if extract_version(HEADER.read_text()) != ver:
        fail("tag validation version parser disagrees with packaging")
    archive = ROOT / "dist" / f"arqan-{ver}-linux-x86_64.tar.gz"
    checksum = archive.with_name(archive.name + ".sha256")
    if not archive.is_file() or not checksum.is_file():
        fail(f"missing {archive.name} and checksum; run make package-linux")
    checksum_line = checksum.read_text().strip()
    if not re.fullmatch(rf"[0-9a-f]{{64}}  {re.escape(archive.name)}", checksum_line):
        fail("checksum filename or format does not agree with AGENT_VERSION")
    run("sha256sum", "-c", checksum.name, cwd=checksum.parent)

    epoch_text = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch_text is None:
        epoch_text = run("git", "show", "-s", "--format=%ct", "HEAD").stdout.strip()
    epoch = int(epoch_text)
    top = f"arqan-{ver}-linux-x86_64"
    with tarfile.open(archive, "r:gz") as tf:
        members = tf.getmembers()
        names = {member.name.rstrip("/") for member in members}
        if names != expected_payload(top):
            fail(
                "archive payload mismatch\nmissing: "
                f"{sorted(expected_payload(top) - names)}\nextra: {sorted(names - expected_payload(top))}"
            )
        for member in members:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or path.parts[0] != top:
                fail(f"unsafe archive path: {member.name}")
            if member.uid != 0 or member.gid != 0 or member.uname or member.gname:
                fail(f"non-normalized archive ownership: {member.name}")
            if int(member.mtime) != epoch:
                fail(f"non-normalized timestamp: {member.name}")
            expected_mode = 0o755 if member.isdir() or member.name in {
                f"{top}/bin/arqan", f"{top}/bin/arqan-highlight",
                f"{top}/install.sh",
            } else 0o644
            if member.mode != expected_mode:
                fail(f"wrong mode {member.mode:o}: {member.name}")
            if not (member.isdir() or member.isfile()):
                fail(f"unexpected archive entry type: {member.name}")

    first_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
    env = os.environ.copy()
    env["SOURCE_DATE_EPOCH"] = str(epoch)
    run(ROOT / "scripts/package-linux.sh", env=env)
    second_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
    if first_hash != second_hash:
        fail("packaging identical inputs twice produced different bytes")

    with tempfile.TemporaryDirectory(prefix="arqan package test ") as tmp_name:
        temp = Path(tmp_name)
        with tarfile.open(archive, "r:gz") as tf:
            # Paths and entry types were checked above before extraction.
            tf.extractall(temp)
        archive_root = temp / top
        arqan = archive_root / "bin/arqan"
        helper = archive_root / "bin/arqan-highlight"
        run(arqan, "--version")
        run(helper, "--version")
        check_elf(arqan, {"libc.so.6", "libcurl.so.4"})
        check_elf(helper, {"libc.so.6"})
        check_installation(archive_root, temp)

    print(f"package-linux: verified {archive.name}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"package-linux: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
