# Changelog

## [Unreleased]

### Added

- Let Build mode agents use `ask_user` to resolve a decision and continue the
  same turn.

### Fixed

- Keep `ask_user` questions visible above tall or multiline option lists.

## [0.1.0] - 2026-08-13

### Added

- Initial public release.
- Interactive terminal agent for OpenAI-compatible and Anthropic APIs.
- Streaming responses with built-in file, search, shell, patch, and web tools.
- Local configuration, credentials, conversation history, and sessions.
- Syntax highlighting through the bundled `arqan-highlight` executable.
- Portable Linux x86_64 archive, installer, checksum, and draft release
  automation.

[Unreleased]: https://github.com/bissakov/arqan/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/bissakov/arqan/releases/tag/v0.1.0
