"""External key stores: a provider whose key arqan asks for rather than keeps.

The helpers are stubs on PATH, so these cases exercise arqan's side of the
contract without needing a session bus, a keyring daemon or a GPG key.
"""

import os
import stat

from .test_provider import credentials_file, select_provider


def fake_bin(ctx):
    """A bin directory on PATH, ahead of anything the developer has."""
    d = ctx.tmp / "bin"
    d.mkdir(exist_ok=True)
    return d


def with_path(ctx, **env):
    """Env overrides that put the stub helpers first on PATH."""
    return {"PATH": f"{fake_bin(ctx)}:{os.environ.get('PATH', '/usr/bin:/bin')}",
            **env}


def write_script(ctx, name, body):
    p = fake_bin(ctx) / name
    p.write_text("#!/usr/bin/env python3\n" + body)
    p.chmod(p.stat().st_mode | stat.S_IEXEC)
    return p


def keyring_file(ctx):
    return ctx.tmp / "keyring.txt"


def install_secret_tool(ctx):
    """A secret-tool that keeps "account<TAB>secret" lines in one file."""
    write_script(ctx, "secret-tool", f"""
import sys
from pathlib import Path

STORE = Path({str(keyring_file(ctx))!r})
args = sys.argv[1:]
op, rest = args[0], args[1:]
attrs = dict(zip(rest[0::2], rest[1::2]))
if op == "store":
    # --label <text> precedes the attribute pairs.
    attrs = dict(zip(rest[2::2], rest[3::2]))
account = attrs.get("account", "")
if attrs.get("service") != "arqan" or not account:
    sys.exit(2)

lines = STORE.read_text().splitlines() if STORE.exists() else []
kept = [l for l in lines if l.split("\\t", 1)[0] != account]
if op == "lookup":
    for l in lines:
        k, _, v = l.partition("\\t")
        if k == account:
            print(v)
            sys.exit(0)
    sys.exit(1)
if op == "store":
    secret = sys.stdin.readline().strip()
    STORE.write_text("\\n".join(kept + [account + "\\t" + secret]) + "\\n")
    sys.exit(0)
if op == "clear":
    STORE.write_text("\\n".join(kept) + ("\\n" if kept else ""))
    sys.exit(0)
sys.exit(2)
""")


def seed_keyring(ctx, account, secret):
    install_secret_tool(ctx)
    with keyring_file(ctx).open("a") as f:
        f.write(f"{account}\t{secret}\n")


def write_credentials(ctx, text):
    c = credentials_file(ctx)
    c.parent.mkdir(parents=True, exist_ok=True)
    c.write_text(text)
    c.chmod(0o600)


def write_endpoint(ctx, name, ctx_url, model="mock-model"):
    p = ctx.config_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("a") as f:
        f.write(f"[providers.{name}]\nbase_url = {ctx_url}\nmodel = {model}\n")


def start_with(ctx, name, **env):
    """Spawn against a stored provider, with nothing supplied by the env."""
    select_provider(ctx, name)
    return ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None,
                     **with_path(ctx, **env))


# ---- reading ---------------------------------------------------------------

def test_a_key_from_the_system_keyring_reaches_the_provider(ctx):
    """key_source names a store; the key itself is nowhere in arqan's files."""
    seed_keyring(ctx, "work", "sk-from-keyring")
    write_endpoint(ctx, "work", ctx.mock.base_url)
    write_credentials(ctx, "[providers.work]\nkey_source = secret-service\n")
    ctx.scenario("text=ok")

    s = start_with(ctx, "work")
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.auth[-1] == "Bearer sk-from-keyring", ctx.mock.auth


def test_no_key_store_keeps_the_credentials_file_working(ctx):
    """A provider written before key stores existed still reads its key."""
    write_endpoint(ctx, "work", ctx.mock.base_url)
    write_credentials(ctx, "[providers.work]\nkey = sk-in-file\n")
    ctx.scenario("text=ok")

    s = start_with(ctx, "work")
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.auth[-1] == "Bearer sk-in-file", ctx.mock.auth


def test_an_unknown_key_source_is_refused_rather_than_guessed(ctx):
    """Falling back to the file would send a key the user moved off it."""
    write_endpoint(ctx, "work", ctx.mock.base_url)
    write_credentials(ctx, "[providers.work]\nkey_source = wallet\nkey = sk-file\n")
    s = start_with(ctx, "work")
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("enter")
    s.wait_text("unknown key_source")


def test_a_missing_helper_is_reported_not_silently_ignored(ctx):
    """Nothing on PATH answers, so the run says so instead of losing the key."""
    write_endpoint(ctx, "work", ctx.mock.base_url)
    write_credentials(ctx, "[providers.work]\nkey_source = secret-service\n")
    # Only the empty stub directory: a secret-tool the developer happens to
    # have installed would answer, and this case is about one that is absent.
    select_provider(ctx, "work")
    s = ctx.spawn(ARQAN_BASE_URL=None, ARQAN_API_KEY=None, ARQAN_MODEL=None,
                  PATH=str(fake_bin(ctx)))
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("enter")
    s.wait_text("not installed")


# ---- the config file may not choose what runs ------------------------------

def test_a_key_command_in_the_config_file_is_never_run(ctx):
    """The config file is shared and committed, so it stays inert data.

    A key store directive is a request to execute a program. Honouring one
    from the config file would turn a synced dotfile repository into remote
    code execution, so it is reported and dropped.
    """
    marker = ctx.tmp / "config-command-ran"
    helper = write_script(ctx, "evil", f"""
from pathlib import Path
Path({str(marker)!r}).write_text("pwned")
print("sk-evil")
""")
    p = ctx.config_file()
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(f"[providers.work]\nbase_url = {ctx.mock.base_url}\n"
                 f"model = mock-model\nkey_source = command\n"
                 f"key_command = {helper}\n")
    write_credentials(ctx, "[providers.work]\nkey = sk-real\n")
    ctx.scenario("text=ok")

    s = start_with(ctx, "work")
    s.submit("hello")
    s.wait_turn_done()
    assert not marker.exists(), "the config file executed a program"
    assert ctx.mock.auth[-1] == "Bearer sk-real", ctx.mock.auth


def test_a_key_command_in_the_credentials_file_is_run(ctx):
    """The 0600 machine-local file is the one place a directive is honoured."""
    helper = write_script(ctx, "helper", 'print("sk-from-command")\n')
    write_endpoint(ctx, "work", ctx.mock.base_url)
    write_credentials(ctx, f"[providers.work]\nkey_source = command\n"
                           f"key_command = {helper}\n")
    ctx.scenario("text=ok")

    s = start_with(ctx, "work")
    s.submit("hello")
    s.wait_turn_done()
    assert ctx.mock.auth[-1] == "Bearer sk-from-command", ctx.mock.auth


def test_a_key_command_is_not_run_through_a_shell(ctx):
    """Words are argv, so a metacharacter cannot become a second command."""
    marker = ctx.tmp / "shell-ran"
    helper = write_script(ctx, "helper", 'print("sk-plain")\n')
    write_endpoint(ctx, "work", ctx.mock.base_url)
    write_credentials(
        ctx,
        f"[providers.work]\nkey_source = command\n"
        f"key_command = {helper} ; touch {marker}\n")
    ctx.scenario("text=ok")

    s = start_with(ctx, "work")
    s.submit("hello")
    s.wait_turn_done()
    assert not marker.exists(), "key_command reached a shell"
    assert ctx.mock.auth[-1] == "Bearer sk-plain", ctx.mock.auth


# ---- writing ---------------------------------------------------------------

def test_choosing_the_keyring_writes_no_key_to_disk(ctx):
    """/provider stores the secret in the keyring and the name of the store."""
    install_secret_tool(ctx)
    ctx.scenario("models=alpha")
    s = ctx.spawn(**with_path(ctx))
    s.submit("/provider")

    s.wait_text("a name for this provider")
    s.type("work").sync()
    s.key("enter")
    s.wait_status("which API does it speak")
    s.key("enter")
    s.wait_text("its base URL")
    s.type(ctx.mock.base_url).sync()
    s.key("enter")
    for _ in range(5):
        s.key("enter")
    s.wait_text("its API key")
    s.type("sk-topsecret").sync()
    s.key("enter")
    s.wait_status("where should the key be kept")
    s.key("down", "enter")
    s.wait_status("pick a model")
    s.key("enter")
    s.wait_text("provider: work")

    section = ctx.settings(credentials_file(ctx)).get("providers.work", {})
    assert section.get("key_source") == "secret-service", section
    assert not section.get("key"), section
    assert "sk-topsecret" in keyring_file(ctx).read_text()
    for path in list(ctx.xdg.rglob("*")) + list((ctx.home / ".local").rglob("*")):
        if path.is_file():
            assert "sk-topsecret" not in path.read_text(), path


def test_a_provider_name_that_is_not_a_bare_key_is_not_a_provider(ctx):
    """A name lands in a "[providers.<name>]" header, so it is a TOML key.

    A section a TOML reader would reject is not an endpoint arqan will use: it
    would mean the file arqan writes and the file an editor parses disagree.
    The keyring holds a key for the name either way, and it stays unused.
    """
    seed_keyring(ctx, "Local Claude", "sk-spaced")
    write_endpoint(ctx, "Local Claude", ctx.mock.base_url)
    write_credentials(ctx, "[providers.Local Claude]\nkey_source = secret-service\n")

    s = start_with(ctx, "Local Claude")
    s.wait_text("no provider yet")
    assert ctx.mock.auth == [], ctx.mock.auth


def test_a_provider_name_that_could_become_an_option_is_refused(ctx):
    """A leading dash or a slash would change what the helper is asked."""
    seed_keyring(ctx, "-w", "sk-nope")
    write_endpoint(ctx, "-w", ctx.mock.base_url)
    write_credentials(ctx, "[providers.-w]\nkey_source = secret-service\n")
    s = start_with(ctx, "-w")
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("enter")
    s.wait_text("provider name")


def test_an_existing_key_can_move_stores_without_being_retyped(ctx):
    """Moving a key must not require the user to have it to hand.

    arqan can already read the key it holds, so asking for it again is busy
    work that pushes people to leave it in the file.
    """
    install_secret_tool(ctx)
    write_endpoint(ctx, "work", ctx.mock.base_url, model="alpha")
    write_credentials(ctx, "[providers.work]\nkey = sk-was-in-file\n")
    ctx.scenario("models=alpha")

    s = start_with(ctx, "work")
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("down", "down", "enter")           # + edit a provider
    s.wait_status("edit a provider")
    s.key("enter")
    s.wait_text("base URL")
    s.key(*(["enter"] * 7))
    s.wait_status("which API does it speak")
    s.key("enter")
    s.wait_status("API key")
    s.key("down", "down", "enter")           # Move
    s.wait_status("where should the key be kept")
    s.key("down", "enter")                   # System keyring
    s.wait_text("provider: work")

    section = ctx.settings(credentials_file(ctx)).get("providers.work", {})
    assert section.get("key_source") == "secret-service", section
    assert not section.get("key"), section
    assert "sk-was-in-file" in keyring_file(ctx).read_text()


def test_the_key_menu_names_the_store_that_holds_it(ctx):
    """Where a key lives is not discoverable unless the menu says so."""
    write_endpoint(ctx, "work", ctx.mock.base_url, model="alpha")
    write_credentials(ctx, "[providers.work]\nkey_source = pass\n")
    ctx.scenario("models=alpha")

    s = start_with(ctx, "work")
    s.submit("/provider")
    s.wait_status("pick a provider")
    s.key("down", "down", "enter")
    s.wait_status("edit a provider")
    s.key("enter")
    s.wait_text("base URL")
    s.key(*(["enter"] * 7))
    s.wait_status("which API does it speak")
    s.key("enter")
    s.wait_status("API key")
    assert "pass" in s.text(), s.text()
