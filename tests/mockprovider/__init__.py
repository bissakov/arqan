"""Dummy OpenAI-compatible provider used by the TUI tests."""

from .server import MockProvider, Scenario

__all__ = ["MockProvider", "Scenario"]
