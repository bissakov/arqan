# Package repositories

`scripts/publish-repos.sh` builds signed apt, dnf and pacman repositories from
the packages attached to the published GitHub releases, and
`.github/workflows/publish-repos.yml` deploys the result to GitHub Pages. The
site is the only state: every run rebuilds it from the releases, so nothing
has to be committed and a bad run is fixed by rerunning.

## Layout

```
/arqan-archive-keyring.asc          public half of the signing key
/index.html                         install instructions
/deb/arqan.sources                  deb822 client configuration
/deb/dists/stable/{InRelease,Release,Release.gpg}
/deb/dists/stable/main/binary-amd64/Packages{,.gz,.xz}
/deb/pool/main/a/arqan/*.deb
/rpm/arqan.repo                     dnf client configuration
/rpm/x86_64/{*.rpm,repodata/{repomd.xml,repomd.xml.asc,...}}
/arch/x86_64/{*.pkg.tar.zst,*.sig,arqan.db,arqan.db.sig,arqan.files,...}
```

Only x86_64 is published, because that is what `scripts/package-linux.sh`
builds.

## What is signed

- apt: the `Release` file, as `InRelease` and `Release.gpg`. The `.deb` files
  themselves are unsigned, as in Debian; their hashes are in the signed index.
- dnf: `repodata/repomd.xml.asc` covers the index, and each `.rpm` carries a
  signature header added by `rpmsign` at publish time. That header is the only
  difference between the repository copy and the release asset, which is
  verified against `SHA256SUMS` before it is signed.
- pacman: a detached `.sig` per package, embedded in the database by
  `repo-add`, and a detached signature over the database.

`repo-metadata.sh` runs inside the image from `Dockerfile.repo` for the apt and
rpm work and every gpg call, and inside the pinned Arch image for `repo-add`,
which is the only tool that has to come from Arch.

## The signing key

One passphrase-less key, kept in the `REPO_SIGNING_KEY` repository secret:

```sh
gpg --quick-generate-key 'arqan repository signing <you@example.com>' \
    rsa4096 sign never
gpg --armor --export-secret-keys <fingerprint>   # paste into the secret
```

Rotating it means every installed client has to import the new key, so treat it
as long-lived and keep an offline copy. The public half is exported by the
script, so it never has to be committed.

## One-time repository setup

1. Settings, Pages, source "GitHub Actions".
2. Add the `REPO_SIGNING_KEY` secret.
3. Publish a release, or run the workflow by hand.

The workflow runs on `release: published`, not on a tag, so it fires when the
draft release is published manually.

## Local dry run

Needs Docker and a throwaway key. `ARQAN_REPO_ASSETS` takes a directory of
`<tag>/` subdirectories holding the release assets, which skips the GitHub CLI:

```sh
mkdir -p /tmp/assets/v0.6.0 && cp dist/* /tmp/assets/v0.6.0/
ARQAN_REPO_ASSETS=/tmp/assets \
ARQAN_REPO_SIGNING_KEY=/tmp/key.asc \
ARQAN_REPO_OUT=/tmp/pages \
    scripts/publish-repos.sh
```

Serve `/tmp/pages` over HTTP and install from it in a `debian`, `fedora` or
`archlinux` container to check a change end to end.

Other knobs: `ARQAN_REPO_URL` (the site base URL that reaches the client
configuration), `ARQAN_REPO_SLUG`, `ARQAN_REPO_KEEP` (how many releases the
repositories carry, newest first), `ARQAN_REPO_IMAGE`.

## Limits

GitHub Pages allows a 1 GB site and 100 GB of bandwidth a month. A release adds
about 5 MB, and `ARQAN_REPO_KEEP` bounds how many are carried.
