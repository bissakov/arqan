# yoke

A terminal AI coding agent written in plain C17, designed as a counterpoint to
Claude Code, Codex, OpenCode and Pi. It talks to any OpenAI-compatible
chat-completions endpoint or any Anthropic-compatible messages endpoint,
streams the reply into a fullscreen TUI, and gives the model six tools: read,
write, bash, patch, grep and find.

## Design principles

- **C17, single translation unit (unity) build**, so full rebuilds stay under a second.
- **Structure-of-Arrays (SoA)** for hot data (messages, tool calls, tool registry).
- **No heap allocations at all in our code.** Everything lives in two static
  blocks wrapped in arenas; per-turn scratch is freed by resetting a pointer,
  and the only heap traffic is libcurl's own.
- **Every bound is checked.** Sizes derived from a provider stream, a config
  file or the filesystem are validated before they reach an allocation, and a
  full arena or a full conversation is reported rather than written past.
- **Minimal deps**: libc and libcurl. JSON, SSE, TUI and the tool runner are
  hand-written.

## Layout

```
src/
  yoke.h          single umbrella header (all types + fwd decls)
  core.c          arena, Str, Buf, log, time
  json.c          tiny arena JSON parser + serializer
  http.c          libcurl POST (SSE or whole reply) + plain GET
  paths.c         XDG base directory resolution
  settings.c      the "key = value" file format, and the state file
  endpoints.c     user-defined providers (/provider), their API and their keys
  history.c       prompt history, mirrored to the XDG state dir
  session.c       per-directory saved conversations (/resume, /fork)
  config.c        env + XDG config file loader
  prompt.c        system prompts: SYSTEM.md / PLAN.md + AGENTS.md, expansion
  cli.c           command line parsing, above the config in precedence
  provider.c      chat completions and Anthropic messages: streaming, tools
  tools.c         SoA tool registry + read/write/bash/patch/grep/find tools
  tui.c           alternate-screen TUI, viewport, composer + raw input
  markdown.c      streaming Markdown rendering of a reply
  main.c          unity includes + main + agent loop
tests/
  run.py          test runner (Python 3, no third-party packages)
  harness/        terminal emulator + pty driver
  mockprovider/   dummy provider, both APIs (lorem ipsum, tool calls)
  cases/          the tests
  golden/         expected screen dumps
```

## Build & run

```
make
./bin/yoke
```

The first run with nothing configured says so on the welcome screen and waits:
`/provider`, then "+ add a provider", asks for a name, which API the endpoint
speaks, a base URL and a key. The same command switches between the providers
already stored. Until one exists a message is answered with that line rather
than sent, since the only reply an unconfigured endpoint has is an HTTP 401.
Nothing is built in, since only you know which endpoints you have.
Its status is `setup`; no default model or endpoint is presented as if it were
configured. The line-oriented banner uses the same setup state.

There are two APIs to pick from: `openai`, the chat-completions protocol
nearly every service speaks, and `anthropic`, the messages one. They differ in
the path a request goes to, the header the key rides in and the shape of a
message, and in nothing you can see: the transcript, the tools and the
settings are the same either way.

A provider is a section of the config file, so what you edit by hand and what
`/provider` writes are one document:

```
[provider openai]
base_url = https://api.openai.com/v1
model = gpt-4o-mini
api = openai

[provider anthropic]
base_url = https://api.anthropic.com/v1
model = claude-sonnet-4-5
api = anthropic
```

### Provider reasoning options

Reasoning controls are provider-scoped. `/provider` → `Edit` can set them,
and `/settings` cycles each configured list (including `Off`) for the active
provider. They may also be edited in the provider section:

```
reasoning_efforts = low,medium,high
thinking_budgets = 1024,4096,8192
reasoning_effort = medium
thinking_budget = 4096
reasoning_template = {"vendor_effort":"$reasoning_effort","budget":"$thinking_budget","cache_control":{"type":"ephemeral"}}
```

Lists keep their order. Effort labels must be nonempty and unique; budgets
must be unique positive integers. The selected value is optional and must be
one from its corresponding list. With an OpenAI-compatible provider an active
effort automatically sends `reasoning_effort`; with an Anthropic-compatible
provider an active budget automatically sends
`thinking: {"type":"enabled","budget_tokens":N}`.

`reasoning_template` is an optional, single-line JSON object merged into each
request. It can add nonstandard fields (including reversed API conventions).
The exact JSON strings `"$reasoning_effort"` and `"$thinking_budget"` are
replaced with the active string and integer respectively. A template is
refused locally if it is malformed, duplicates or conflicts with request
fields yoke owns, or references a control that is Off. Templates live in the
configuration file, not credentials: never put API keys or other secrets in
them.

The key is not there. It lives under the same section name in
`$XDG_STATE_HOME/yoke/credentials` at mode 0600, so the file worth keeping in
a dotfile repository carries no secret. A credentials file anyone else can
read is refused rather than loaded: that key wants rotating, not using.

A write by the UI touches the keys it owns and nothing else, so comments, key
order and settings yoke knows nothing about survive it.

An endpoint can also be named per invocation, which is what a script or a
throwaway local server wants:

```
export YOKE_BASE_URL=https://api.openai.com/v1
export YOKE_MODEL=gpt-4o-mini
export YOKE_API_KEY=sk-...
export YOKE_API=openai          # or anthropic
```

or put them in `$XDG_CONFIG_HOME/yoke/config`, by default
`~/.config/yoke/config`:

```
base_url = https://api.openai.com/v1
model = gpt-4o-mini
api_key = sk-...
api = openai             # the wire format base_url speaks: openai or anthropic
max_tokens = 32768       # cap on one reply; a turn that reaches it stops mid-sentence
stream = true            # paint replies as they arrive
mode = build             # build or plan
max_messages = 4096      # conversation capacity; a full one is reported, not overrun
retries = 3              # further attempts for a request that reached nothing
retry_delay_ms = 500     # the wait before the first, doubling up to 8s
disable_tools = bash     # tools no turn may call, comma separated
verbose_tools = false    # show complete tool blocks
raw_markdown = false     # do not render Markdown
show_ignored = false     # include ignored files in the @ picker
show_instructions = false
wrap = word              # word or justified
status_fields = 511      # bit mask for /statusline fields
```

Every file yoke owns is written this way: `key = value` lines, `#` comments,
values unquoted to the end of the line, grouped under `[section]` headers when
there is more than one thing to configure. The keys above are the head of the
file, above the first header.

A turn whose request failed before a single byte of the reply arrived is sent
again, up to `retries` times, and the transcript says so in red each time. A
stream that died halfway is not: those bytes are already on screen, so the
turn ends on the error instead of repeating itself. Neither is a refusal that
answers the request rather than the network: an HTTP 401 or 404 fails at once.

Remembered UI choices win over config files, environment variables win over
remembered choices, and command-line options win over all of them. The
matching UI environment variables are `YOKE_STREAM`, `YOKE_MODE`,
`YOKE_MAX_TOKENS`, `YOKE_DISABLE_TOOLS`, `YOKE_VERBOSE_TOOLS`,
`YOKE_RAW_MARKDOWN`, `YOKE_SHOW_IGNORED`, `YOKE_SHOW_INSTRUCTIONS`,
`YOKE_WRAP`, and `YOKE_STATUS_FIELDS`. A model picked with `/model` is
remembered on its provider, or as `model` in state when none is selected.

Command line options outrank all of it, being the most local statement about
one invocation:

```
yoke --help
yoke -m gpt-4o --base-url https://api.openai.com/v1
yoke --api anthropic -u https://api.anthropic.com/v1 -m claude-sonnet-4-5
yoke -p "summarise src/tui.c"    # one turn, reply on stdout, then exit
yoke "summarise src/tui.c"       # the same: a bare argument is the prompt
yoke --disable-tools bash,write,patch   # a read-only run for a model you distrust
```

A successful one-shot run writes only its final assistant response to stdout,
verbatim with one trailing newline. Reasoning and intermediate assistant prose
are suppressed; bounded tool calls, tool results, retries, and errors go to
stderr. A failed turn exits nonzero and leaves stdout empty.

### The system prompt

Write your own in `.yoke/SYSTEM.md` for one project, or in
`$XDG_CONFIG_HOME/yoke/SYSTEM.md` for every session. The project file wins
when both exist, and it is looked up from the working directory upwards, so a
subdirectory still gets the project's prompt. `--system` and
`YOKE_SYSTEM_PROMPT` outrank both; with none of them, a built-in prompt is
used. It is not a config key: a prompt is a document, not a setting.

What only startup knows goes in as a placeholder:

| placeholder | expands to |
| --- | --- |
| `{tools}` | one `- name: description` line per registered tool |
| `{cwd}` | the directory yoke was launched in |

```markdown
You are a terse reviewer for this C17 codebase.

Available tools:
{tools}
Current working directory: {cwd}

Run `make test` after touching src/.
```

So a prompt written once keeps describing the tools that exist now. Anything
else in braces is left exactly as written, `{"a": 1}` included. A SYSTEM.md
larger than 64 KiB (about 65k characters) is refused at startup with an error
naming the file: a truncated prompt would be a different prompt.

### Project context

`AGENTS.md` is the project's instructions rather than yours, so it does not
compete with the prompt: it is appended to whichever prompt won, `--system`
included. Every one from the working directory up to the root applies,
outermost first, because a subdirectory refines its parent instead of
replacing it. It is a document about the project, not a template, so braces
in it stay exactly as written and the same 64 KiB limit applies.

## Using it

Enter sends and Alt+Enter inserts a newline; a pasted line break is one too,
so pasting several lines fills the composer rather than sending the first of
them. Ctrl-C discards the draft or
cancels a running turn, Esc also cancels one, and Ctrl-D quits from an empty
composer. Esc at an idle composer, twice, goes back to an earlier message: the
list is ordered like the transcript, so it opens on the last turn and Up walks
further back, and the message picked returns to the composer with everything
after it dropped. Up/Down recall past prompts, PageUp/PageDown and the mouse wheel
scroll the transcript, and Ctrl-L repaints. Dragging selects any text on
screen and copies it over OSC 52 on release, so it works over ssh; Shift falls
back to the terminal's own selection.

Anything that takes a while says so under the transcript: one spinner row
naming what is running, whether that is the model thinking, the reply arriving,
a tool call or a `!` command, next to the seconds it has been running and the
key that ends it. The row is painted rather than written, so it leaves when the
work does and the transcript keeps none of it, and while it is up the status
line says the state in the colour of its bullet alone rather than repeating the
word a row below. A tool call is timed twice over: its own seconds next to the
turn's while it runs, and the time it took at the end of its result line once
it is done, which the saved session keeps so `/resume` reads the same.

A line starting with `!` is a shell command rather than a message: the `!`
becomes the prompt marker itself, red in place of the blue one, so the composer
holds the command alone. Submitting it runs it here rather than asking the
model for anything, and the command and its output read in the transcript the
way a tool call does. The run takes a turn of its own in the conversation, so
the model sees what was run on the next message, a saved session keeps it and
`/resume` brings it back.

Typing `/` opens a completion popup: `/clear` starts a fresh conversation,
`/resume` reopens one saved for this directory,
`/fork` continues in a copy of the conversation and leaves the session it was
forked from where `/resume` can still find it, `/model` switches model,
`/provider` switches provider or adds one,
`/mode` switches between Build and Plan, as Shift+Tab does,
`/rewind` goes back to an earlier message, as the double Esc does,
`/copy` puts the last reply on the clipboard as the Markdown the model wrote,
`/settings` opens the screen below, `/about` shows project information and
credits, `/help` starts a fresh session with a generated first user message
that inventories the live configuration, providers, tools, prompt sources,
settings and resolved paths, and `/exit` quits. API key presence is reported,
but key values are never included. `/help` waits for the user's next message
before contacting the provider. A few commands answer to a
second name: `/config` finds `/settings`, `/new` finds `/clear` and `/quit`
finds `/exit`, and the popup lists the command rather than the alias.
An unescaped leading `/` is reserved for these commands. An unknown command is
rejected locally with a hint and never reaches the provider. Prefix command or
shell-looking model text with a backslash: `\/clear` sends `/clear`, and
`\!important` sends `!important`. History keeps the escaped spelling for
recall. An unescaped leading `!` continues to run a local shell command.

A word starting with `@` is a path being picked rather than typed: the same
popup lists what the directory holds, folders first and marked with a slash
the way `ls -A --group-directories-first --indicator-style=slash` does, and
typing searches. The search is not the directory's own entries: it goes down
the tree, so `@widget` finds `src/deep/widget.c` without naming a folder on
the way, and the letters need only appear in order, so `@srmain` finds
`src/main.c`. A name that starts with what was typed comes first, then one
that holds it, then a path that does, then the scattered match, and shallower
beats deeper. Descending still works: `@src/` lists that folder and scopes
everything typed after it to what is below.
Tab or Enter takes the highlighted entry into the composer,
which for a folder is a step into it and lists its contents instead of
answering. Nothing is submitted by picking: the path lands in the message, `@`
and all, and the message is sent when you send it.

What the project excludes is not offered. `.gitignore` and `.ignore` say the
same thing about a path, so both are read, every one from the working
directory down to the folder being listed, with `!` bringing a path back the
way git does; `.git` is never offered at all. That is a default rather than a
rule: "Ignored files" in `/settings` offers them anyway, and a path typed by
hand always reaches anything.

### Settings

`/settings` is where the rest of them live, since a toggle is not worth a
command of its own. The rows are the session's: untruncated tool output, raw
Markdown, whether a reply is streamed or arrives whole, whether the `@` picker
offers ignored paths, the telemetry record below, the text wrap, the mode, the
token cap a turn is sent with, and one row per tool. Every row is changed
where it is read: Space or Right acts on the selected one, Left acts on it
backwards, and Enter or Escape close. Nothing here opens a screen of its own
or a question, since a setting a reader has to walk to is a setting they have
to find twice; the token cap steps between the values on either side of it
rather than being typed. The screen is its own answer: a box that stayed empty
is a setting that refused to change.

The model and the provider are not rows: they name the endpoint a session
talks to rather than how it behaves, and `/model` and `/provider` choose them.

Every choice is applied immediately and remembered in the atomic state file,
including the status-line mask from `/statusline`. A write failure does not
undo the runtime change and is reported as "not remembered". Telemetry and
provider reasoning retain their existing storage paths. UI keys are
namespaced, including `ui_stream`, `ui_mode`, `ui_disable_tools`, and
`ui_status_fields`.

### Turning tools off

The last rows of `/settings` are one checkbox per tool, each saying in one
line what the tool does rather than repeating the description the model is
sent, and what is turned
off there is withheld from the schemas the next turn is sent with and refused
if the model asks for it anyway, since a schema offered earlier in the
conversation is still in its context. It is the same promise plan mode makes,
made one tool at a time: a model being tested against a real working tree
cannot run `bash` it was not given. The two plan-mode tools are not rows,
because the agent loop answers those and a mode that cannot end is not a
setting.

A session that should start that way says so before it starts, with
`disable_tools` in the config file, `YOKE_DISABLE_TOOLS`, or
`--disable-tools`; each replaces the one below it rather than adding to it.
Applied before the system prompt is built, so a disabled tool is absent from
the listing the model reads as well as from what it is offered. A name no tool
answers to ends the run rather than being ignored: a typo in a list whose
point is that `bash` cannot run is worth hearing about.

### Telemetry

Telemetry records what a session did, for a bug report. It is off until it
is asked for, and the answer is remembered, so a run that never reaches the
composer records too. Events are JSON objects, one per line, appended to the
record of the conversation they belong to,
`$XDG_STATE_HOME/yoke/telemetry/<cwd>/<timestamp>.jsonl`, named after that
conversation's session file: `/clear` starts a record as it starts a
conversation and `/resume` continues the one it reopened. What is recorded
before a conversation exists, a startup or the `/resume` that picks one, waits
for the file that session names, so opening yoke and resuming leaves that
conversation's record and nothing beside it; a run that ends with lines still
waiting writes them to a record named after the run. Each file opens with the
session and its settings, and then holds each
turn and how long it took, every request with its size, its SSE event count
and its token usage, every tool call with its duration and outcome, the
commands and mode switches, the arenas as they fill, and the log lines yoke
would otherwise only print to stderr.

A transfer gets an event of its own, since most of what goes wrong with an
endpoint is invisible from the transcript: the HTTP status and curl's own
result, the phases it spent its time in (DNS, connect, TLS, time to first
byte, total), the bytes each way, the HTTP version and address family, and,
for a streamed turn, how many SSE lines arrived, how many waits the event loop
took and the longest the stream went silent, which is what "it froze" looks
like in numbers. The endpoint is a hash and a class (loopback, TLS) rather
than a URL, because a private host names its owner as surely as a path does;
the request path is yoke's own (`/chat/completions`, `/models`), so it is
recorded as it is.

What it records is the shape of a session and none of its content. A message
is a byte and a line count, a tool call is its name and the *keys* of its
arguments rather than the path or the command they carry, a reply is its size
and its tokens, and the working directory is a hash, so two projects can be
told apart without either being named. The strings in the file are the ones
yoke formats itself: a tool name, a model id, an HTTP status, a log line. A
command yoke does not offer is recorded as `(unknown)`, since a line it does
not know is the user's own text.

### Plan mode

Shift+Tab switches between the two modes the status line names. Build is the
one that works: it reads, writes, patches and runs commands. Plan reads and
proposes, and changes nothing, because `write` and `patch` are not in the
registry it is offered and are refused if it asks for one anyway. It has a
system prompt of its own, resolved like the other one from `.yoke/PLAN.md`,
the global `PLAN.md` or a built-in template, and two tools the other mode does
not have.

`ask_user` is how it asks: the question and its options open in the picker,
the option it recommends is where the list opens, and a last row hands the
composer over for an answer it did not think of. `submit_plan` is how it
finishes: the plan is rendered as the Markdown it wrote, and the question
under it is what happens next.

| answer | what it does |
| --- | --- |
| Yes | switches to Build mode and carries the plan out in the same turn |
| Yes, but from a new session | continues in a new session holding the plan alone, so the work starts without the conversation that produced it |
| No | ends the turn in Plan mode, so the next message refines the plan |

Escape is a No, since a dismissed question is not an approval. The session the
handover left behind keeps everything that was said while planning, where
`/resume` can still find it.

A reply is Markdown, and the transcript renders it: headings, lists, block
quotes, thematic breaks and fenced code become shapes, emphasis and inline
code become styles, and the markers themselves are dropped. Rendering follows
the stream, so a delta is painted as soon as its shape is known and only an
unclosed marker waits for its closer. The raw setting turns it off and shows the reply
exactly as the model wrote it, transcript included, since what is on screen is
a rendering of the conversation and the toggle applies to it too; `/copy`
copies that source either way, and a
one-shot `-p` run is never rendered, since its stdout is a reply rather than a
view.

A tool call reads as the tool, what it acts on and a preview of what it
carries, with a `patch` shown as the diff it is, and its result as a summary line: the
exit status of a command, the size of a file, the error a failure returned.
Both previews are capped so one tool cannot take the scrollback; the verbose
setting lifts the caps and shows every line, for the blocks already on screen as well
as the next ones. A single block answers to the pointer: its `N more lines`
tail is drawn as a link and brightens under the cursor, clicking it unfolds
that block alone, `show less` folds it back, and the block keeps its place on
screen while the transcript is replayed around it.

`/model` lists what the provider's `/models` endpoint serves and remembers the
choice for the next run. Every picker also offers manual model entry. If
`/models` fails or returns no usable IDs, the error is shown and manual entry
opens directly; Escape cancels without changing state. Provider add and edit
allow an unverified manual model and label it clearly. `/provider` lists the
endpoints you have stored, plus the entry that adds one. Past ten model IDs the
popup takes the keyboard and typing filters it by literal substring, so
nothing typed while it is open reaches the composer.

## Files

yoke follows the XDG Base Directory Specification and writes nothing directly
into `$HOME`. A relative value in an `XDG_*` variable is invalid per the spec
and ignored as if unset, and directories yoke creates are mode 0700.

| what | where | note |
| --- | --- | --- |
| settings | `$XDG_CONFIG_HOME/yoke/config` | keys and `[provider ...]` sections; every `$XDG_CONFIG_DIRS` entry is searched too, at lower precedence |
| system prompt | `$XDG_CONFIG_HOME/yoke/SYSTEM.md` | used for every session; `$XDG_CONFIG_DIRS` searched too, at lower precedence |
| project prompt | `.yoke/SYSTEM.md` | nearest one at or above the working directory, and it wins over the global one |
| plan prompt | `.yoke/PLAN.md`, `$XDG_CONFIG_HOME/yoke/PLAN.md` | what Plan mode is told instead, resolved the same way |
| project context | `AGENTS.md` | every one at or above the working directory, appended to the prompt |
| prompt history | `$XDG_STATE_HOME/yoke/history` | last 500 prompts, recalled in the composer with Up/Down |
| remembered choices | `$XDG_STATE_HOME/yoke/state` | model, provider, telemetry, and namespaced durable UI choices |
| provider keys | `$XDG_STATE_HOME/yoke/credentials` | one `key` per `[provider ...]` section, mode 0600, refused when anyone else can read it |
| telemetry record | `$XDG_STATE_HOME/yoke/telemetry/<cwd>/<timestamp>.jsonl` | the anonymized record of one conversation, named after its session file |
| sessions | `$XDG_DATA_HOME/yoke/sessions/<cwd>/<timestamp>.jsonl` | one file per conversation, keyed by the directory it ran in |

## Tests

```
make test                    # run the suite
make test T="-k composer"    # run matching cases
make test-update             # accept intended golden-screen changes
make test-asan               # the same suite against an ASan+UBSan binary
```

`bin/yoke` runs unmodified inside a pseudo-terminal against a dummy provider
that speaks both APIs, and its output is replayed into a small terminal
emulator, so the tests assert on the rendered screen rather than on escape
sequences. The provider streams customisable lorem ipsum, tool calls, token
usage and HTTP errors, and doubles as a standalone server for driving the UI by
hand without an API key:

```
make mock MOCK_ARGS="--port 8080 --scenario words=80,chunk=2,delay=0.05"
YOKE_BASE_URL=http://127.0.0.1:8080/v1 YOKE_API_KEY=x YOKE_MODEL=mock ./bin/yoke
```

See `tests/README.md` for the scenario language and how to write a case.
