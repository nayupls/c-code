# Agentic C

A small agentic coding harness written in C with a raylib GUI. It loads markdown-defined agents, talks to an OpenRouter-compatible chat completions provider, and gives the model a bash tool that runs in the currently opened workspace.

## Build

Requirements:

- C compiler
- raylib
- libcurl
- pkg-config
- bash coreutils `timeout`

```sh
make
./agentic-c
```

Check config loading without opening the GUI:

```sh
./agentic-c --check-config
./agentic-c --check-config /path/to/workspace
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

You can also put provider config in one of these files in the launch directory, next to the `agentic-c` binary, or in the opened workspace:

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

The harness runs the command in the opened workspace with a 45 second timeout, appends the output to the chat, and continues the agent loop. This is intentionally simple and powerful; run it only in workspaces where model-issued shell commands are acceptable.

## Core Layout

- `src/main.c`: raylib UI, workspace controls, chat, worker thread.
- `src/agent.c`: markdown agent loader and prompt assembly.
- `src/provider_openrouter.c`: OpenRouter-compatible provider implementation.
- `src/tool_bash.c`: bash tool extraction and execution.
- `src/common.c`: small string, JSON, shell quoting, and file utilities.

The provider boundary is deliberately narrow: add another provider by implementing the same chat call shape used by `openrouter_chat`.

kimi was here
