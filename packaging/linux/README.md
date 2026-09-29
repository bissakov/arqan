# Linux packages

This covers building, testing and publishing the Linux release packages. For
installing them, see the top-level `README.md`.

## Build

`scripts/build-musl.sh` builds and tests the static pair in the pinned Alpine
container and leaves it in `bin/musl`. `make package-linux` then produces the
tarball, `.deb`, `.rpm`, `.pkg.tar.zst` and the `SHA256SUMS` manifest.

Each format takes its binaries from a different build:

- `.deb`: the host binaries in `bin`.
- `.rpm` and the Arch package: the EL9 binaries.
- The portable archive: the static musl binaries. Packaging fails rather than
  ship an archive that links a shared library.

## Test

`make test-package-linux` checks all four formats and that they build
reproducibly.

`make release-linux` (or `scripts/release-linux.sh`) runs the full release
check:

1. a clean build and all tests;
2. the builds and packaging in the pinned Debian 11 and Alpine containers;
3. install, reinstall and removal in disposable Debian, Ubuntu,
   Fedora-family and Arch containers;
4. `pacman -Qkk` on the Arch package against its own file manifest;
5. the portable archive run in Alpine and in Debian 11.

## Publish

`make publish-repos` (or `scripts/publish-repos.sh`) rebuilds the signed apt,
dnf and pacman repositories from the packages attached to the published
releases. `.github/workflows/publish-repos.yml` runs it on every published
release and deploys the result to GitHub Pages. See
[REPOSITORIES.md](REPOSITORIES.md) for the layout, what each format signs, and
how the signing key is held.
