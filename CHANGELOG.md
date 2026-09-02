# Changelog

## [Unreleased]

### Added

- Delegate an investigation with the `task` tool. It starts a subagent with
  its own conversation that can only read, search and fetch. The task runs in
  the background while the parent keeps working. Poll it with `task(id=N)`,
  or add `wait_ms` to wait for the written report.

- Run one task at a time, or as many as `subagent_tasks` allows, up to eight.
  A start over the limit is refused until a task is collected or dropped, and
  the task tool is told how many the session allows. Each task keeps its own
  id, transcript and report. Ctrl-O shows the task last started or last
  polled. A project config file may not set `subagent_tasks`, so a repository
  cannot widen what a turn spends.

- Add `subagents` to turn the `task` tool on and off, `subagent_model` to run
  the delegate on the small model, `subagent_tasks` to set how many tasks run
  at once, and `subagent_slice_ms` to set how long a fallback slice runs when
  a worker cannot start. All four are rows of the settings screen.

- Watch a delegated task. It now has its own transcript, shown the same way
  the conversation is. Ctrl-O switches to it and back, it updates as the task
  works, and it names the model and provider the task runs on. The transcript
  stays there after the task reports, until the next task starts or the
  conversation is cleared.

- Name the delegate's model and provider in the report the parent reads, so
  a resumed session still says what the task ran on.

- Keep the description of a picked answer in the transcript. An `ask` result
  used to show the label alone; it now shows the detail the question offered
  with it, live and in a resumed session.

- Send `prompt_cache_key` on every OpenAI request. An OpenAI-compatible
  endpoint routes a request to a machine by the prompt's prefix and this key,
  so one value per working directory keeps a session's turns, and the next
  session in that directory, on the machine that already holds the prefix.
  The value is a hash; the path is not sent. A `reasoning_template` that sets
  the same field is now refused, as one that sets any other field the request
  owns already is.

### Fixed

- Let the transcript scroll while the search box is open. Page Up, Page Down
  and the wheel used to snap the view back to the current match on the next
  repaint. The view now follows a match only when the search moves to it.

- Keep the ask for a step list out of the transcript. The ask is appended to
  a tool result for the model to read. A shell result then ended with it
  instead of with its exit line, so the ask was rendered where the exit code
  belongs and the exit code was pushed into the output.

- Drop the search highlight when the box closes. Matches stayed painted after
  Escape until the query was cleared by hand.

- Keep the leading backslash when a rewind reloads a message. A message that
  starts with `/` or `!` is typed with a `\` in front so it is sent instead of
  run. The picker used to put the message back without it, so sending it again
  ran a command or a shell line.

- Link the math library, so a build without link-time optimization and
  section garbage collection resolves the `ceil` call in the vendored HTML
  parser. `make asan` and `make test-asan` failed to link on a system whose
  linker will not take a math symbol from `libc` alone.

- Open a session read-only when another instance already has it live. Two
  runs used to append to the same file at once. The instance that owns a
  session keeps writing; a second one that resumes it replays the transcript,
  says it is read-only, and refuses to send, so `/fork` is how to carry on in
  a copy. The picker marks such a session, and Ctrl-X leaves it alone.
  Sessions are independent, so any number of instances can run their own.

- Stop leaving a note in place of an older call's arguments where the model
  reads it as an example. A call to `read`, `grep` or `find` old enough to be
  dropped now leaves the conversation whole, with its result, instead of
  being described; so does a call the tool refused. `patch` and `write` keep
  their arguments however old the call is, so a retry has something to work
  from. Notes that remain name the call they stand for.
- Refuse a tool call built from the note left where an older call's arguments
  were dropped. A model that read the note as an example used to have the call
  run and fail with a missing argument.
- Keep one result per tool call when a session was answered twice, which
  happens when a second run resumes a session the first is still writing.
  The provider used to refuse the whole conversation.
- Redraw tables and rules when the terminal is resized. A table drawn for a
  wide window used to break apart once the window narrowed; it is now laid
  out again for the new width.
- Keep the scrolled transcript in place when the terminal is resized. The
  view used to slide towards the end of the conversation as the window
  changed width.

## [0.7.0] - 2026-08-25

### Added

- Track long work as a step list. The model keeps a checklist for a task of
  several steps: the transcript shows it, the status line counts what is
  left, and `/todo` prints it. The list is part of the conversation, so it
  survives a resumed session and a `/compact`. Turn it off with `-d todo`.
  ([`f46c4ca`])

- Publish signed apt, dnf and pacman repositories on GitHub Pages, so a
  release installs and upgrades with the system package manager. The landing
  page carries the install steps and the signing key. ([`a48577c`])

- Add `elide_at`, the share of the context window at which old tool output
  goes out as a note instead of in full. It defaults to 75 and must stay
  below `compact_at`. ([`403fec6`])

- Report a rebuilt prompt cache in the transcript, with what caused it and
  how much room it bought. A rebuild the session did not cause stops the
  turn; set `cache_guard` to `warn` to keep the report without the stop, or
  to `off` for neither. ([`403fec6`])

### Fixed

- Grant a `bash` or `job` wait that asks for longer than the deadline allows
  as the longest wait allowed, instead of refusing the call. ([`16e12b8`])

- Keep going when an OpenAI-compatible endpoint answers from an older
  request's cached prefix. The turn now says the cache is behind instead of
  stopping as if the session were at fault. ([`2da0497`])

- Refuse a malformed number in JSON instead of reading part of it. A tool
  argument written as `1.2.3` used to arrive as `1.2`. ([`6234220`])

- Keep wide table rows inside the table. A row longer than the line buffer,
  which non-Latin scripts reach quickly, closed the table early and printed
  the rest as raw pipes. ([`98e1ab8`])

### Changed

- With `resume_last` on, a directory left at the welcome screen by `/clear`
  is greeted again on the next start instead of reopening the cleared
  conversation. ([`c203546`])

- Elide old tool output far less often. The boundary now moves only under
  context pressure instead of every few rounds, so it stops throwing away the
  provider's cached prefix. ([`403fec6`])

- Keep prompt history per directory, so one project no longer recalls
  another's prompts. The old shared file is kept as `history.global`.
  ([`a8a3a8e`])

- Draw a rule between table rows when a cell wraps, so a row that spans
  several lines has a visible end. Tables whose rows fit on one line are
  unchanged. ([`98e1ab8`])

## [0.6.0] - 2026-08-21

### Added

- Add `resume_last` to reopen the newest session in the current directory. It
  is off by default. ([`da28f93`])

- Add `/restart` to restart the process and reload settings. ([`da28f93`])

- Add PNG, JPEG, GIF and WebP attachments with `/attach <path>`. A message can
  contain four images. Sessions preserve attachments. Images over 5 MB or
  8000 pixels per side are refused. ([`7c753bd`])

- Add clipboard image attachments with Ctrl-V or `/attach` without a path.
  ([`7c753bd`])

- Attach supported images selected from the `@` path picker. ([`7c753bd`])

- Add `images = off` to disable image tools and resumed attachments. Project
  configuration cannot enable images. ([`7c753bd`])

- Add manual and automatic context compaction. It summarizes older messages
  and preserves recent rounds. Settings control the mode, threshold and model.
  ([`a770655`])

### Changed

- Store session titles in the first JSONL record instead of separate files.
  ([`38e87f7`])

- Make `read` identify binary files instead of returning invalid text. Image
  results include the type, size and dimensions. ([`7c753bd`])

- Add syntax highlighting to tool blocks opened in a separate window.
  ([`a6777b3`])

### Fixed

- Keep the previous file when a `write` tool call cannot finish safely. A
  destination symlink is replaced instead of changing its target. ([`4a425a2`])

- Report failed session saves and retry messages that were not persisted.
  ([`47f24a6`])

- Improve tool errors for failed patches, invalid JSON, limits and file access.
  ([`7c55970`])

- Accept common patch envelopes, return current context after failed hunks and
  keep patch diagnostics valid UTF-8. ([`49b007e`])

- Make the advertised shell output limit match the accepted limit.
  ([`49b007e`])

- Retry syntax highlighting after a slow response instead of disabling it.
  ([`c83b9c2`])

- Exclude omitted older tool output from the context usage estimate.
  ([`a770655`])

- Preserve recent rounds when `/compact` uses a declared context window.
  ([`a770655`])

- Show an estimated context size immediately after resuming a session.
  ([`a770655`])

- Show the delete key when the session picker opens. ([`c83b9c2`])

- Preserve output received while a background job exits. ([`a186c42`])

## [0.5.0] - 2026-08-16

### Added

- Run long commands in the background. A `bash` command still going after two
  minutes keeps running as a job, and the new `job` tool waits for it, lists
  jobs, or stops one. `shell_timeout_ms` sets the two minutes; 0 waits for
  every command. Jobs end with the session. ([`6efb5bf`])

- Ship an Arch Linux package. The release now carries a
  `arqan-X.Y.Z-1-x86_64.pkg.tar.zst` next to the `.deb`, the `.rpm` and the
  portable archive, so Arch and its derivatives install with `pacman -U`
  and remove with `pacman -R`. It carries the same binaries as the `.rpm`,
  and a file manifest `pacman -Qkk` verifies. ([`3af8d2c`])

### Changed

- Start about 1.5 ms sooner. libcurl's dependency tree - OpenSSL, krb5,
  nghttp2, brotli, zstd and a dozen others - was loaded at exec, which cost
  some four times everything else the program did before its first frame, and
  a session that never asks a provider anything never needs it. It is now
  opened at the first request, and a machine without it is told so there
  rather than refusing to start. What must be installed is unchanged: every
  package names libcurl itself. ([`0c7a904`])

- Mark a user turn on every row. What the reader wrote was told from what the
  model answered by a background one shade off the transcript's, and a fenced
  code block inside a user turn dropped even that and took the same slab the
  model's code is painted with, so a message carrying code or a table read as
  a reply. A user turn now carries a rule down its left edge on every row it
  covers - code, tables and the padding around it included - and its panel
  runs unbroken, with a fence inside it shaded as part of the turn. The rule
  is a glyph, so the turn is still marked under `NO_COLOR`. ([`21b4801`])

- Show a command instead of summarising it. A tool call's header used to cut
  its command at a row's worth of bytes, which hid the tail of most pipelines
  and every quoted argument past it; it now shows what will run. A command
  longer than the new width still ends in an ellipsis, and the block offers a
  "show in full" row that expands it in one click, so nothing a call does is
  out of reach without turning verbose output on. Tool *output* is unchanged.
  ([`b3baa6d`])

- Lift the transcript for a notice. A one-line answer to a command that
  opens nothing - "copied the last response", "exported session to ..." -
  used to cover the newest transcript row for as long as it stayed up, which
  hid the end of the last reply. It now takes a row of its own, the way the
  activity spinner does. Pickers, the settings screen and the completion
  popup still cover the transcript instead of moving it. ([`abde625`])

- Paint every frame inside a synchronized update. Terminals that support
  DEC mode 2026 (kitty, WezTerm, Ghostty, foot, recent tmux) now hold the
  display until the frame is complete, so a repaint that leaves the process
  in several writes - a fast stream, a slow pipe, an ssh link - is shown
  whole rather than half drawn. Terminals without the mode ignore it.
  ([`44afdd2`])

### Fixed

- Stop reading ready with a message in hand. Pressing Enter emptied the
  composer one frame before the status left ready, so an accepted message
  spent that frame on screen under an idle status, looking like a message
  that had gone nowhere. The frame that takes the message now says so, and a
  form that stops to ask for a value reads ready while it waits, since
  waiting on the reader is not work. ([`0b6afa1`])

- Keep a wrapped command out of the justifier. With justified wrapping on, a
  command, file content or diff that ran past one row had the gaps of its
  wrapped rows widened like prose, which moved the columns of the code under
  it. Those rows now keep the spacing they were written with. ([`25131e2`])

- Keep a session through a power loss. Each save now reaches the disk before
  the call returns, instead of sitting in the page cache behind whatever the
  next tool does, and the directory entry of a new session file is persisted
  with it. A session cut off between a round asking for tools and running
  them now resumes: the calls nothing answered are answered as interrupted,
  which the provider accepts and the model can act on, and the answer is
  written back so the next resume finds the file whole. A line torn in half
  by the cut is also closed before the next append, which used to run onto
  its tail and cost the first message after it. ([`331950f`])

- Keep a command's password prompt off the screen. A command that wanted a
  human, `sudo` above all, opened the terminal behind the closed standard
  streams: its `[sudo] password for ...` was painted into the composer box
  and its read of the keyboard either raced the composer or stopped the
  command until Ctrl-C. Commands now run in a session of their own with no
  terminal to open, so `sudo` reports that it cannot ask for a password and
  exits, and the agent is told as much. Run `arqan` itself under `sudo` if
  that is what the work needs. ([`21a7cf1`])

- Stop a Ctrl-C from ending the next `!` command as well. Interrupting a
  local run left the interrupt pending, so the command after it reported
  itself interrupted without running. ([`21a7cf1`])

- Stop claiming a copy under tmux succeeded. tmux ships with `set-clipboard
  external`, which forbids an application inside it from setting the
  clipboard and answers nothing, so `/copy` and a drag-select reported
  "copied" for text that never left tmux. Both now name the option that
  carries it, and the README says what to set for the clipboard and for
  desktop notifications. ([`c283505`])

- Stop the `libcurl.so.4: no version information available` warning the rpm
  printed on every run. Debian versions libcurl's symbols and the rpm
  distributions do not, so the package's binaries, built on Debian 11, asked
  each rpm host for a `CURL_OPENSSL_4` that is not defined there. The rpm is
  now built on EL9 against its own family's libcurl and its dependencies are
  no longer filtered, so it declares what it actually needs: glibc 2.34 or
  newer. Hosts below that take the portable archive, which needs nothing.
  ([`2021585`])

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

[Unreleased]: https://github.com/bissakov/arqan/compare/v0.7.0...HEAD
[0.7.0]: https://github.com/bissakov/arqan/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/bissakov/arqan/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/bissakov/arqan/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/bissakov/arqan/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/bissakov/arqan/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/bissakov/arqan/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/bissakov/arqan/releases/tag/v0.1.0

[`f46c4ca`]: https://github.com/bissakov/arqan/commit/f46c4ca47365d0c4dbb1e01af0b170ca9bfb2c39
[`a48577c`]: https://github.com/bissakov/arqan/commit/a48577c779726db76ccdf9989df95473eea0f3dd
[`403fec6`]: https://github.com/bissakov/arqan/commit/403fec613497120afa8423faacc22a97856e180e
[`16e12b8`]: https://github.com/bissakov/arqan/commit/16e12b86324b52d0cdee5589b8fad52761fb3f80
[`2da0497`]: https://github.com/bissakov/arqan/commit/2da0497296a25187fdbf5a32de30cc1dd7649930
[`6234220`]: https://github.com/bissakov/arqan/commit/623422017dbc0cab9cf280d15d1e8daad0666700
[`98e1ab8`]: https://github.com/bissakov/arqan/commit/98e1ab867fbd13198093f56d5864c5b5a3c627b5
[`c203546`]: https://github.com/bissakov/arqan/commit/c2035468fd878b5a0015b767f1561c0d9235ff7e
[`a8a3a8e`]: https://github.com/bissakov/arqan/commit/a8a3a8ee4555b2fc8914139be510558dd9967e29
[`da28f93`]: https://github.com/bissakov/arqan/commit/da28f9386de009c4327772815dc7058499d52304
[`7c753bd`]: https://github.com/bissakov/arqan/commit/7c753bdc35cf19af3d93ffddb21cc330901909d6
[`a770655`]: https://github.com/bissakov/arqan/commit/a770655380d68cb56fe994b4c9af757dca1ce511
[`38e87f7`]: https://github.com/bissakov/arqan/commit/38e87f742998210bf778e12ccca75b959084a305
[`a6777b3`]: https://github.com/bissakov/arqan/commit/a6777b3ff277427de140a913ab00010b74aac1f5
[`4a425a2`]: https://github.com/bissakov/arqan/commit/4a425a23804e668662a8d1c983f9f40ce647b0a6
[`47f24a6`]: https://github.com/bissakov/arqan/commit/47f24a68f77149c28b1d1344fa5be5726a5c4a4c
[`7c55970`]: https://github.com/bissakov/arqan/commit/7c55970a96fd013d1ce74b11fcf4fcadef88b814
[`49b007e`]: https://github.com/bissakov/arqan/commit/49b007eaf2a0f2e9f32eeac6e5b59ce7afde9acc
[`c83b9c2`]: https://github.com/bissakov/arqan/commit/c83b9c271832c1e2b67113a6bd3b9a157ba05f02
[`a186c42`]: https://github.com/bissakov/arqan/commit/a186c426f05a2b9acb16cf1ed38de047c2f5cd44
[`6efb5bf`]: https://github.com/bissakov/arqan/commit/6efb5bf697325810417ca7e0a0f8c7200e53cbef
[`3af8d2c`]: https://github.com/bissakov/arqan/commit/3af8d2c7e512f266f87ca341f514e88373ccb32e
[`0c7a904`]: https://github.com/bissakov/arqan/commit/0c7a9042fb2af783b05a20ba2ed5c5a049b36f80
[`21b4801`]: https://github.com/bissakov/arqan/commit/21b48017f3cab4b10751ecb8d7517e1901978b5c
[`b3baa6d`]: https://github.com/bissakov/arqan/commit/b3baa6dc7775dedfd2bca82de3c995d476266d55
[`abde625`]: https://github.com/bissakov/arqan/commit/abde62581ee3607af3a75f471159b3e0dc6807eb
[`44afdd2`]: https://github.com/bissakov/arqan/commit/44afdd2f3eab6b2635986807bf9a4b8b703e3d29
[`0b6afa1`]: https://github.com/bissakov/arqan/commit/0b6afa182b420c95c44d08186e1c8a3855e1160c
[`25131e2`]: https://github.com/bissakov/arqan/commit/25131e29c9429a15e60e1ed2a74cc1d9a230fba0
[`331950f`]: https://github.com/bissakov/arqan/commit/331950f3e506f4763701cc3e1cae0abb787b3469
[`21a7cf1`]: https://github.com/bissakov/arqan/commit/21a7cf14e62e8fc030b1960f3dbb21776439139e
[`c283505`]: https://github.com/bissakov/arqan/commit/c28350532070cd8d268bba1f0f13722f585b3958
[`2021585`]: https://github.com/bissakov/arqan/commit/202158580abd54ad75e6ad0a287c9336c688bb4d
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
