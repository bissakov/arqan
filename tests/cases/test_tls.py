"""TLS trust store: the environment, libcurl's default, then the distributions.

libcurl's CA location is fixed when libcurl is built, so a binary that moves
between machines - a container image, a relocated build, any static link -
carries a path that need not exist where it runs. `--ca-trust` reports the
resolution the next request would use; ARQAN_TEST_CA_ROOT hangs a trust store
off a directory the case controls and, by being set, keeps the host's own
store out of the answer.
"""


def trust(ctx, root=None, **env):
    """The resolved store as {"ca-file": ..., "ca-dir": ...}, "-" when unset."""
    if root is not None:
        env["ARQAN_TEST_CA_ROOT"] = str(root)
    out = ctx.run_cli("--ca-trust", **env)
    assert out.returncode == 0, out
    rows = dict(line.split(": ", 1) for line in out.stdout.splitlines())
    assert set(rows) == {"ca-file", "ca-dir"}, out.stdout
    return rows


def store(ctx, *paths):
    """A fake root with a bundle at each path, returned as (root, [full...])."""
    root = ctx.work / "root"
    made = []
    for path in paths:
        p = root / path.lstrip("/")
        p.parent.mkdir(parents=True, exist_ok=True)
        if path.endswith("/"):
            p.mkdir(parents=True, exist_ok=True)
        else:
            p.write_text("# not a certificate, only a file that exists\n")
        made.append(p)
    root.mkdir(parents=True, exist_ok=True)
    return root, made


def test_debian_bundle_is_found(ctx):
    """The bundle the majority of distributions install."""
    root, [bundle] = store(ctx, "/etc/ssl/certs/ca-certificates.crt")
    rows = trust(ctx, root)
    assert rows["ca-file"] == str(bundle), rows
    assert rows["ca-dir"] == "-", rows


def test_fedora_bundle_is_found(ctx):
    """A layout with no Debian bundle resolves to its own."""
    root, [bundle] = store(ctx, "/etc/pki/tls/certs/ca-bundle.crt")
    rows = trust(ctx, root)
    assert rows["ca-file"] == str(bundle), rows


def test_bundle_outranks_directory(ctx):
    """One file beats a lookup per chain, so a bundle wins when both exist."""
    root, [bundle, _] = store(
        ctx, "/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/certs/"
    )
    rows = trust(ctx, root)
    assert rows["ca-file"] == str(bundle), rows
    assert rows["ca-dir"] == "-", "a bundle answers alone"


def test_hashed_directory_when_no_bundle(ctx):
    """No bundle anywhere falls back to the hashed directory."""
    root, [certs] = store(ctx, "/etc/ssl/certs/")
    rows = trust(ctx, root)
    assert rows["ca-file"] == "-", rows
    assert rows["ca-dir"] == str(certs), rows


def test_bundle_must_be_a_file(ctx):
    """A directory sitting where a bundle belongs is not a bundle."""
    root, _ = store(ctx, "/etc/pki/tls/certs/ca-bundle.crt/")
    rows = trust(ctx, root)
    assert rows["ca-file"] == "-", rows


def test_no_store_is_reported_not_guessed(ctx):
    """Nothing found leaves libcurl's options alone and says so."""
    root, _ = store(ctx)
    out = ctx.run_cli("--ca-trust", ARQAN_TEST_CA_ROOT=str(root))
    assert out.returncode == 0, out
    assert "ca-file: -" in out.stdout, out.stdout
    assert "ca-dir: -" in out.stdout, out.stdout
    assert "no CA trust store" in out.stderr, out.stderr


def test_environment_outranks_the_probe(ctx):
    """An operator naming a store is the most local statement about trust."""
    root, [bundle] = store(ctx, "/etc/ssl/certs/ca-certificates.crt")
    rows = trust(ctx, root, SSL_CERT_FILE="/named/by/hand.pem")
    assert rows["ca-file"] == "/named/by/hand.pem", rows
    assert str(bundle) not in rows.values(), rows


def test_missing_named_store_is_honoured(ctx):
    """A named path that does not exist is a TLS error worth reporting, not a
    silent fall back to a store the operator declined."""
    root, _ = store(ctx, "/etc/ssl/certs/ca-certificates.crt")
    rows = trust(ctx, root, CURL_CA_BUNDLE="/gone.pem")
    assert rows["ca-file"] == "/gone.pem", rows


def test_curl_variable_outranks_openssl_variable(ctx):
    """CURL_CA_BUNDLE is libcurl's own; SSL_CERT_FILE is the fallback's."""
    rows = trust(
        ctx,
        ctx.work / "empty",
        CURL_CA_BUNDLE="/curl.pem",
        SSL_CERT_FILE="/openssl.pem",
    )
    assert rows["ca-file"] == "/curl.pem", rows


def test_certificate_directory_variable(ctx):
    """SSL_CERT_DIR names a hashed directory beside any bundle."""
    rows = trust(
        ctx, ctx.work / "empty", SSL_CERT_FILE="/bundle.pem", SSL_CERT_DIR="/hashed"
    )
    assert rows["ca-file"] == "/bundle.pem", rows
    assert rows["ca-dir"] == "/hashed", rows


def test_default_is_left_alone_without_a_test_root(ctx):
    """With no root the host decides, and a working default stays unset: an
    option this binary never sets is libcurl behaving as it always has."""
    out = ctx.run_cli("--ca-trust")
    assert out.returncode == 0, out
    assert "ca-file:" in out.stdout, out.stdout
