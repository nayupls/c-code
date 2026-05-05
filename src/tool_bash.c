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
    sb_appendf(&shell, "timeout %d bash -lc %s 2>&1", timeout_seconds > 0 ? timeout_seconds : 45, quoted_script);
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

char *bash_extract_tool_call(const char *assistant_message) {
    if (!assistant_message) return NULL;

    const char *start = strstr(assistant_message, "```bash-tool");
    if (start) {
        start = strchr(start, '\n');
        if (!start) return NULL;
        start++;
        const char *end = strstr(start, "\n```");
        if (!end) return NULL;
        char *raw = malloc((size_t)(end - start) + 1);
        if (!raw) return NULL;
        memcpy(raw, start, (size_t)(end - start));
        raw[end - start] = '\0';
        char *trimmed = str_trim_dup(raw);
        free(raw);
        return trimmed;
    }

    start = strstr(assistant_message, "<tool name=\"bash\">");
    if (start) {
        start += strlen("<tool name=\"bash\">");
        const char *end = strstr(start, "</tool>");
        if (!end) return NULL;
        char *raw = malloc((size_t)(end - start) + 1);
        if (!raw) return NULL;
        memcpy(raw, start, (size_t)(end - start));
        raw[end - start] = '\0';
        char *trimmed = str_trim_dup(raw);
        free(raw);
        return trimmed;
    }

    return NULL;
}
