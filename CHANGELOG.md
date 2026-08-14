# Changelog

## [Unreleased]

### Added

- Name a saved session: set or edit the name with `/title`, see it in
  `/resume`, and let a configured small model name a session after its first
  turn.
- Pick a provider's small model with `Ctrl-S` in `/model`, and turn automatic
  session naming off with the `Name sessions` setting.

### Changed

- Answer a click on a folded tool block mid-turn with a separate centered,
  scrollable and selectable window containing the complete block text,
  instead of deferring the fold until the turn ends. ([`6ac01f4`])

### Fixed

- Stop the model picker from reading past the command table when a picked
  entry sits beyond it. ([`946fa99`])

## [0.2.0] - 2026-08-14

### Added

- Add native Debian/Ubuntu and RPM Linux release packages. ([`e44b0ca`])
- Let Build mode agents use `ask_user` to resolve a decision and continue the
  same turn. ([`44b4173`])
- Let users configure context windows and reasoning controls for exact
  provider/model pairs, without a built-in provider or model registry.
  ([`b700a1e`])

### Changed

- Make the portable Linux archive installer-free and replace its sidecar
  checksum with one manifest covering the archive and native packages.
  ([`e44b0ca`])
- Abbreviate context token counts in the status line with compact `k` and `M`
  units, matching the context window. ([`abdcee2`])
- Keep provider setup focused on transport and credentials; configure model
  capabilities from the model picker's visible key actions instead.
  ([`b700a1e`])
- Stop inferring context windows from non-standard model-listing fields.
  ([`b700a1e`])

### Fixed

- Keep `ask_user` questions visible above tall or multiline option lists.
  ([`8a0f493`])
- Wrap picker values as well as their descriptions, and keep section
  separators out of keyboard selection. ([`0515ece`])

## [0.1.0] - 2026-08-13

### Added

- Initial public release.
- Interactive terminal agent for OpenAI-compatible and Anthropic APIs.
- Streaming responses with built-in file, search, shell, patch, and web tools.
- Local configuration, credentials, conversation history, and sessions.
- Syntax highlighting through the bundled `arqan-highlight` executable.
- Portable Linux x86_64 archive, installer, checksum, and draft release
  automation.

[Unreleased]: https://github.com/bissakov/arqan/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/bissakov/arqan/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/bissakov/arqan/releases/tag/v0.1.0

[`6ac01f4`]: https://github.com/bissakov/arqan/commit/6ac01f4ead97e861365149a2f09e7feeb2294e78
[`946fa99`]: https://github.com/bissakov/arqan/commit/946fa99a0bc75301ea3835e95ff6f9af7a749f73
[`e44b0ca`]: https://github.com/bissakov/arqan/commit/e44b0ca3628069c32acfbd0e2cd337f42f514add
[`44b4173`]: https://github.com/bissakov/arqan/commit/44b41738e25ed038807474616ea5f89a70c75302
[`b700a1e`]: https://github.com/bissakov/arqan/commit/b700a1ec55cea080448f96ccfefef3ef35326748
[`abdcee2`]: https://github.com/bissakov/arqan/commit/abdcee22870239b1bcaeafff6598cd05af8c1d91
[`8a0f493`]: https://github.com/bissakov/arqan/commit/8a0f493c2b84cc765eb12cc1bf2a58f7613e624d
[`0515ece`]: https://github.com/bissakov/arqan/commit/0515ece970c025a303b868d75dd3941b649ce158
