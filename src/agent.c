#include "agent.h"

#include "common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_field(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

void agent_definition_init(AgentDefinition *def) {
    memset(def, 0, sizeof(*def));
    copy_field(def->name, sizeof(def->name), "default");
    copy_field(def->provider, sizeof(def->provider), "openrouter");
    copy_field(def->model, sizeof(def->model), "openai/gpt-4.1-mini");
    copy_field(def->temperature, sizeof(def->temperature), "0.2");
    def->system_prompt = str_dup("You are a pragmatic coding agent.");
}

void agent_definition_free(AgentDefinition *def) {
    free(def->system_prompt);
    def->system_prompt = NULL;
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void parse_frontmatter_line(AgentDefinition *def, char *line) {
    char *colon = strchr(line, ':');
    if (!colon) return;
    *colon = '\0';
    char *key = str_trim_dup(line);
    char *value = str_trim_dup(colon + 1);
    if (!key || !value) {
        free(key);
        free(value);
        return;
    }
    if (strcmp(key, "name") == 0) copy_field(def->name, sizeof(def->name), value);
    else if (strcmp(key, "provider") == 0) copy_field(def->provider, sizeof(def->provider), value);
    else if (strcmp(key, "model") == 0) copy_field(def->model, sizeof(def->model), value);
    else if (strcmp(key, "temperature") == 0) copy_field(def->temperature, sizeof(def->temperature), value);
    free(key);
    free(value);
}

static char *prompt_from_markdown(const char *text) {
    const char *prompt = strstr(text, "\n# Prompt");
    if (!prompt && starts_with(text, "# Prompt")) prompt = text;
    if (prompt) {
        const char *line = strchr(prompt, '\n');
        if (line) return str_trim_dup(line + 1);
    }

    const char *body = text;
    if (starts_with(text, "---\n")) {
        const char *end = strstr(text + 4, "\n---");
        if (end) {
            body = strchr(end + 1, '\n');
            if (body) body++;
        }
    }
    return str_trim_dup(body);
}

static bool load_from_path(AgentDefinition *def, const char *path, char *error, size_t error_size) {
    char *text = read_text_file(path);
    if (!text) {
        snprintf(error, error_size, "Could not read %s", path);
        return false;
    }

    copy_field(def->path, sizeof(def->path), path);
    if (starts_with(text, "---\n")) {
        char *front = text + 4;
        char *end = strstr(front, "\n---");
        if (end) {
            *end = '\0';
            char *save = NULL;
            for (char *line = strtok_r(front, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                parse_frontmatter_line(def, line);
            }
            *end = '\n';
        }
    }

    char *prompt = prompt_from_markdown(text);
    if (!prompt || prompt[0] == '\0') {
        free(prompt);
        free(text);
        snprintf(error, error_size, "Agent %s has no prompt body", path);
        return false;
    }
    free(def->system_prompt);
    def->system_prompt = prompt;
    free(text);
    return true;
}

bool agent_load(AgentDefinition *def, const char *workspace, const char *agent_name, char *error, size_t error_size) {
    if (!agent_name || agent_name[0] == '\0') agent_name = "default";
    char rel[AGENT_PATH];
    snprintf(rel, sizeof(rel), ".agents/agents/%s.md", agent_name);

    char path[AGENT_PATH];
    if (path_join(path, sizeof(path), workspace, rel) && file_exists(path)) {
        return load_from_path(def, path, error, error_size);
    }

    snprintf(rel, sizeof(rel), "templates/.agents/agents/%s.md", agent_name);
    if (path_join(path, sizeof(path), workspace, rel) && file_exists(path)) {
        return load_from_path(def, path, error, error_size);
    }
    if (file_exists(rel)) {
        return load_from_path(def, rel, error, error_size);
    }

    if (strcmp(agent_name, "default") != 0) {
        snprintf(rel, sizeof(rel), ".agents/agents/default.md");
        if (path_join(path, sizeof(path), workspace, rel) && file_exists(path)) {
            return load_from_path(def, path, error, error_size);
        }
        snprintf(rel, sizeof(rel), "templates/.agents/agents/default.md");
        if (path_join(path, sizeof(path), workspace, rel) && file_exists(path)) {
            return load_from_path(def, path, error, error_size);
        }
        if (file_exists(rel)) {
            return load_from_path(def, rel, error, error_size);
        }
    }

    snprintf(error, error_size, "No .agents/agents/%s.md found; using built-in defaults", agent_name);
    return true;
}

char *agent_build_system_prompt(const AgentDefinition *def, const char *workspace) {
    StringBuilder sb;
    sb_init(&sb);
    char agents_path[AGENT_PATH];
    if (workspace && path_join(agents_path, sizeof(agents_path), workspace, "AGENTS.md") && file_exists(agents_path)) {
        char *agents = read_text_file(agents_path);
        if (agents && agents[0] != '\0') {
            sb_append(&sb, "Repository instructions from AGENTS.md:\n\n");
            sb_append(&sb, agents);
            sb_append(&sb, "\n\n---\n\n");
        }
        free(agents);
    }
    sb_append(&sb, def->system_prompt);
    sb_append(&sb, "\n\n");
    sb_appendf(&sb, "Workspace: %s\n", workspace ? workspace : ".");
    sb_append(&sb, "Tool protocol: request at most one tool call per assistant turn.\n");
    sb_append(&sb, "For shell access, write one fenced block exactly like this, using three backticks:\n");
    sb_append(&sb, "```bash-tool\npwd && ls\n```\n");
    sb_append(&sb, "For file edits, prefer edit-file over shell commands. Use exact old/new replacement:\n");
    sb_append(&sb, "```edit-file\npath: relative/path.c\n--- old\nold exact text\n--- new\nnew exact text\n```\n");
    sb_append(&sb, "For creating or replacing a whole file, use:\n");
    sb_append(&sb, "```edit-file\npath: relative/path.txt\n--- content\nfull file content\n```\n");
    sb_append(&sb, "Do not emit JSON tool calls, XML tool calls, functions.Bash calls, or tool_calls_section markup. ");
    sb_append(&sb, "The harness will run the tool in the workspace, append the output, and continue. ");
    sb_append(&sb, "Prefer small, inspectable commands before edits. Do not use interactive commands.\n");
    return sb_steal(&sb);
}
