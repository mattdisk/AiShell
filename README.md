# AiShell

AiShell is an interactive Unix shell written in C99 against POSIX APIs. It parses each line through a tokenizer and a recursive-descent parser into an abstract syntax tree (AST), then executes it with its own pipeline, redirection, and job-control code. Commands are dispatched to an in-process registry of builtins or to external programs via `fork`/`execvp`.

On top of that core, AiShell adds an optional natural-language mode. A line starting with `@` is sent to the configured AI backend. The default `mock` backend returns a fixed proposal. With `AISHELL_AI_BACKEND=claude`, AiShell runs the Claude Code CLI headless as a child process, and Claude may consult a small MCP server embedded in the shell. AiShell turns the model's structured reply into a single command proposal. A local validator checks the proposal, and the user must confirm it before the normal executor runs it.

## Highlights

- **Grammar-driven front end.** A tokenizer and a recursive-descent parser follow the EBNF in [`grammar.ebnf`](grammar.ebnf) and build an explicit AST. An optional debug mode prints the AST as a text tree and as Graphviz DOT.
- **Process and job control.** Pipelines use `pipe`/`dup2`/`fork`/`execvp`, with per-job process groups (`setpgid`), terminal hand-off (`tcsetpgrp`), background jobs and `SIGCHLD`-driven reaping.
- **Builtin registry.** 44 registered command names across 37 builtin source files under [`src/builtins/`](src/builtins/). Each is registered in a function-pointer table, and options are parsed with [argtable3](https://github.com/argtable/argtable3) using a shared `-h` / `--help` / `--json` convention.
- **Systems components written from scratch.** These include a socket-level HTTP/1.0 client (`http-get`), a small package manager (`pkg`) and multicall (`argv[0]`) dispatch.
- **Human-in-the-loop AI command proposals.** Claude Code is launched headless with timeouts, retries and error classification. Its JSON reply is parsed strictly, the command is checked by a local rule-based validator, and the user confirms before anything runs.
- **Embedded MCP server in C.** It speaks JSON-RPC over loopback HTTP on a background thread, uses Bearer-token authentication, and exposes four read-only or validation-oriented tools.

## Architecture

```
                      ┌──────────────── REPL (src/main.c) ────────────────┐
  user input ───────▶ │ line starts with '@' ?                            │
                      └───────┬──────────────────────────────┬────────────┘
                              │ no                           │ yes
                              ▼                              ▼
           tokenize_line   (src/token.c)        at_command_handle   (src/ai/ai_generate.c)
                 │                                   │  mock backend, or
           parse_input     (src/parser.c)            │  claude_invoke ──▶ claude -p … (child process)
                 │                                   │                      │ optional MCP calls
              AST          (src/ast.c)               │                      ▼
                 │                                   │      MCP server thread (src/ai/mcp_server.c)
           execute_ast     (src/exec.c)              │
                 │                                   ├─ ai_json_parse_result   (src/ai/ai_json.c)
    ┌────────────┼──────────────┐                    ├─ ai_validate_command    (src/ai/ai_validate.c)
    ▼            ▼              ▼                    ├─ user confirmation
 builtin     builtin in     fork/execvp              └─ execute_line(command) ──▶ same executor
 (main       pthread        pipeline / external
 thread)     (joined)       command
```

The `@` path never has its own execution mechanism. An approved proposal is handed to `execute_line()`, the same entry point used for typed commands.

## Shell Execution Model

### From text to AST

`execute_line()` in [`src/exec.c`](src/exec.c) runs every command through the same stages:

1. **Tokenize** ([`src/token.c`](src/token.c)). The tokenizer recognizes words and the operators `|`, `<`, `>`, `>>` and `&`. It handles double and single quotes; single-quoted text is marked so that `$VAR` inside it is not expanded.
2. **Parse** ([`src/parser.c`](src/parser.c)). A recursive-descent parser implements the grammar in [`grammar.ebnf`](grammar.ebnf):
   ```
   inputline ::= assignment | pipeline redirection* ['&']
   ```
   - A single `NAME=VALUE` token becomes an assignment node.
   - Redirections wrap the whole pipeline. The executor applies `<` to the first stage and `>`/`>>` to the last.
   - Syntax errors such as a dangling `|` or a missing redirect target produce specific messages instead of being executed.
3. **AST** ([`src/ast.h`](src/ast.h)). Node types are `AST_COMMAND`, `AST_PIPELINE`, `AST_REDIRECT`, `AST_BACKGROUND` and `AST_ASSIGNMENT`. In AST debug mode (`./aishell --ast`, `AISHELL_AST=1` or the `ast on` builtin), [`src/ast_print.c`](src/ast_print.c) prints a text tree and writes `ast.dot`. It also renders `ast.png` if Graphviz `dot` is installed.
4. **Execute** (`execute_ast`). Assignments call `setenv`. Other nodes are flattened into pipeline segments; `$NAME`/`${NAME}` expansion is applied to arguments and redirect targets, then globs are expanded with `glob(3)`.

### Dispatch rules

| Situation | How it runs |
|---|---|
| Single foreground builtin from a fixed list of output-only commands (`help`, `jobs`, `pwd`, `history`, `echo`, `printf`, `type`, `true`, `false`, `date`, `env`) | In a worker thread created with `pthread_create` and immediately joined. This isolates the builtin in its own thread, but it does not make execution parallel. |
| Any other single foreground builtin (`cd`, `export`, `alias`, `kill`, file commands, …) | Directly on the main thread, so it can change shell state. |
| Single builtin with redirection | On the main thread. stdin/stdout are saved with `dup`, redirected with `dup2`, then restored. |
| Pipelines, background jobs, external commands | `run_pipeline()` forks one child per stage. Builtins inside a pipeline run in the child, so they cannot change the parent shell. Other commands use `execvp`. |

### Job control

- **Process groups.** Every forked job gets its own process group. `setpgid` is called in both parent and child to close the fork race, and all stages of a pipeline share the first child's group.
- **Interactive mode.** The shell becomes its own group leader, ignores `SIGINT`/`SIGTSTP`/`SIGTTOU`/`SIGTTIN`, and uses `tcsetpgrp` to give a foreground job the terminal and take it back afterwards. As a result, Ctrl+C reaches the foreground job but not the shell or background jobs.
- **Background jobs.** They are recorded in a mutex-protected job table. The `SIGCHLD` handler is installed without `SA_RESTART`, so a finished job interrupts the blocking `fgets()`, and the REPL reaps it and reports completion immediately.
- **Job builtins.** `jobs`, `kill` (which can signal a whole process group) and `wait` operate on that table and on process IDs.

The exit status of a pipeline is the status of its last stage.

Building with `make demo` defines `DEMO_TRACE`. The shell then logs to stderr which path each command takes: worker thread, main thread, or forked child.

## Builtins and Supporting Components

Each builtin is a `static int run(int argc, char **argv)` function registered by name through `registry_add()` ([`src/registry.h`](src/registry.h)). Lookups go through `find_command()`, and anything not in the registry is treated as an external program. The same registry powers:

- **Multicall dispatch.** If the binary is invoked under another name (for example through a symlink named `pwd`), it runs that builtin directly.
- **One-shot mode.** `./aishell CMD [ARGS…]` runs a single command and exits.

| Area | Commands | Notes |
|---|---|---|
| Files | `ls`, `stat`, `cat`, `head`, `tail`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch` | `head`/`tail` read stdin when no file is given. |
| Search and edit | `rg`, `edit-replace-line`, `edit-insert-line`, `edit-delete-line`, `edit-replace` | `rg` is AiShell's own POSIX ERE (`regcomp`/`regexec`) search over named files. It is not ripgrep and does not recurse into directories. |
| Shell state | `cd`, `pwd`, `env`, `export`, `unset`, `type`, `alias`, `unalias`, `history`, `ast` | History is kept in memory for the session. |
| Processes | `jobs`, `kill`, `wait`, `ps`, `sleep` | `ps` reports only the shell's own process. |
| Networking | `http-get`, `http-post` | `http-get` sends an HTTP/1.0 GET over plain TCP sockets (no TLS), with connect, send and receive timeouts ([`src/net/socket_client.c`](src/net/socket_client.c)). `http-post` is a registered placeholder. |
| Packages | `pkg build`, `pkg install`, `pkg list`, `pkg info`, `pkg remove` | Builds and installs tarball packages under `~/.mysh`; AiShell prepends `~/.mysh/bin` to `PATH` at startup. |
| Misc | `help`, `echo`, `printf`, `date`, `true`, `false`, `vi`, `less` | `vi` and `less` launch the system programs. |

Builtins exist only inside AiShell's process. An external program such as `find -exec` cannot invoke them.

## Natural-Language Command Mode (`@`)

Illustrative output with the Claude backend. By default the `mock` backend proposes `pwd`; setting `AISHELL_MOCK_AI_JSON` overrides that response.

```
aishell> @ show the first 20 lines of README.md
Going to execute:
  head -n 20 README.md
OK to continue? Press Enter to run, or type anything else to cancel:
```

### Request flow

1. **Entry point.** The REPL detects a leading `@` and passes the rest of the line to `at_command_handle()` ([`src/ai/ai_generate.c`](src/ai/ai_generate.c)). This only happens in the interactive REPL.
2. **Backend selection.** The backend comes from configuration. The **default is `mock`**, which returns a fixed `pwd` proposal so the whole pipeline can be exercised without Claude.
3. **Claude backend.** With `AISHELL_AI_BACKEND=claude`, [`src/ai/claude_client.c`](src/ai/claude_client.c) does the following:
   - forks and runs `claude -p <prompt>`, with stdin set to `/dev/null` and stdout/stderr read through pipes
   - enforces a per-attempt deadline using `select()`, prints periodic progress messages, and kills the child on timeout
   - classifies failures (binary not found, not logged in, timeout, non-zero exit, empty output, truncated output)
   - retries only the transient failures, with capped exponential backoff
4. **MCP access.** If the embedded MCP server is running, AiShell writes a temporary MCP config file (created with `mkstemp`, mode `0600`, and deleted after the call). It passes the file with `--mcp-config … --strict-mcp-config --allowedTools mcp__aishell__*`, which pre-approves only AiShell's own tools.
5. **Parsing.** The reply must be a JSON object, optionally inside a Markdown code fence. [`src/ai/ai_json.c`](src/ai/ai_json.c) parses it strictly into `intent`, `command`, `explanation`, `risk` and `confidence`. If the model marks a request as not a shell task, AiShell prints that explanation and stops.
6. **Validation.** [`src/ai/ai_validate.c`](src/ai/ai_validate.c) classifies the proposed command as `OK`, `WARN` or `BLOCKED` (details below).
7. **Confirmation.**
   - `BLOCKED`: the proposal is rejected and never offered.
   - `WARN`: the reason is shown, and the user must type `yes` to run it.
   - `OK`: pressing Enter runs it, and any other input cancels.
8. **Execution.** An approved command goes through `execute_line()`, exactly like a typed command.

### Embedded MCP server

[`src/ai/mcp_server.c`](src/ai/mcp_server.c) runs on a background thread when `AISHELL_MCP_ENABLED=1` and a token is configured.

- **Binding.** It binds to `127.0.0.1` only, on a fixed port or an OS-assigned one (`AISHELL_MCP_PORT=0`).
- **Request checks.**
  - Only `POST /mcp` is accepted.
  - Every request must carry `Authorization: Bearer <token>`.
  - Requests whose `Origin` header names a non-local origin are rejected.
- **Connection handling.** Connections are handled one at a time.
- **Protocol coverage.** It implements the JSON-RPC methods AiShell needs: `initialize`, `notifications/initialized`, `tools/list` and `tools/call`. It is a minimal server, not a complete implementation of the MCP specification.

| Tool | Purpose |
|---|---|
| `get_shell_context` | Working directory, process and port information, project layout hints and common pitfalls |
| `list_aishell_builtins` | Names of all registered builtins |
| `get_command_help` | Synopsis and usage notes for one builtin, from a static table |
| `validate_command` | Runs the same local validator and returns its level, reason and suggestion |

None of these tools runs shell commands. The server has no command-execution path; it only reads the registry, a static help table and the filesystem (for path-existence checks).

### Command validator

The validator is a set of deny rules and heuristics, applied to the command text before variable and glob expansion:

- **Blocked:**
  - empty commands
  - command chaining and substitution (`;`, `&&`, `||`, backticks, `$( )`)
  - piping into a shell (for example `curl … | sh`)
  - `sudo`, `chmod`, `chown`, `mkfs`, `dd`, `shred`, `eval`
  - `rm` with both recursive and force flags, or targeting `/`
  - `git push --force`, `git reset --hard`, `git clean -f`
  - some patterns that cannot work in AiShell, such as `find -exec` with an AiShell-only builtin
- **Warned:**
  - `rm`, `mv`, `git push` and `>` redirection
  - commands the model itself labels risky or low-confidence
  - missing file arguments
  - common mistakes with AiShell's `rg`
  - GNU-only options that fail with macOS/BSD tools

**What the validator does not protect against:**
- It is a review aid, not a sandbox, and it does not make the feature safe against a malicious or prompt-injected model.
- It does not flag unknown commands, standalone network commands, or `>>` appends.
- The final safeguard is the user reading the proposed command.
- AiShell pre-approves only its own MCP tools. Whether Claude Code may use its other built-in tools during a request is governed by Claude Code's own permission settings, not by AiShell.

## Building and Running

### Requirements

- A C compiler (`cc`/`clang`/`gcc`) and `make`
- POSIX threads
- [argtable3](https://github.com/argtable/argtable3)
- Optional: the [Claude Code](https://github.com/anthropics/claude-code) CLI, installed and logged in, for the `claude` backend
- Optional: Graphviz, to render AST images

The project has been developed on macOS with Homebrew. The [`Makefile`](Makefile) adds `-I/opt/homebrew/include` and `-L/opt/homebrew/lib`.

```sh
brew install argtable3
make            # cleans, then builds ./aishell (objects go to build/)
./aishell
```

Linux builds have not been verified. If argtable3 is installed elsewhere, pass its paths on the command line without editing the Makefile. `LDFLAGS` replaces the default, so it must keep the libraries:

```sh
make CFLAGS_EXTRA="-I/path/to/include" LDFLAGS="-L/path/to/lib -largtable3 -pthread"
```

Other targets:
- `make demo`: trace build, described above.
- `make demo-server`: builds [`tools/tiny_http_server.c`](tools/tiny_http_server.c), a minimal local server for trying `http-get`.
- `make docs`: generates the `pkg` reference pages under `docs/`. These are git-ignored.

### Using the shell

```
aishell> ls -l src | head -n 5
aishell> NAME=world
aishell> echo hello $NAME > /tmp/greeting.txt
aishell> wc -c < /tmp/greeting.txt
aishell> sleep 5 &
aishell> jobs
aishell> ast on
aishell> echo hello | head -n 1
aishell> @ list all C source files under the src directory
```

```sh
./aishell ls -l /tmp                  # one-shot mode
./tools/tiny_http_server 8080 &       # after `make demo-server`
./aishell http-get 127.0.0.1 8080 /   # HTTP/1.0 request to it
```

Every builtin answers `-h`/`--help`. Most also accept `--json` for machine-readable output, and `-h --json` returns the help text as JSON.

### Configuring the AI mode

Settings come from the environment or from a `.env` file in the directory where AiShell is started. Environment variables take precedence, and only `AISHELL_*` keys are read. [`.env.example`](.env.example) documents all keys:

| Variable | Default | Meaning |
|---|---|---|
| `AISHELL_AI_BACKEND` | `mock` | `mock` or `claude` |
| `AISHELL_CLAUDE_PATH` | *(search `PATH`)* | Path to the `claude` binary |
| `AISHELL_MCP_ENABLED` | `0` | Start the embedded MCP server |
| `AISHELL_MCP_PORT` | `0` | `0` lets the OS pick a free port |
| `AISHELL_MCP_TOKEN` | *(empty)* | Local Bearer token; the server stays off without one |
| `AISHELL_AI_TIMEOUT_MS` | `30000` | Per-attempt timeout for a Claude call |
| `AISHELL_AI_MAX_RETRIES` | `6` | Retries for transient failures |
| `AISHELL_AI_RETRY_BACKOFF_MS` | `500` | Base backoff, doubled per retry and capped at 10 s |

The defaults above are those built into [`src/ai/ai_config.c`](src/ai/ai_config.c). `.env.example` contains sample override values, and some of them differ from these compiled defaults. For example, it suggests a longer timeout and fewer retries for MCP-assisted sessions, because Claude may make several tool calls before it answers.

To use Claude with the MCP server:

```sh
cp .env.example .env
# in .env: AISHELL_AI_BACKEND=claude, AISHELL_MCP_ENABLED=1,
#          AISHELL_MCP_TOKEN=<output of: openssl rand -hex 32>
./aishell
```

AiShell does not call the Anthropic API directly or handle Anthropic credentials. It relies on the local Claude Code installation and that installation's own login. The MCP token only authenticates requests to the local server. `./aishell --dump-config` prints the effective configuration and reports whether a token is set, without printing it.

## Testing

The [`tests/`](tests/) directory contains two suites, both runnable without a Claude account:

- [`tests/smoke_test.py`](tests/smoke_test.py) is a standard-library Python harness that drives the binary through subprocesses. Each check is recorded under an ID, and the run ends with a `N passed, M failed` summary. It covers:
  - the build
  - `-h`/`--json` help for every registered builtin (discovered from the source)
  - functional shell behaviour, including an end-to-end `pkg` cycle
  - multicall dispatch and docs generation
  - the AI JSON parser, configuration loading and the Claude client
  - the validator and the `@` confirmation flow
  - the MCP server over HTTP, including authentication, origin checks and the four tools
- [`tests/test_ast.sh`](tests/test_ast.sh) covers parsing, AST output, `ast.dot` generation and syntax errors. It must be run from the repository root after `make`.
- `tests/mock_claude_*.sh` are fixture scripts that impersonate the `claude` CLI. They are substituted via `AISHELL_CLAUDE_PATH` to simulate valid, malformed, slow, failing and not-logged-in responses.

```sh
python3 tests/smoke_test.py      # see side effects below
bash tests/test_ast.sh
```

**Smoke-test side effects:**
- It runs `make clean` and `make`, so it deletes and rebuilds `build/` and `./aishell`.
- Its package-manager test installs and then removes a package in your real `~/.mysh`.
- Mock fixtures write files under `/tmp`.

## Project Structure

```
AiShell/
├── src/
│   ├── main.c                 REPL, startup, multicall and one-shot dispatch
│   ├── token.c  parser.c      tokenizer and recursive-descent parser
│   ├── ast.c  ast_print.c     AST nodes, text and Graphviz output
│   ├── exec.c                 executor: dispatch, pipelines, redirection, job control
│   ├── registry.c  command.c  builtin registry and shared helpers
│   ├── alias.c  history.c     alias table and in-memory history
│   ├── builtins/              37 builtin source files
│   ├── net/socket_client.c    TCP helpers used by http-get
│   └── ai/                    config, Claude client, JSON parser, validator, MCP server
├── tests/                     smoke harness, AST tests, mock Claude fixtures
├── tools/tiny_http_server.c   local HTTP server for http-get demos
├── scripts/                   demo.sh, demo_ast.sh, demo_reset.sh
├── docs/
│   ├── parser_and_ast.md      parser, AST and job-control design notes
│   └── ai_mode_demo.md        example @ requests and proposed commands
├── grammar.ebnf               input grammar
├── Makefile
└── .env.example               AI-mode configuration template
```

The scripts in [`scripts/`](scripts/) can be run from any directory, because each changes to the repository root first.
- [`scripts/demo.sh`](scripts/demo.sh) walks through builtins, pipelines, redirection and background jobs.
- [`scripts/demo_ast.sh`](scripts/demo_ast.sh) shows the AST debug mode.
- [`scripts/demo_reset.sh`](scripts/demo_reset.sh) prepares the `pkg` demo. **It deletes `~/.mysh` and the repository's `tmp/`.**

## Limitations

- **Not a full POSIX shell.** There is no `;`, `&&`, `||`, command substitution, here-documents, subshells, functions or control flow, and no stderr redirection (`2>`). Assignments are recognized only as a standalone `NAME=VALUE` line. There is no line editing, tab completion or persistent history.
- **Exit status.** Pipeline exit status reflects only the last stage.
- **Threading.** Worker-thread builtins are joined immediately, so they never run concurrently with the shell.
- **Builtin gaps.**
  - `http-post` is a placeholder.
  - `http-get` has no TLS.
  - `ps` is minimal.
  - `rg -C` context output is not fully implemented.
  - `printf --json` is incomplete.
- **AI mode.**
  - Proposals are nondeterministic and can be wrong.
  - The validator is heuristic and incomplete.
  - The MCP server is minimal, handles one connection at a time, and its builtin help table is maintained by hand.
- **Platform.** Developed and exercised on macOS. Some validator hints assume BSD tool syntax.

## Further Reading

- [`docs/parser_and_ast.md`](docs/parser_and_ast.md): the grammar, AST node types, debug mode and the process-group design.
- [`docs/ai_mode_demo.md`](docs/ai_mode_demo.md): sample `@` requests with the commands they produced.
