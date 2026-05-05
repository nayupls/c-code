#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stddef.h>

typedef struct StringBuilder {
    char *data;
    size_t len;
    size_t cap;
} StringBuilder;

void sb_init(StringBuilder *sb);
void sb_free(StringBuilder *sb);
bool sb_reserve(StringBuilder *sb, size_t needed);
bool sb_append(StringBuilder *sb, const char *text);
bool sb_append_n(StringBuilder *sb, const char *text, size_t n);
bool sb_appendf(StringBuilder *sb, const char *fmt, ...);
char *sb_steal(StringBuilder *sb);

char *read_text_file(const char *path);
bool write_text_file(const char *path, const char *content);
bool file_exists(const char *path);
bool dir_exists(const char *path);
bool path_join(char *out, size_t out_size, const char *a, const char *b);
char *str_dup(const char *src);
char *str_trim_dup(const char *src);
char *json_escape(const char *src);
char *shell_single_quote(const char *src);
char *json_extract_string_for_key(const char *json, const char *key);
char *current_working_dir(void);

#endif
