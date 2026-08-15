# Changelog

## [Unreleased]

## [0.4.0] - 2026-08-15

### Changed

- Ship the portable archive as one relocatable executable. `arqan` and
  `arqan-highlight` in `arqan-X.Y.Z-linux-x86_64.tar.gz` are now static-pie
  musl builds that borrow no shared library and name no interpreter, so the
  archive runs on a musl distribution, on a glibc older than the packages
  ask for, and in a container with no libcurl installed. The `.deb` and
  `.rpm` still link the distribution's libc and libcurl and are the better
  choice where they install. ([`6566f35`])

- Give a picker the room the terminal has. A modal list - the question the
  agent asks, `/model`, `/resume`, the settings screen - now grows to about
  two thirds of the body instead of the eight rows the composer's completion
  popup uses, and a question too long for one row wraps over as many as four
  rather than being cut. A page of answers is read at once where before every
  question past the third had to be scrolled through. ([`686df1a`])

- Name a session from its first response instead of at the end of the turn.
  A build that runs tools for minutes was unnamed until it finished, and one
  that was interrupted was never named at all. The naming now happens as soon
  as the turn's first response is whole, from the user's message alone when
  the model opened with tool calls, and a Ctrl-C during it stops the turn.
  ([`614b416`])

- Measure glyph widths from an owned Unicode table instead of the C library's
  `wcwidth`, and decode UTF-8 without consulting the locale. A transcript of
  CJK, emoji or combining marks now frames identically whatever the C library
  and its Unicode version are, and under `LC_ALL=C` as under a UTF-8 locale,
  where before every byte of a wide glyph took a column of its own.
  ([`54cd5f7`])

### Fixed

- Find the TLS trust store at run time. libcurl's CA location is chosen when
  libcurl is built, so a binary that runs somewhere else - a container, a
  relocated build - could fail every HTTPS request with a verification error.
  `CURL_CA_BUNDLE`, `SSL_CERT_FILE` and `SSL_CERT_DIR` are honoured first, a
  working built-in default is left alone, and otherwise the usual
  distribution locations are searched.
  A default is now taken as working only when every store it names exists,
  and a resolved store answers for both of libcurl's options rather than
  leaving the other at a build-time path: libcurl loads the bundle and the
  hashed directory into one trust store, so either one naming somewhere
  absent failed the handshake on its own. ([`54cd5f7`])

- Leave a reasoning small model room to answer when it names a session. The
  naming request was capped at a line's worth of tokens, which a model that
  thinks first spent entirely on reasoning, so the session was never named.
  ([`8b6777b`])

## [0.3.0] - 2026-08-14

### Added

- Answer an unattended `ask_user` question automatically: after
  `ask_timeout_ms` (three minutes by default) with no key pressed, the picker
  takes the option the model recommended and tells it nobody read the
  question, so a user who stepped away costs a wait rather than the
  provider's prompt cache. Any key restarts the wait, a question that
  recommends nothing waits as before, and `ask_timeout_ms = 0` waits forever.
  A project config file may not set it. ([`73fa08d`])
- Name a saved session: set or edit the name with `/title`, see it in
  `/resume`, and let a configured small model name a session after its first
  turn. ([`0d187c3`])
- Pick the small model with `Ctrl-S` in `/model`, including one served by
  another provider: the choice is one model at one endpoint, and errands such
  as naming a session are sent there with that provider's key while the
  conversation stays where it is. Turn automatic session naming off with the
  `Name sessions` setting. ([`e98fc9a`])
- Offer every configured provider's models in one `/model` list. An entry is a
  model at a provider, named `<model> @ <provider>` when more than one
  provider serves the list, so one id offered by several endpoints stays
  several rows and can be searched for by provider name. Each open asks every
  provider for its models; one that cannot answer is named with the reason,
  and the models pinned on it are still offered. ([`e98fc9a`])

### Changed

- Make a provider a connection and nothing else. `/provider` adds, edits and
  removes endpoints and no longer switches between them, and `/model` is the
  one place a model is chosen: picking one selects the connection that serves
  it, with its URL, API and key. The choice is remembered as a `provider` and
  `model` pair in the state file, so a `model` line under a
  `[providers.<name>]` section is now only a default for a run that has chosen
  none, and pinned models are pinned per provider. ([`e98fc9a`])
- Keep the block a modal screen asks about on screen: the question of
  `ask_user`, a submitted plan, and the call awaiting approval lift the
  transcript out from under their options instead of being covered by them.
  Every other overlay still leaves the transcript where it was. ([`aac2dee`])
- Answer a click on a folded tool block mid-turn with a separate centered,
  scrollable and selectable window containing the complete block text,
  instead of deferring the fold until the turn ends. ([`6ac01f4`])

### Fixed

- Stop `/model` from writing a raw transport error into the transcript when a
  provider is unreachable. The listing failure is reported once, with the
  reason, in the line that names the providers the list is missing.
  ([`a79c5c2`])
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

[Unreleased]: https://github.com/bissakov/arqan/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/bissakov/arqan/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/bissakov/arqan/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/bissakov/arqan/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/bissakov/arqan/releases/tag/v0.1.0

[`6566f35`]: https://github.com/bissakov/arqan/commit/6566f35f11a17baf50ded550c68ec1a684852ae4
[`686df1a`]: https://github.com/bissakov/arqan/commit/686df1ab7cdc072c8fd229b66d30f46f80b6110b
[`614b416`]: https://github.com/bissakov/arqan/commit/614b41641aa115e89fb2dd47f0804798584fba72
[`54cd5f7`]: https://github.com/bissakov/arqan/commit/54cd5f78c07c91520b6293a9d97b4d03c8d84f5b
[`8b6777b`]: https://github.com/bissakov/arqan/commit/8b6777b4ccb4bfa30451adc908edebc241e2641d
[`73fa08d`]: https://github.com/bissakov/arqan/commit/73fa08d9b39c18378ccfef3af0617390a26636f6
[`0d187c3`]: https://github.com/bissakov/arqan/commit/0d187c32dc098d11f6b7c13f55c8e6053691ed3a
[`e98fc9a`]: https://github.com/bissakov/arqan/commit/e98fc9a60dca8eca563f4579cb32d59ef038a637
[`aac2dee`]: https://github.com/bissakov/arqan/commit/aac2deefbe4e18b5104cfffd02c2722ee2b372bb
[`6ac01f4`]: https://github.com/bissakov/arqan/commit/6ac01f4ead97e861365149a2f09e7feeb2294e78
[`a79c5c2`]: https://github.com/bissakov/arqan/commit/a79c5c23f37ae38a24e3cbc0fdef467fac8323cf
[`946fa99`]: https://github.com/bissakov/arqan/commit/946fa99a0bc75301ea3835e95ff6f9af7a749f73
[`e44b0ca`]: https://github.com/bissakov/arqan/commit/e44b0ca3628069c32acfbd0e2cd337f42f514add
[`44b4173`]: https://github.com/bissakov/arqan/commit/44b41738e25ed038807474616ea5f89a70c75302
[`b700a1e`]: https://github.com/bissakov/arqan/commit/b700a1ec55cea080448f96ccfefef3ef35326748
[`abdcee2`]: https://github.com/bissakov/arqan/commit/abdcee22870239b1bcaeafff6598cd05af8c1d91
[`8a0f493`]: https://github.com/bissakov/arqan/commit/8a0f493c2b84cc765eb12cc1bf2a58f7613e624d
[`0515ece`]: https://github.com/bissakov/arqan/commit/0515ece970c025a303b868d75dd3941b649ce158
