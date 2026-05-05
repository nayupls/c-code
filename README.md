# C-Code

A small agentic coding harness written in C with a raylib GUI. It loads markdown-defined agents, talks to an OpenRouter-compatible chat completions provider, and gives the model a bash tool that runs in the currently opened workspace.

## Build

Requirements:

- C compiler
- raylib
- libcurl
- sqlite3
- pkg-config
- bash coreutils `timeout`

```sh
make
./c-code
```

For a stable local binary that normal rebuilds will not overwrite:

```sh
make prod
./dont_touch/c-code
```

`make clean` removes the normal build output but leaves `dont_touch/c-code` alone.

Check config loading without opening the GUI:

```sh
./c-code --check-config
./c-code --check-config /path/to/workspace
```

Set your provider key before launching:

```sh
export OPENROUTER_API_KEY=sk-or-v1-...
export OPENROUTER_MODEL=openai/gpt-4.1-mini
make run
```

Optional:

```sh
export OPENROUTER_BASE_URL=https://openrouter.ai/api/v1/chat/completions
```

You can also put provider config in one of these files in the launch directory, next to the `c-code` binary, or in the opened workspace:

```text
.env
openrouter.env
openrouter.json
env/.env
env/openrouter.env
env/openrouter.json
env/.env/openrouter.env
env/.env/openrouter.json
.agents/config.env
.agents/providers/openrouter.env
.agents/providers/openrouter.json
.agents/config.json
```

Environment variables supplied by the shell win over file values. File-loaded values can change when you open/reload a workspace. The loader is tolerant: env-style `KEY=value` config also works if you accidentally put it in a `.json` file.

`.env` / `openrouter.env`:

```env
OPENROUTER_API_KEY=sk-or-v1-...
OPENROUTER_BASE_URL=https://openrouter.ai/api/v1/chat/completions
OPENROUTER_MODEL=openai/gpt-4.1-mini
```

`openrouter.json`:

```json
{
  "apiKey": "sk-or-v1-...",
  "baseUrl": "https://openrouter.ai/api/v1/chat/completions",
  "model": "openai/gpt-4.1-mini"
}
```

## Agent Files

The harness loads agents from the opened workspace:

```text
.agents/agents/default.md
.agents/agents/review.md
```

If `AGENTS.md` exists at the opened workspace root, C-Code prepends it to the system prompt before the selected agent prompt.

Agent files are markdown with YAML-like frontmatter:

```md
---
name: review
provider: openrouter
model: anthropic/claude-3.5-sonnet
temperature: 0.1
---

# Prompt

You are a code review agent. Prioritize bugs and regressions.
```

This session has `.agents` mounted read-only by the runner, so the checked-in examples live under `templates/.agents/...`. In a normal checkout, copy them into `.agents` with:

```sh
make install-agents
```

## Bash Tool

The agent can request a command with a fenced block:

````md
```bash-tool
pwd && rg --files
```
````

The harness runs the command in the opened workspace with a 15 minute timeout, appends the output to the chat, and continues the agent loop. This is intentionally simple and powerful; run it only in workspaces where model-issued shell commands are acceptable.

Visible tool-call request cards are capped to one truncated line; the full command still runs and the output is still captured. Provider requests and bash tools both have 15 minute timeouts.

The chat pane supports mouse-wheel scrolling, a right-side draggable scrollbar, and `Home` / `End` for quick top/bottom jumps.

## Edit File Tool

Agents can patch files without shell commands using an exact replacement block:

````md
```edit-file
path: src/example.c
--- old
old exact text
--- new
new exact text
```
````

For creating or replacing a whole file:

````md
```edit-file
path: notes.txt
--- content
full file content
```
````

Paths must be relative and stay inside the opened workspace.

## Threads And Usage

C-Code stores chat threads in a local SQLite database at:

```text
$XDG_DATA_HOME/c-code/threads.sqlite
~/.local/share/c-code/threads.sqlite
```

Threads are keyed by workspace path. Opening a folder restores that project’s latest thread, and the `New` button creates a fresh thread for the current project.

The status bar shows cumulative token and dollar totals for the active thread when the provider response includes usage/cost fields.

When the estimated provider context reaches roughly 250k tokens, C-Code asks the model to summarize the current user request, task progress, relevant tool results, and next steps. The visible SQLite history is preserved, but future provider calls send that summary plus newer messages instead of the full older history. `AGENTS.md` and the selected agent prompt remain in the system prompt.

## Core Layout

- `src/main.c`: raylib UI, workspace controls, chat, worker thread.
- `src/agent.c`: markdown agent loader and prompt assembly.
- `src/provider_openrouter.c`: OpenRouter-compatible provider implementation.
- `src/store.c`: SQLite-backed per-project thread and usage storage.
- `src/tool_bash.c`: bash tool extraction and execution.
- `src/tool_edit.c`: edit-file extraction and workspace-safe file edits.
- `src/common.c`: small string, JSON, shell quoting, and file utilities.

The provider boundary is deliberately narrow: add another provider by implementing the same chat call shape used by `openrouter_chat`.

kimi was here
