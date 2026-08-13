# Changelog

## [Unreleased]

### Added

- Add native Debian/Ubuntu and RPM Linux release packages.
- Let Build mode agents use `ask_user` to resolve a decision and continue the
  same turn.
- Let users configure context windows and reasoning controls for exact
  provider/model pairs, without a built-in provider or model registry.

### Changed

- Make the portable Linux archive installer-free and replace its sidecar
  checksum with one manifest covering the archive and native packages.
- Abbreviate context token counts in the status line with compact `k` and `M`
  units, matching the context window.
- Keep provider setup focused on transport and credentials; configure model
  capabilities from the model picker's visible key actions instead.
- Stop inferring context windows from non-standard model-listing fields.

### Fixed

- Keep `ask_user` questions visible above tall or multiline option lists.
- Wrap picker values as well as their descriptions, and keep section
  separators out of keyboard selection.

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
