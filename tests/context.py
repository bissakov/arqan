"""Per-test fixture: an isolated HOME/cwd, a mock provider, and pty sessions.

Every test gets a fresh temp directory, a fresh mock server on an ephemeral
port and a fresh request log, so tests never observe each other. The
environment handed to `yoke` is scrubbed down to a fixed set of variables — the
status line renders the cwd and the model name, so anything leaking in from
the developer's shell would show up in a golden file.
"""

from __future__ import annotations

import difflib
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from .harness import Session
from .harness.session import QUIET
from .mockprovider import MockProvider, Scenario

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "bin" / "yoke"
GOLDEN = Path(__file__).resolve().parent / "golden"

# Enough room for the status line's model · provider · cwd · tokens groups.
DEFAULT_COLS = 80
DEFAULT_ROWS = 24


class GoldenMismatch(AssertionError):
    pass


class Ctx:
    def __init__(self, case: str, update: bool = False, keep: bool = False):
        self.case = case
        self.update = update
        self.keep = keep
        self.tmp = Path(tempfile.mkdtemp(prefix="yoke-test-"))
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
        settled screen — so the window follows the pacing.
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
        p = self.xdg / "yoke" / "config"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
        return p

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
            "YOKE_BASE_URL": self.mock.base_url,
            "YOKE_API_KEY": "test-key",
            "YOKE_MODEL": "mock-model",
            "YOKE_SYSTEM_PROMPT": "You are a test fixture.",
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
        **env_overrides,
    ) -> Session:
        """Start `yoke` on a pty and wait for the first frame."""
        s = Session(
            [str(BIN)],
            env=self.env(**env_overrides),
            cwd=str(self.work),
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
                lambda t: t.contains("Message yoke") or t.contains("\u203a "),
                "first frame",
            )
            s.settle()
        return s

    def run_piped(self, stdin_text: str, timeout: float = 15.0, **env_overrides):
        """Run `yoke` with pipes instead of a tty (the line-oriented path)."""
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

    def check_text(self, actual: str, name: str | None = None):
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
