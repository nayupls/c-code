#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void sb_init(StringBuilder *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(StringBuilder *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

bool sb_reserve(StringBuilder *sb, size_t needed) {
    if (needed <= sb->cap) return true;
    size_t next = sb->cap ? sb->cap * 2 : 1024;
    while (next < needed) next *= 2;
    char *data = realloc(sb->data, next);
    if (!data) return false;
    sb->data = data;
    sb->cap = next;
    return true;
}

bool sb_append_n(StringBuilder *sb, const char *text, size_t n) {
    if (!sb_reserve(sb, sb->len + n + 1)) return false;
    memcpy(sb->data + sb->len, text, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return true;
}

bool sb_append(StringBuilder *sb, const char *text) {
    return sb_append_n(sb, text ? text : "", strlen(text ? text : ""));
}

bool sb_appendf(StringBuilder *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return false;
    }
    if (!sb_reserve(sb, sb->len + (size_t)needed + 1)) {
        va_end(args);
        return false;
    }
    vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, args);
    sb->len += (size_t)needed;
    va_end(args);
    return true;
}

char *sb_steal(StringBuilder *sb) {
    char *out = sb->data;
    if (!out) out = str_dup("");
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return out;
}

char *read_text_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *data = calloc((size_t)size + 1, 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    size_t read = fread(data, 1, (size_t)size, fp);
    fclose(fp);
    data[read] = '\0';
    return data;
}

bool write_text_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t len = strlen(content ? content : "");
    bool ok = fwrite(content ? content : "", 1, len, fp) == len;
    fclose(fp);
    return ok;
}

bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool ensure_dir(const char *path) {
    if (dir_exists(path)) return true;
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

bool path_join(char *out, size_t out_size, const char *a, const char *b) {
    if (!out || out_size == 0 || !a || !b) return false;
    size_t len = strlen(a);
    const char *slash = (len > 0 && a[len - 1] == '/') ? "" : "/";
    int n = snprintf(out, out_size, "%s%s%s", a, slash, b);
    return n > 0 && (size_t)n < out_size;
}

char *str_dup(const char *src) {
    if (!src) src = "";
    size_t len = strlen(src);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, src, len + 1);
    return out;
}

char *str_trim_dup(const char *src) {
    if (!src) return str_dup("");
    const char *start = src;
    while (*start && isspace((unsigned char)*start)) start++;
    const char *end = src + strlen(src);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    char *out = malloc((size_t)(end - start) + 1);
    if (!out) return NULL;
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
    return out;
}

char *json_escape(const char *src) {
    StringBuilder sb;
    sb_init(&sb);
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        switch (*p) {
            case '\\': sb_append(&sb, "\\\\"); break;
            case '"': sb_append(&sb, "\\\""); break;
            case '\b': sb_append(&sb, "\\b"); break;
            case '\f': sb_append(&sb, "\\f"); break;
            case '\n': sb_append(&sb, "\\n"); break;
            case '\r': sb_append(&sb, "\\r"); break;
            case '\t': sb_append(&sb, "\\t"); break;
            default:
                if (*p < 0x20) sb_appendf(&sb, "\\u%04x", *p);
                else sb_append_n(&sb, (const char *)p, 1);
        }
    }
    return sb_steal(&sb);
}

char *shell_single_quote(const char *src) {
    StringBuilder sb;
    sb_init(&sb);
    sb_append(&sb, "'");
    for (const char *p = src ? src : ""; *p; p++) {
        if (*p == '\'') sb_append(&sb, "'\\''");
        else sb_append_n(&sb, p, 1);
    }
    sb_append(&sb, "'");
    return sb_steal(&sb);
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *parse_json_string(const char *p, char **out) {
    if (!p || *p != '"') return NULL;
    p++;
    StringBuilder sb;
    sb_init(&sb);
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"': sb_append(&sb, "\""); p++; break;
                case '\\': sb_append(&sb, "\\"); p++; break;
                case '/': sb_append(&sb, "/"); p++; break;
                case 'b': sb_append(&sb, "\b"); p++; break;
                case 'f': sb_append(&sb, "\f"); p++; break;
                case 'n': sb_append(&sb, "\n"); p++; break;
                case 'r': sb_append(&sb, "\r"); p++; break;
                case 't': sb_append(&sb, "\t"); p++; break;
                case 'u': {
                    int h0 = hex_digit(p[1]), h1 = hex_digit(p[2]);
                    int h2 = hex_digit(p[3]), h3 = hex_digit(p[4]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                        sb_free(&sb);
                        return NULL;
                    }
                    unsigned code = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    if (code < 0x80) {
                        char c = (char)code;
                        sb_append_n(&sb, &c, 1);
                    } else {
                        sb_append(&sb, "?");
                    }
                    p += 5;
                    break;
                }
                default:
                    if (*p) sb_append_n(&sb, p++, 1);
            }
        } else {
            sb_append_n(&sb, p++, 1);
        }
    }
    if (*p != '"') {
        sb_free(&sb);
        return NULL;
    }
    *out = sb_steal(&sb);
    return p + 1;
}

char *json_extract_string_for_key(const char *json, const char *key) {
    if (!json || !key) return NULL;
    StringBuilder needle;
    sb_init(&needle);
    sb_appendf(&needle, "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, needle.data)) != NULL) {
        p += needle.len;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != ':') continue;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        char *value = NULL;
        const char *after = parse_json_string(p, &value);
        if (after) {
            sb_free(&needle);
            return value;
        }
    }
    sb_free(&needle);
    return NULL;
}

static const char *json_value_after_key(const char *json, const char *key) {
    if (!json || !key) return NULL;
    StringBuilder needle;
    sb_init(&needle);
    sb_appendf(&needle, "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, needle.data)) != NULL) {
        p += needle.len;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != ':') continue;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        sb_free(&needle);
        return p;
    }
    sb_free(&needle);
    return NULL;
}

long long json_extract_int_for_key(const char *json, const char *key, long long fallback) {
    const char *p = json_value_after_key(json, key);
    if (!p) return fallback;
    char *end = NULL;
    long long value = strtoll(p, &end, 10);
    return end && end != p ? value : fallback;
}

double json_extract_double_for_key(const char *json, const char *key, double fallback) {
    const char *p = json_value_after_key(json, key);
    if (!p) return fallback;
    char *end = NULL;
    double value = strtod(p, &end);
    return end && end != p ? value : fallback;
}

char *current_working_dir(void) {
    size_t cap = 1024;
    for (;;) {
        char *buf = malloc(cap);
        if (!buf) return NULL;
        if (getcwd(buf, cap)) return buf;
        free(buf);
        if (errno != ERANGE) return NULL;
        cap *= 2;
    }
}
