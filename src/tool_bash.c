#include "tool_bash.h"

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

char *bash_run_tool(const char *workspace, const char *command, int timeout_seconds) {
    if (!command || command[0] == '\0') return str_dup("No command provided.");

    char *quoted_workspace = shell_single_quote(workspace ? workspace : ".");
    StringBuilder script;
    sb_init(&script);
    sb_appendf(&script, "cd %s && %s", quoted_workspace, command);
    free(quoted_workspace);

    char *quoted_script = shell_single_quote(script.data);
    StringBuilder shell;
    sb_init(&shell);
    sb_appendf(&shell, "timeout %d bash -lc %s 2>&1", timeout_seconds > 0 ? timeout_seconds : 900, quoted_script);
    free(quoted_script);
    sb_free(&script);

    FILE *fp = popen(shell.data, "r");
    sb_free(&shell);
    if (!fp) return str_dup("Failed to start bash tool.");

    StringBuilder out;
    sb_init(&out);
    char buf[4096];
    size_t cap = 256 * 1024;
    while (fgets(buf, sizeof(buf), fp)) {
        if (out.len + strlen(buf) < cap) sb_append(&out, buf);
    }
    int status = pclose(fp);
    if (out.len == 0) sb_append(&out, "(no output)\n");
    if (WIFEXITED(status)) {
        sb_appendf(&out, "\n[exit %d]\n", WEXITSTATUS(status));
    } else {
        sb_append(&out, "\n[process terminated]\n");
    }
    return sb_steal(&out);
}

static char *slice_trim(const char *start, const char *end) {
    if (!start || !end || end <= start) return NULL;
    char *raw = malloc((size_t)(end - start) + 1);
    if (!raw) return NULL;
    memcpy(raw, start, (size_t)(end - start));
    raw[end - start] = '\0';
    char *trimmed = str_trim_dup(raw);
    free(raw);
    return trimmed;
}

static char *extract_fenced(const char *message) {
    const char *p = message;
    while ((p = strstr(p, "```")) != NULL) {
        const char *tick_end = p;
        while (*tick_end == '`') tick_end++;
        size_t tick_count = (size_t)(tick_end - p);
        const char *lang = tick_end;
        while (*lang == ' ' || *lang == '\t') lang++;
        if (strncmp(lang, "bash-tool", 9) != 0 && strncmp(lang, "bash", 4) != 0) {
            p = tick_end;
            continue;
        }
        const char *line = strchr(lang, '\n');
        if (!line) return NULL;
        line++;
        StringBuilder closing;
        sb_init(&closing);
        sb_append(&closing, "\n");
        for (size_t i = 0; i < tick_count; i++) sb_append(&closing, "`");
        const char *end = strstr(line, closing.data);
        sb_free(&closing);
        if (!end) return NULL;
        return slice_trim(line, end);
    }
    return NULL;
}

static char *extract_xml_tool(const char *message) {
    const char *start = strstr(message, "<tool name=\"bash\">");
    if (!start) start = strstr(message, "<tool name='bash'>");
    if (start) {
        start = strchr(start, '>');
        if (!start) return NULL;
        start++;
        const char *end = strstr(start, "</tool>");
        if (!end) return NULL;
        return slice_trim(start, end);
    }
    return NULL;
}

static char *extract_json_command_after(const char *start) {
    const char *key = strstr(start, "\"command\"");
    if (!key) key = strstr(start, "'command'");
    if (!key) return NULL;
    const char *colon = strchr(key, ':');
    if (!colon) return NULL;
    const char *quote = colon + 1;
    while (*quote == ' ' || *quote == '\t') quote++;
    if (*quote != '"' && *quote != '\'') return NULL;
    char delimiter = *quote++;
    StringBuilder sb;
    sb_init(&sb);
    while (*quote) {
        if (*quote == '\\' && quote[1]) {
            quote++;
            switch (*quote) {
                case 'n': sb_append(&sb, "\n"); break;
                case 't': sb_append(&sb, "\t"); break;
                case 'r': sb_append(&sb, "\r"); break;
                default: sb_append_n(&sb, quote, 1); break;
            }
            quote++;
            continue;
        }
        if (*quote == delimiter) break;
        sb_append_n(&sb, quote, 1);
        quote++;
    }
    char *raw = sb_steal(&sb);
    char *trimmed = str_trim_dup(raw);
    free(raw);
    return trimmed;
}

static char *extract_serialized_tool_call(const char *message) {
    const char *start = strstr(message, "functions.Bash");
    if (!start) start = strstr(message, "Bash");
    if (!start) return NULL;
    return extract_json_command_after(start);
}

char *bash_extract_tool_call(const char *assistant_message) {
    if (!assistant_message) return NULL;
    char *command = extract_fenced(assistant_message);
    if (command && command[0]) return command;
    free(command);
    command = extract_xml_tool(assistant_message);
    if (command && command[0]) return command;
    free(command);
    command = extract_serialized_tool_call(assistant_message);
    if (command && command[0]) return command;
    free(command);
    return NULL;
}
