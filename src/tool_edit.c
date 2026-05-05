#include "tool_edit.h"

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static char *slice_dup(const char *start, const char *end) {
    if (!start || !end || end < start) return NULL;
    char *out = malloc((size_t)(end - start) + 1);
    if (!out) return NULL;
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
    return out;
}

static const char *line_after_marker(const char *body, const char *marker) {
    const char *p = strstr(body, marker);
    if (!p) return NULL;
    p += strlen(marker);
    if (*p == '\r') p++;
    if (*p == '\n') p++;
    return p;
}

static char *parse_path_line(const char *body) {
    const char *p = body;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "path:", 5) == 0) {
        p += 5;
        const char *end = strpbrk(p, "\r\n");
        if (!end) end = p + strlen(p);
        return slice_trim(p, end);
    }
    if (strncmp(p, "file:", 5) == 0) {
        p += 5;
        const char *end = strpbrk(p, "\r\n");
        if (!end) end = p + strlen(p);
        return slice_trim(p, end);
    }
    return NULL;
}

static bool parse_body(const char *body, EditToolCall *call) {
    char *path = parse_path_line(body);
    if (!path || path[0] == '\0') {
        free(path);
        return false;
    }

    const char *content_start = line_after_marker(body, "--- content");
    if (content_start) {
        call->path = path;
        call->content = str_dup(content_start);
        call->mode = EDIT_TOOL_WRITE;
        return call->content != NULL;
    }

    const char *old_start = line_after_marker(body, "--- old");
    const char *new_marker = strstr(body, "\n--- new");
    if (!old_start || !new_marker || new_marker < old_start) {
        free(path);
        return false;
    }
    const char *new_start = strchr(new_marker + 1, '\n');
    if (!new_start) {
        free(path);
        return false;
    }
    new_start++;

    call->path = path;
    call->old_text = slice_dup(old_start, new_marker);
    call->new_text = str_dup(new_start);
    call->mode = EDIT_TOOL_REPLACE;
    return call->old_text && call->new_text;
}

static bool extract_fenced(const char *message, EditToolCall *call) {
    const char *p = message;
    while ((p = strstr(p, "```")) != NULL) {
        const char *tick_end = p;
        while (*tick_end == '`') tick_end++;
        size_t tick_count = (size_t)(tick_end - p);
        const char *lang = tick_end;
        while (*lang == ' ' || *lang == '\t') lang++;
        if (strncmp(lang, "edit-file", 9) != 0 && strncmp(lang, "edit", 4) != 0) {
            p = tick_end;
            continue;
        }
        const char *body = strchr(lang, '\n');
        if (!body) return false;
        body++;
        StringBuilder closing;
        sb_init(&closing);
        sb_append(&closing, "\n");
        for (size_t i = 0; i < tick_count; i++) sb_append(&closing, "`");
        const char *end = strstr(body, closing.data);
        sb_free(&closing);
        if (!end) return false;
        char *raw = slice_dup(body, end);
        if (!raw) return false;
        bool ok = parse_body(raw, call);
        free(raw);
        return ok;
    }
    return false;
}

static bool extract_json_call(const char *message, EditToolCall *call) {
    const char *start = strstr(message, "functions.Edit");
    if (!start) start = strstr(message, "edit_file");
    if (!start) start = strstr(message, "edit-file");
    if (!start) return false;

    char *path = json_extract_string_for_key(start, "path");
    if (!path) path = json_extract_string_for_key(start, "file");
    char *old_text = json_extract_string_for_key(start, "old");
    if (!old_text) old_text = json_extract_string_for_key(start, "old_string");
    char *new_text = json_extract_string_for_key(start, "new");
    if (!new_text) new_text = json_extract_string_for_key(start, "new_string");
    char *content = json_extract_string_for_key(start, "content");

    if (!path || path[0] == '\0') {
        free(path);
        free(old_text);
        free(new_text);
        free(content);
        return false;
    }

    call->path = path;
    if (content) {
        call->content = content;
        call->mode = EDIT_TOOL_WRITE;
        free(old_text);
        free(new_text);
        return true;
    }
    if (old_text && new_text) {
        call->old_text = old_text;
        call->new_text = new_text;
        call->mode = EDIT_TOOL_REPLACE;
        return true;
    }

    free(old_text);
    free(new_text);
    return false;
}

bool edit_extract_tool_call(const char *assistant_message, EditToolCall *call) {
    if (!assistant_message || !call) return false;
    memset(call, 0, sizeof(*call));
    if (extract_fenced(assistant_message, call)) return true;
    edit_tool_call_free(call);
    memset(call, 0, sizeof(*call));
    if (extract_json_call(assistant_message, call)) return true;
    edit_tool_call_free(call);
    return false;
}

static bool safe_relative_path(const char *path) {
    if (!path || path[0] == '\0') return false;
    if (path[0] == '/' || path[0] == '~') return false;
    if (strstr(path, "../") || strstr(path, "/..") || strcmp(path, "..") == 0) return false;
    return true;
}

static bool ensure_file_parent_dirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    *slash = '\0';
    return ensure_dir(tmp);
}

static char *replace_once(const char *source, const char *old_text, const char *new_text, bool *replaced) {
    const char *hit = strstr(source, old_text);
    if (!hit) {
        *replaced = false;
        return NULL;
    }
    StringBuilder sb;
    sb_init(&sb);
    sb_append_n(&sb, source, (size_t)(hit - source));
    sb_append(&sb, new_text);
    sb_append(&sb, hit + strlen(old_text));
    *replaced = true;
    return sb_steal(&sb);
}

char *edit_apply_tool_call(const char *workspace, const EditToolCall *call) {
    if (!call || !call->path || call->mode == EDIT_TOOL_NONE) return str_dup("Edit failed: no edit-file tool call.");
    if (!safe_relative_path(call->path)) return str_dup("Edit failed: path must be relative and stay inside the workspace.");

    char full_path[1024];
    if (!path_join(full_path, sizeof(full_path), workspace ? workspace : ".", call->path)) {
        return str_dup("Edit failed: path is too long.");
    }

    if (call->mode == EDIT_TOOL_WRITE) {
        if (!ensure_file_parent_dirs(full_path)) return str_dup("Edit failed: could not create parent directories.");
        if (!write_text_file(full_path, call->content ? call->content : "")) return str_dup("Edit failed: could not write file.");
        StringBuilder out;
        sb_init(&out);
        sb_appendf(&out, "Wrote %s (%zu bytes).", call->path, strlen(call->content ? call->content : ""));
        return sb_steal(&out);
    }

    char *current = read_text_file(full_path);
    if (!current) return str_dup("Edit failed: could not read target file.");
    if (!call->old_text || call->old_text[0] == '\0') {
        free(current);
        return str_dup("Edit failed: replace mode requires a non-empty old block.");
    }
    bool replaced = false;
    char *next = replace_once(current, call->old_text, call->new_text ? call->new_text : "", &replaced);
    free(current);
    if (!replaced || !next) return str_dup("Edit failed: old block was not found exactly.");
    bool ok = write_text_file(full_path, next);
    size_t bytes = strlen(next);
    free(next);
    if (!ok) return str_dup("Edit failed: could not write replacement.");

    StringBuilder out;
    sb_init(&out);
    sb_appendf(&out, "Edited %s (%zu bytes after replacement).", call->path, bytes);
    return sb_steal(&out);
}

char *edit_tool_preview(const EditToolCall *call) {
    if (!call || !call->path) return str_dup("edit-file: (invalid)");
    StringBuilder sb;
    sb_init(&sb);
    sb_appendf(&sb, "edit-file: %s ", call->path);
    if (call->mode == EDIT_TOOL_WRITE) sb_append(&sb, "(write)");
    else if (call->mode == EDIT_TOOL_REPLACE) sb_append(&sb, "(replace)");
    else sb_append(&sb, "(unknown)");
    return sb_steal(&sb);
}

void edit_tool_call_free(EditToolCall *call) {
    if (!call) return;
    free(call->path);
    free(call->old_text);
    free(call->new_text);
    free(call->content);
    memset(call, 0, sizeof(*call));
}
