#!/usr/bin/env python3
"""Regression tests for Linux release archives and native packages."""

from __future__ import annotations

import hashlib
import gzip
import io
import itertools
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
    "bash.txt", "c.txt", "cpp.txt", "go.txt", "javascript.txt", "json.txt",
    "python.txt", "rust.txt", "toml.txt", "tree-sitter.txt",
    "typescript.txt", "yaml.txt",
}


def fail(message: str) -> None:
    raise AssertionError(message)


def run(*args, cwd=ROOT, env=None, check=True, text=True, input_data=None):
    result = subprocess.run(
        [str(arg) for arg in args], cwd=cwd, env=env, text=text,
        input=input_data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if check and result.returncode:
        stdout = result.stdout if text else result.stdout.decode(errors="replace")
        stderr = result.stderr if text else result.stderr.decode(errors="replace")
        fail(
            f"command failed ({result.returncode}): {' '.join(map(str, args))}\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
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


def payload_files() -> set[str]:
    files = {
        "README.md", "CHANGELOG.md", "LICENSE", "THIRD_PARTY_NOTICES.md",
        "vendor/lexbor/LICENSE", "vendor/lexbor/NOTICE",
        "vendor/tree-sitter/runtime/unicode/LICENSE",
    }
    files.update(f"vendor/tree-sitter/licenses/{name}" for name in LICENSES)
    return files


def paths_with_dirs(files: set[str], prefix: str = "") -> set[str]:
    paths = set()
    if prefix:
        paths.add(prefix)
    for name in files:
        path = PurePosixPath(prefix, name) if prefix else PurePosixPath(name)
        paths.add(str(path))
        parent = path.parent
        while str(parent) != ".":
            paths.add(str(parent))
            parent = parent.parent
    return paths


def glibc_versions(binary: Path) -> list[tuple[int, ...]]:
    text = run("readelf", "--version-info", "-W", binary).stdout
    found = re.findall(r"GLIBC_(\d+(?:\.\d+)*)", text)
    return [tuple(map(int, item.split("."))) for item in found]


def needed(binary: Path) -> set[str]:
    text = run("readelf", "-dW", binary).stdout
    return set(re.findall(r"\(NEEDED\).*\[([^]]+)\]", text))


def curl_versions(binary: Path) -> list[str]:
    text = run("readelf", "--version-info", "-W", binary).stdout
    return re.findall(r"CURL_[A-Za-z0-9_]+", text)


def check_elf(
    binary: Path, expected: set[str], max_glibc: tuple[int, ...] = (2, 31),
    resolvable: bool = True,
) -> None:
    check_arch(binary)
    actual = needed(binary)
    if actual != expected:
        fail(f"unexpected dependencies for {binary}: {actual}")
    versions = glibc_versions(binary)
    if not versions or max(versions) > max_glibc:
        fail(f"GLIBC ceiling exceeded by {binary}: {max(versions, default=())}")
    if not resolvable:
        return
    ldd = run("ldd", binary)
    if "not found" in ldd.stdout + ldd.stderr:
        fail(f"missing shared library for {binary}")


def check_arch(binary: Path) -> None:
    file_out = run("file", "-L", binary).stdout
    if not re.search(r"ELF 64-bit LSB.*x86-64", file_out):
        fail(f"wrong ELF architecture: {file_out.strip()}")
    header = run("readelf", "-hW", binary).stdout
    if not re.search(r"Machine:\s+Advanced Micro Devices X86-64", header):
        fail(f"wrong ELF machine: {binary}")
    dynamic = run("readelf", "-dW", binary).stdout
    if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
        fail(f"RPATH/RUNPATH found in {binary}")


def check_static_elf(binary: Path) -> None:
    """The archive runs on any x86_64 Linux, so it may borrow nothing.

    readelf answers, not file: file 5.39 calls a static-pie binary
    "dynamically linked" because it is an ET_DYN object.
    """
    check_arch(binary)
    header = run("readelf", "-hW", binary).stdout
    if not re.search(r"^\s*Type:\s+DYN", header, re.MULTILINE):
        fail(f"archive binary is not position independent: {binary}")
    if needed(binary):
        fail(f"archive binary needs shared libraries: {needed(binary)}")
    if re.search(r"INTERP", run("readelf", "-lW", binary).stdout):
        fail(f"archive binary names an interpreter: {binary}")
    if glibc_versions(binary):
        fail(f"archive binary carries glibc versioned symbols: {binary}")


def check_member(
    member: tarfile.TarInfo, epoch: int, executable: bool,
    named_root: bool = False,
) -> None:
    path = PurePosixPath(member.name)
    if path.is_absolute() or ".." in path.parts:
        fail(f"unsafe package path: {member.name}")
    expected_names = ("root", "root") if named_root else ("", "")
    if (member.uid, member.gid, member.uname, member.gname) != (
        0, 0, *expected_names,
    ):
        fail(f"non-root/non-normalized ownership: {member.name}")
    if int(member.mtime) != epoch:
        fail(f"non-normalized timestamp: {member.name}")
    expected_mode = 0o755 if member.isdir() or executable else 0o644
    if member.mode != expected_mode:
        fail(f"wrong mode {member.mode:o}: {member.name}")
    if not (member.isdir() or member.isfile()):
        fail(f"unexpected entry type: {member.name}")


def check_executables(root: Path) -> None:
    # libcurl is opened at the first request, so it is named by the package's
    # dependencies and not by the binary. Debian 11 predates glibc 2.34 and
    # still keeps dlopen in libdl.
    checks = {
        "arqan": {"libc.so.6", "libdl.so.2"},
        "arqan-highlight": {"libc.so.6"},
    }
    for name, dependencies in checks.items():
        binary = root / "usr/bin" / name
        run(binary, "--version")
        check_elf(binary, dependencies)


def check_el9_executables(root: Path) -> None:
    """The rpm and the pacman package share the EL9 binaries, so this host may
    not be able to run them; the release script's Fedora and Arch smoke tests
    do that. What matters here is that they ask a non-Debian host for nothing
    it lacks: glibc no newer than EL9's 2.34, and no versioned libcurl symbol,
    which only Debian's libcurl defines and whose absence makes the loader
    warn on every startup."""
    # EL9 is glibc 2.34, which carries dlopen in libc itself, and libcurl is
    # opened at the first request rather than linked.
    checks = {
        "arqan": {"libc.so.6"},
        "arqan-highlight": {"libc.so.6"},
    }
    for name, dependencies in checks.items():
        binary = root / "usr/bin" / name
        check_elf(binary, dependencies, max_glibc=(2, 34), resolvable=False)
        if curl_versions(binary):
            fail(f"versioned libcurl symbols required by {binary}: "
                 f"{sorted(set(curl_versions(binary)))}")


def check_tarball(archive: Path, top: str, epoch: int, temp: Path) -> None:
    expected_files = {"bin/arqan", "bin/arqan-highlight"} | payload_files()
    expected = paths_with_dirs(expected_files, top)
    with tarfile.open(archive, "r:gz") as tf:
        members = tf.getmembers()
        names = {member.name.rstrip("/") for member in members}
        if names != expected:
            fail(f"tar payload mismatch\nmissing: {sorted(expected - names)}\n"
                 f"extra: {sorted(names - expected)}")
        if any(name.endswith("install.sh") for name in names):
            fail("portable archive still contains install.sh")
        for member in members:
            path = PurePosixPath(member.name)
            if not path.parts or path.parts[0] != top:
                fail(f"path outside versioned archive directory: {member.name}")
            executable = member.name in {
                f"{top}/bin/arqan", f"{top}/bin/arqan-highlight",
            }
            check_member(member, epoch, executable)
        # Paths and entry types were validated before extraction.
        tf.extractall(temp)
    root = temp / top
    check_static_elf(root / "bin/arqan")
    check_static_elf(root / "bin/arqan-highlight")
    run(root / "bin/arqan", "--version")
    run(root / "bin/arqan-highlight", "--version")


def deb_expected() -> set[str]:
    docs = payload_files() - {"LICENSE"}
    docs.add("copyright")
    files = {"usr/bin/arqan", "usr/bin/arqan-highlight"}
    files.update(f"usr/share/doc/arqan/{name}" for name in docs)
    return paths_with_dirs(files) | {"."}


def check_deb(package: Path, ver: str, epoch: int, temp: Path) -> None:
    fields = run("dpkg-deb", "-f", package).stdout
    expected_fields = {
        "Package": "arqan", "Version": f"{ver}-1", "Architecture": "amd64",
        "Maintainer": "Alikhan Bissakov <bissakov@users.noreply.github.com>",
        "Homepage": "https://github.com/bissakov/arqan",
    }
    for field, value in expected_fields.items():
        if not re.search(rf"^{field}: {re.escape(value)}$", fields, re.MULTILINE):
            fail(f"wrong Debian {field}")
    depends = run("dpkg-deb", "-f", package, "Depends").stdout.strip()
    for dependency in ("libc6 (>= 2.31)", "ca-certificates", "libcurl4 | libcurl4t64"):
        if dependency not in depends:
            fail(f"missing Debian dependency: {dependency}")

    control_dir = temp / "deb-control"
    run("dpkg-deb", "-e", package, control_dir)
    control_files = {path.relative_to(control_dir).as_posix()
                     for path in control_dir.rglob("*") if path.is_file()}
    if control_files != {"control"}:
        fail(f"Debian package has maintainer scripts: {sorted(control_files)}")

    data = run("dpkg-deb", "--fsys-tarfile", package, text=False).stdout
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as tf:
        members = tf.getmembers()
        names = {member.name.rstrip("/").removeprefix("./") for member in members}
        expected = deb_expected()
        if names != expected:
            fail(f"deb payload mismatch\nmissing: {sorted(expected - names)}\n"
                 f"extra: {sorted(names - expected)}")
        for member in members:
            executable = member.name in {"./usr/bin/arqan", "./usr/bin/arqan-highlight"}
            check_member(member, epoch, executable, named_root=True)
        root = temp / "deb-root"
        root.mkdir()
        # Paths and entry types were validated before extraction.
        tf.extractall(root)
    check_executables(root)


def rpm_file_records(package: Path) -> dict[str, tuple[str, str, str, int]]:
    query = "[%{FILENAMES}\\t%{FILEMODES:perms}\\t%{FILEUSERNAME}\\t%{FILEGROUPNAME}\\t%{FILEMTIMES}\\n]"
    lines = run("rpm", "-qp", "--qf", query, package).stdout.splitlines()
    records = {}
    for line in lines:
        name, mode, owner, group, mtime = line.split("\t")
        records[name] = (mode, owner, group, int(mtime))
    return records


def rpm_expected_files() -> set[str]:
    docs = {"README.md", "CHANGELOG.md", "THIRD_PARTY_NOTICES.md",
            "vendor/lexbor/NOTICE"}
    licenses = payload_files() - docs
    files = {"/usr/bin/arqan", "/usr/bin/arqan-highlight"}
    files.update(f"/usr/share/doc/arqan/{name}" for name in docs)
    files.update(f"/usr/share/licenses/arqan/{name}" for name in licenses)
    return files


def check_rpm(package: Path, ver: str, epoch: int, temp: Path) -> None:
    query = "%{NAME}\n%{VERSION}\n%{RELEASE}\n%{ARCH}\n%{PACKAGER}\n%{URL}\n%{LICENSE}\n%{BUILDHOST}\n%{BUILDTIME}\n"
    values = run("rpm", "-qp", "--qf", query, package).stdout.splitlines()
    expected = [
        "arqan", ver, "1", "x86_64",
        "Alikhan Bissakov <bissakov@users.noreply.github.com>",
        "https://github.com/bissakov/arqan",
        "MPL-2.0 AND Apache-2.0 AND MIT AND ICU AND BSD-3-Clause",
        "reproducible.invalid", str(epoch),
    ]
    if values != expected:
        fail(f"wrong RPM identity metadata: {values}")
    requires = set(run("rpm", "-qp", "--requires", package).stdout.splitlines())
    if "ca-certificates" not in requires:
        fail("RPM lacks explicit CA certificate dependency")
    # libcurl is opened at the first request, so no NEEDED entry names it and
    # rpm generates nothing for it. The spec carries the package by hand, and
    # this is the check that it still does.
    if "libcurl" not in requires:
        fail("RPM lacks explicit libcurl dependency")
    for pattern in (r"^libc\.so\.6",):
        if not any(re.search(pattern, item) for item in requires):
            fail(f"RPM lacks generated dependency {pattern}")
    # Nothing filters the generated dependencies any more, so a versioned
    # libcurl requirement would reach the host resolver and fail the install.
    if any(re.search(r"^libcurl\.so\.4\(CURL_", item) for item in requires):
        fail(f"RPM requires versioned libcurl symbols: {sorted(requires)}")
    scripts = run("rpm", "-qp", "--scripts", package).stdout.strip()
    if scripts:
        fail(f"RPM contains scriptlets: {scripts}")

    records = rpm_file_records(package)
    expected_files = rpm_expected_files()
    if set(records) != expected_files:
        fail(f"rpm payload mismatch\nmissing: {sorted(expected_files - set(records))}\n"
             f"extra: {sorted(set(records) - expected_files)}")
    for name, (mode, owner, group, mtime) in records.items():
        expected_mode = "-rwxr-xr-x" if name.startswith("/usr/bin/") else "-rw-r--r--"
        if (mode, owner, group, mtime) != (expected_mode, "root", "root", epoch):
            fail(f"wrong RPM file metadata for {name}: {records[name]}")

    root = temp / "rpm-root"
    root.mkdir()
    cpio_data = run("rpm2cpio", package, text=False).stdout
    run("cpio", "-idm", "--quiet", cwd=root, text=False, input_data=cpio_data)
    extracted = {"/" + path.relative_to(root).as_posix()
                 for path in root.rglob("*") if path.is_file()}
    if extracted != expected_files:
        fail("RPM extraction disagrees with its file metadata")
    check_el9_executables(root)


def pkg_expected_files() -> set[str]:
    return {name.removeprefix("/") for name in rpm_expected_files()}


def parse_pkginfo(text: str) -> dict[str, list[str]]:
    fields: dict[str, list[str]] = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition(" = ")
        if not separator:
            fail(f"malformed .PKGINFO line: {line}")
        fields.setdefault(key, []).append(value)
    return fields


def parse_mtree(data: bytes) -> dict[str, dict[str, str]]:
    """Rebuild the records pacman checks installed files against.

    Only the subset the packaging script emits is understood: a gzip-compressed
    mtree whose defaults come from /set and whose paths need no unescaping.
    """
    defaults: dict[str, str] = {}
    records: dict[str, dict[str, str]] = {}
    for line in gzip.decompress(data).decode().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        keywords = dict(item.split("=", 1) for item in fields[1:] if "=" in item)
        if fields[0] == "/set":
            defaults.update(keywords)
        elif fields[0] == "/unset":
            for keyword in fields[1:]:
                defaults.pop(keyword, None)
        else:
            records[fields[0].removeprefix("./")] = {**defaults, **keywords}
    return records


def check_pkg(package: Path, ver: str, epoch: int, temp: Path) -> None:
    """The pacman package is assembled without Arch tooling, so this checks
    what makepkg would otherwise guarantee: the metadata dot-files pacman
    stops reading at, root ownership, and an .MTREE that describes exactly the
    files the package installs."""
    data = run("zstd", "-dc", package, text=False).stdout
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as tf:
        members = tf.getmembers()
        names = [member.name.rstrip("/") for member in members]
        metadata = {".PKGINFO", ".MTREE"}
        expected = paths_with_dirs(pkg_expected_files())
        payload = set(names) - metadata
        if payload != expected:
            fail(f"pacman payload mismatch\nmissing: {sorted(expected - payload)}\n"
                 f"extra: {sorted(payload - expected)}")
        leading = set(itertools.takewhile(lambda name: name.startswith("."), names))
        if leading != metadata:
            fail(f"pacman metadata must precede the payload: {sorted(leading)}")
        for member in members:
            executable = member.name in {"usr/bin/arqan", "usr/bin/arqan-highlight"}
            check_member(member, epoch, executable)
        pkginfo = tf.extractfile(".PKGINFO").read().decode()
        mtree = tf.extractfile(".MTREE").read()
        root = temp / "pkg-root"
        root.mkdir()
        # Paths and entry types were validated before extraction.
        tf.extractall(root)

    fields = parse_pkginfo(pkginfo)
    expected_fields = {
        "pkgname": ["arqan"],
        "pkgbase": ["arqan"],
        "pkgver": [f"{ver}-1"],
        "arch": ["x86_64"],
        "url": ["https://github.com/bissakov/arqan"],
        "packager": ["Alikhan Bissakov <bissakov@users.noreply.github.com>"],
        "builddate": [str(epoch)],
        "license": ["MPL-2.0", "Apache-2.0", "MIT", "ICU", "BSD-3-Clause"],
        "depend": ["glibc", "curl", "ca-certificates"],
    }
    for key, value in expected_fields.items():
        if fields.get(key) != value:
            fail(f"wrong .PKGINFO {key}: {fields.get(key)}")
    size = fields.get("size", ["0"])[0]
    if not size.isdigit() or int(size) <= 0:
        fail(f"pacman package reports no installed size: {size}")

    records = parse_mtree(mtree)
    expected_records = expected | {".PKGINFO"}
    if set(records) != expected_records:
        fail(f"mtree mismatch\nmissing: {sorted(expected_records - set(records))}\n"
             f"extra: {sorted(set(records) - expected_records)}")
    for name, record in records.items():
        path = root / name
        if (record.get("uid"), record.get("gid")) != ("0", "0"):
            fail(f"non-root mtree ownership: {name}")
        if record.get("time") != f"{epoch}.0":
            fail(f"non-normalized mtree timestamp: {name}")
        if record.get("type") == "dir":
            if not path.is_dir():
                fail(f"mtree names a directory the package lacks: {name}")
            if record.get("mode") != "755":
                fail(f"wrong mtree mode for {name}: {record.get('mode')}")
            continue
        expected_mode = "755" if name.startswith("usr/bin/") else "644"
        if record.get("mode") != expected_mode:
            fail(f"wrong mtree mode for {name}: {record.get('mode')}")
        content = path.read_bytes()
        if record.get("size") != str(len(content)):
            fail(f"mtree size disagrees with the payload: {name}")
        if record.get("sha256digest") != hashlib.sha256(content).hexdigest():
            fail(f"mtree digest disagrees with the payload: {name}")
    check_el9_executables(root)


def main() -> int:
    if platform.system() != "Linux" or platform.machine() not in {"x86_64", "amd64"}:
        print("package-linux tests require Linux x86_64", file=sys.stderr)
        return 2
    requirements = ("cpio", "dpkg-deb", "file", "readelf", "rpm", "rpm2cpio",
                    "sha256sum", "tar", "zstd")
    for command in requirements:
        if shutil.which(command) is None:
            print(f"missing test requirement: {command}", file=sys.stderr)
            return 2

    ver = version()
    workflow = ROOT / ".github/workflows/release-linux.yml"
    workflow_text = workflow.read_text() if workflow.is_file() else ""
    if "GITHUB_REF_NAME" not in workflow_text:
        fail("release workflow does not validate its tag")

    names = sorted([
        f"arqan-{ver}-linux-x86_64.tar.gz",
        f"arqan_{ver}-1_amd64.deb",
        f"arqan-{ver}-1.x86_64.rpm",
        f"arqan-{ver}-1-x86_64.pkg.tar.zst",
    ])
    dist = ROOT / "dist"
    assets = [dist / name for name in names]
    manifest = dist / "SHA256SUMS"
    actual_files = {path.name for path in dist.iterdir() if path.is_file()}
    if actual_files != set(names) | {"SHA256SUMS"}:
        fail(f"unexpected release asset set: {sorted(actual_files)}")
    lines = manifest.read_text().splitlines()
    expected_pattern = [rf"[0-9a-f]{{64}}  {re.escape(name)}" for name in names]
    if len(lines) != len(names) or any(
            not re.fullmatch(pattern, line)
            for pattern, line in zip(expected_pattern, lines)):
        fail("SHA256SUMS must contain exactly the four sorted versioned artifacts")
    run("sha256sum", "--ignore-missing", "-c", manifest.name, cwd=dist)

    epoch_text = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch_text is None:
        epoch_text = run("git", "show", "-s", "--format=%ct", "HEAD").stdout.strip()
    epoch = int(epoch_text)
    with tempfile.TemporaryDirectory(prefix="arqan package test ") as tmp_name:
        temp = Path(tmp_name)
        check_tarball(
            dist / f"arqan-{ver}-linux-x86_64.tar.gz",
            f"arqan-{ver}-linux-x86_64", epoch, temp / "tar",
        )
        check_deb(dist / f"arqan_{ver}-1_amd64.deb", ver, epoch, temp)
        check_rpm(dist / f"arqan-{ver}-1.x86_64.rpm", ver, epoch, temp)
        check_pkg(dist / f"arqan-{ver}-1-x86_64.pkg.tar.zst", ver, epoch, temp)

    first = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
             for path in assets + [manifest]}
    env = os.environ.copy()
    env["SOURCE_DATE_EPOCH"] = str(epoch)
    run(ROOT / "scripts/package-linux.sh", env=env)
    second = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
              for path in assets + [manifest]}
    if first != second:
        changed = sorted(name for name in first if first[name] != second[name])
        fail(f"packaging identical inputs produced different bytes: {changed}")

    print("package-linux: verified tar, deb, rpm, pkg.tar.zst, and SHA256SUMS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"package-linux: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
