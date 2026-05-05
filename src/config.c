#include "config.h"

#include "common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct ConfigVar {
    const char *name;
    bool owned_by_config;
} ConfigVar;

static ConfigVar g_vars[] = {
    {"OPENROUTER_API_KEY", false},
    {"OPENROUTER_BASE_URL", false},
    {"OPENROUTER_MODEL", false},
};

static ConfigVar *find_var(const char *name) {
    for (size_t i = 0; i < sizeof(g_vars) / sizeof(g_vars[0]); i++) {
        if (strcmp(g_vars[i].name, name) == 0) return &g_vars[i];
    }
    return NULL;
}

static bool set_config_value(const char *name, const char *value) {
    ConfigVar *var = find_var(name);
    if (!var || !value || value[0] == '\0') return false;
    const char *existing = getenv(name);
    if (existing && existing[0] != '\0' && !var->owned_by_config) return false;
    if (setenv(name, value, 1) != 0) return false;
    var->owned_by_config = true;
    return true;
}

static char *unquote_value(char *value) {
    char *trimmed = str_trim_dup(value);
    if (!trimmed) return NULL;
    size_t len = strlen(trimmed);
    if (len >= 2 && ((trimmed[0] == '"' && trimmed[len - 1] == '"') || (trimmed[0] == '\'' && trimmed[len - 1] == '\''))) {
        trimmed[len - 1] = '\0';
        char *out = str_dup(trimmed + 1);
        free(trimmed);
        return out;
    }
    return trimmed;
}

static bool load_env_text(char *text, int *loaded_values) {
    bool loaded_file = false;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *trimmed = str_trim_dup(line);
        if (!trimmed) continue;
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            free(trimmed);
            continue;
        }
        char *p = trimmed;
        if (strncmp(p, "export ", 7) == 0) p += 7;
        char *eq = strchr(p, '=');
        if (!eq) {
            free(trimmed);
            continue;
        }
        *eq = '\0';
        char *key = str_trim_dup(p);
        char *value = unquote_value(eq + 1);
        if (key && value && set_config_value(key, value)) {
            loaded_file = true;
            if (loaded_values) (*loaded_values)++;
        }
        free(key);
        free(value);
        free(trimmed);
    }
    return loaded_file;
}

static bool load_env_file(const char *path, int *loaded_values) {
    char *text = read_text_file(path);
    if (!text) return false;
    bool loaded_file = load_env_text(text, loaded_values);
    free(text);
    return loaded_file;
}

static void set_json_alias(const char *json, const char *json_key, const char *env_key, bool *loaded_file, int *loaded_values) {
    char *value = json_extract_string_for_key(json, json_key);
    if (!value) return;
    if (set_config_value(env_key, value)) {
        *loaded_file = true;
        if (loaded_values) (*loaded_values)++;
    }
    free(value);
}

static bool load_json_file(const char *path, int *loaded_values) {
    char *json = read_text_file(path);
    if (!json) return false;
    bool loaded_file = false;
    set_json_alias(json, "OPENROUTER_API_KEY", "OPENROUTER_API_KEY", &loaded_file, loaded_values);
    set_json_alias(json, "apiKey", "OPENROUTER_API_KEY", &loaded_file, loaded_values);
    set_json_alias(json, "api_key", "OPENROUTER_API_KEY", &loaded_file, loaded_values);
    set_json_alias(json, "OPENROUTER_BASE_URL", "OPENROUTER_BASE_URL", &loaded_file, loaded_values);
    set_json_alias(json, "baseUrl", "OPENROUTER_BASE_URL", &loaded_file, loaded_values);
    set_json_alias(json, "base_url", "OPENROUTER_BASE_URL", &loaded_file, loaded_values);
    set_json_alias(json, "OPENROUTER_MODEL", "OPENROUTER_MODEL", &loaded_file, loaded_values);
    set_json_alias(json, "model", "OPENROUTER_MODEL", &loaded_file, loaded_values);
    if (!loaded_file) {
        char *env_copy = str_dup(json);
        if (env_copy) {
            loaded_file = load_env_text(env_copy, loaded_values);
            free(env_copy);
        }
    }
    free(json);
    return loaded_file;
}

static bool load_file(const char *path, int *loaded_values, int *seen_files) {
    if (!file_exists(path)) return false;
    if (seen_files) (*seen_files)++;
    const char *ext = strrchr(path, '.');
    if (ext && strcmp(ext, ".json") == 0) return load_json_file(path, loaded_values);
    return load_env_file(path, loaded_values);
}

static bool load_relative(const char *base, const char *rel, int *loaded_values, int *seen_files) {
    char path[1024];
    if (!path_join(path, sizeof(path), base, rel)) return false;
    return load_file(path, loaded_values, seen_files);
}

static void load_config_set(const char *base, int *loaded_values, int *seen_files, bool *loaded_any) {
    *loaded_any |= load_relative(base, ".env", loaded_values, seen_files);
    *loaded_any |= load_relative(base, "openrouter.env", loaded_values, seen_files);
    *loaded_any |= load_relative(base, "openrouter.json", loaded_values, seen_files);
    *loaded_any |= load_relative(base, "env/.env", loaded_values, seen_files);
    *loaded_any |= load_relative(base, "env/openrouter.env", loaded_values, seen_files);
    *loaded_any |= load_relative(base, "env/openrouter.json", loaded_values, seen_files);
    *loaded_any |= load_relative(base, "env/.env/openrouter.env", loaded_values, seen_files);
    *loaded_any |= load_relative(base, "env/.env/openrouter.json", loaded_values, seen_files);
    *loaded_any |= load_relative(base, ".agents/config.env", loaded_values, seen_files);
    *loaded_any |= load_relative(base, ".agents/config.json", loaded_values, seen_files);
    *loaded_any |= load_relative(base, ".agents/providers/openrouter.env", loaded_values, seen_files);
    *loaded_any |= load_relative(base, ".agents/providers/openrouter.json", loaded_values, seen_files);
}

static char *executable_dir(void) {
    char path[1024];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return NULL;
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) return NULL;
    *slash = '\0';
    return str_dup(path);
}

bool config_load_for_workspace(const char *workspace, char *summary, size_t summary_size) {
    int loaded_values = 0;
    int seen_files = 0;
    bool loaded_any = false;

    char *cwd = current_working_dir();
    char *exe_dir = executable_dir();
    bool workspace_is_cwd = false;
    if (cwd) {
        workspace_is_cwd = workspace && strcmp(cwd, workspace) == 0;
        load_config_set(cwd, &loaded_values, &seen_files, &loaded_any);
    }

    if (exe_dir && (!cwd || strcmp(exe_dir, cwd) != 0)) {
        load_config_set(exe_dir, &loaded_values, &seen_files, &loaded_any);
    }

    if (workspace && workspace[0] != '\0' && !workspace_is_cwd) {
        load_config_set(workspace, &loaded_values, &seen_files, &loaded_any);
    }

    if (summary && summary_size > 0) {
        if (loaded_any) snprintf(summary, summary_size, "Config loaded (%d value%s)", loaded_values, loaded_values == 1 ? "" : "s");
        else if (seen_files > 0) snprintf(summary, summary_size, "Config file found, but no supported keys were loaded");
        else snprintf(summary, summary_size, "No config file found");
    }
    free(cwd);
    free(exe_dir);
    return loaded_any;
}

const char *config_model_override(void) {
    const char *model = getenv("OPENROUTER_MODEL");
    return model && model[0] ? model : NULL;
}

bool config_has_api_key(void) {
    const char *key = getenv("OPENROUTER_API_KEY");
    return key && key[0] != '\0';
}
