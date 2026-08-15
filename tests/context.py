"""Per-test fixture: an isolated HOME/cwd, a mock provider, and pty sessions.

Every test gets a fresh temp directory, a fresh mock server on an ephemeral
port and a fresh request log, so tests never observe each other. The
environment handed to `arqan` is scrubbed down to a fixed set of variables. The
status line renders the cwd and the model name, so anything leaking in from
the developer's shell would show up in a golden file.
"""

from __future__ import annotations

import difflib
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from .harness import Session
from .harness.session import QUIET
from .mockprovider import MockProvider, Scenario

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / os.environ.get("ARQAN_TEST_BIN", "bin/arqan")
# The agent looks for its highlighter beside its own executable, so a variant
# build (bin/asan) tests its own helper, never the shipped one.
HIGHLIGHT_BIN = BIN.parent / "arqan-highlight"
GOLDEN = Path(__file__).resolve().parent / "golden"


def _agent_version() -> str:
    """The version the binary reports, read from its single source.

    A release edits `AGENT_VERSION` and nothing else, so an assertion about
    the version must follow it rather than repeat it.
    """
    text = (ROOT / "src/agent.h").read_text()
    found = re.findall(
        r'^\s*#\s*define\s+AGENT_VERSION\s+"([^"]+)"', text, re.MULTILINE
    )
    assert len(found) == 1, "src/agent.h must define AGENT_VERSION once"
    return found[0]


VERSION = _agent_version()

# Enough room for the status line's model · provider · cwd · tokens groups.
DEFAULT_COLS = 80
DEFAULT_ROWS = 24


class GoldenMismatch(AssertionError):
    pass


def parse_settings(text: str) -> dict:
    """A settings file as {section: {key: value}}; "" holds its head.

    The format is a TOML subset, so a value may be quoted or bare; quotes and
    their escapes are removed here the way settings.c removes them.
    """
    out: dict = {"": {}}
    section = ""
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            out.setdefault(section, {})
            continue
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            body = v[1:-1]
            if v[0] == '"':
                body = (body.replace("\\n", "\n").replace("\\t", "\t")
                        .replace('\\"', '"').replace("\\\\", "\\"))
            v = body
        out[section][k.strip()] = v
    return out


class Ctx:
    def __init__(self, case: str, update: bool = False, keep: bool = False):
        self.case = case
        self.update = update
        self.keep = keep
        self.tmp = Path(tempfile.mkdtemp(prefix="arqan-test-"))
        # realpath: getcwd(3) resolves symlinks, and the status line shows it
        self.tmp = Path(os.path.realpath(self.tmp))
        self.home = self.tmp / "home"
        self.work = self.home / "work"
        self.xdg = self.home / "xdg"
        self.work.mkdir(parents=True)
        self.xdg.mkdir(parents=True)
        self.mock = MockProvider().start()
        self.sessions: list[Session] = []
        self._checked: list[str] = []
        self.quiet = QUIET

    # ---- provider ---------------------------------------------------------
    def scenario(self, spec: str | Scenario):
        """Set what the dummy provider streams back for the next turns.

        A paced scenario leaves gaps between deltas, and a quiet window
        shorter than one of those gaps would read a mid-stream pause as a
        settled screen, so the window follows the pacing.
        """
        self.mock.scenario = spec
        self.quiet = max(QUIET, self.mock.scenario.delay * 2.5)
        for s in self.sessions:
            s.quiet = self.quiet
        return self

    # ---- files ------------------------------------------------------------
    def write_file(self, relpath: str, content: str) -> Path:
        p = self.work / relpath
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
        return p

    def write_config(self, content: str) -> Path:
        p = self.config_file()
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
        return p

    def write_project_config(self, content: str, at: Path | None = None) -> Path:
        """A project's own settings, in <dir>/.arqan/config.toml."""
        p = (at or self.work) / ".arqan" / "config.toml"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
        return p

    def config_file(self) -> Path:
        return self.xdg / "arqan" / "config.toml"

    def state_file(self) -> Path:
        return self.home / ".local" / "state" / "arqan" / "state.toml"

    def settings(self, path: Path) -> dict:
        """A settings file as {section: {key: value}}; "" is its head."""
        if not path.exists():
            return {}
        return parse_settings(path.read_text())

    def state(self) -> dict:
        """The remembered choices: model, provider, telemetry."""
        return self.settings(self.state_file()).get("", {})

    # ---- environment ------------------------------------------------------
    def env(self, **overrides) -> dict:
        env = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": str(self.home),
            "XDG_CONFIG_HOME": str(self.xdg),
            "TERM": "xterm-256color",
            "LC_ALL": "C.UTF-8",
            "LANG": "C.UTF-8",
            # Colour stays on: the UI encodes the turn state in the status
            # bullet's colour and nowhere in text, so tests need the attribute
            # grid. Golden snapshots only record glyphs, so they are unaffected.
            "ARQAN_BASE_URL": self.mock.base_url,
            "ARQAN_API_KEY": "test-key",
            "ARQAN_MODEL": "mock-model",
            "ARQAN_SYSTEM_PROMPT": "You are a test fixture.",
            # A failing request is an answer here, not weather: a case that
            # wants the retry loop asks for it and pins its backoff.
            "ARQAN_RETRIES": "0",
            # Most cases exercise tool mechanics rather than approval. They
            # opt into trusted automation; Ask-focused cases override this.
            "ARQAN_PERMISSIONS": "free",
        }
        for k, v in overrides.items():
            if v is None:
                env.pop(k, None)
            else:
                env[k] = str(v)
        return env

    # ---- sessions ---------------------------------------------------------
    def spawn(
        self,
        cols: int = DEFAULT_COLS,
        rows: int = DEFAULT_ROWS,
        wait: bool = True,
        cwd: str | None = None,
        args: list[str] | None = None,
        **env_overrides,
    ) -> Session:
        """Start `arqan` on a pty and wait for the first frame."""
        s = Session(
            [str(BIN), *(args or [])],
            env=self.env(**env_overrides),
            cwd=cwd or str(self.work),
            cols=cols,
            rows=rows,
            name=self.case,
            quiet=self.quiet,
        )
        s.start()
        self.sessions.append(s)
        if wait:
            # The placeholder is the one piece of chrome that is always there
            # and never depends on the configured model or provider.
            s.wait_for(
                lambda t: t.contains("Message arqan") or t.contains("\u203a "),
                "first frame",
            )
            s.settle()
        return s

    def run_cli(self, *args: str, stdin_text: str = "", timeout: float = 15.0,
                **env_overrides):
        """Run `arqan` with arguments and pipes, for the non-interactive paths."""
        return subprocess.run(
            [str(BIN), *args],
            input=stdin_text,
            env=self.env(**env_overrides),
            cwd=str(self.work),
            capture_output=True,
            text=True,
            timeout=timeout,
        )

    def run_piped(self, stdin_text: str, timeout: float = 15.0, **env_overrides):
        """Run `arqan` with pipes instead of a tty (the line-oriented path)."""
        return subprocess.run(
            [str(BIN)],
            input=stdin_text,
            env=self.env(**env_overrides),
            cwd=str(self.work),
            capture_output=True,
            text=True,
            timeout=timeout,
        )

    # ---- golden files -----------------------------------------------------
    def golden_path(self, name: str | None) -> Path:
        stem = self.case if not name else f"{self.case}.{name}"
        return GOLDEN / f"{stem}.txt"

    def check_screen(self, session: Session, name: str | None = None, label: str = ""):
        """Compare the settled screen against its golden file.

        `name` is only needed when one case snapshots more than once.
        """
        session.settle()
        return self.check_text(session.snapshot(label), name)

    # A tool result and the spinner row both carry a clock, which is the one
    # thing on screen a rerun cannot reproduce. Layout is what a golden is
    # about, so the number is normalised and its shape is left to assert on.
    ELAPSED = re.compile(r"\u00b7 (\d+ms|\d+\.\d+s|\d+m\d\ds|\d+s)")

    def check_text(self, actual: str, name: str | None = None):
        actual = self.ELAPSED.sub("\u00b7 <t>", actual)
        path = self.golden_path(name)
        self._checked.append(path.name)
        if self.update or not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(actual)
            return actual
        expected = path.read_text()
        if expected != actual:
            diff = "".join(
                difflib.unified_diff(
                    expected.splitlines(True),
                    actual.splitlines(True),
                    fromfile=f"golden/{path.name}",
                    tofile="actual",
                )
            )
            raise GoldenMismatch(
                f"screen does not match {path.name}\n{diff}\n"
                "(re-run with --update after confirming the change is wanted)"
            )
        return actual

    # ---- teardown ---------------------------------------------------------
    def cleanup(self, failed: bool = False):
        for s in self.sessions:
            s.close()
        self.mock.stop()
        if self.keep and failed:
            print(f"  [kept] {self.tmp}")
        else:
            shutil.rmtree(self.tmp, ignore_errors=True)
