#include "agent.h"
#include "config.h"
#include "common.h"
#include "provider_openrouter.h"
#include "store.h"
#include "tool_bash.h"
#include "tool_edit.h"

#include <pthread.h>
#include <raylib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INPUT 8192
#define MAX_STATUS 512
#define MAX_AGENT_NAME 64
#define TOOL_ITERATIONS 1000
#define TOOL_TIMEOUT_SECONDS 900
#define TOOL_PREVIEW_CHARS 180
#define CONTEXT_COMPACT_THRESHOLD_TOKENS 250000LL

typedef struct ChatLog {
    ChatMessage *items;
    size_t count;
    size_t cap;
} ChatLog;

typedef struct WorkerArgs {
    char workspace[AGENT_PATH];
    AgentDefinition agent;
    ChatMessage *messages;
    size_t count;
    long long thread_id;
} WorkerArgs;

typedef struct AppState {
    pthread_mutex_t lock;
    ChatLog chat;
    ThreadStore *store;
    long long thread_id;
    ThreadUsage usage;
    AgentDefinition agent;
    char workspace[AGENT_PATH];
    char workspace_input[AGENT_PATH];
    char agent_name[MAX_AGENT_NAME];
    char thread_title[128];
    char input[MAX_INPUT];
    char status[MAX_STATUS];
    bool busy;
    bool focus_workspace;
    bool focus_agent;
    bool focus_chat;
    bool focus_rename;
    bool dragging_scrollbar;
    bool dragging_sidebar;
    float scroll;
    float chat_content_height;
    ThreadInfo *threads;
    size_t thread_count;
    float sidebar_width;
    bool context_menu_open;
    long long context_thread_id;
    float context_menu_x;
    float context_menu_y;
    char rename_buf[128];
    bool renaming;
    char *context_summary;
    size_t context_cutoff_count;
} AppState;

static AppState g_app;
static Font g_ui_font;
static Font g_ui_font_bold;
static bool g_ui_font_loaded;
static bool g_ui_font_bold_loaded;

static Font ui_font(void) {
    return g_ui_font_loaded ? g_ui_font : GetFontDefault();
}

static Font ui_font_bold(void) {
    if (g_ui_font_bold_loaded) return g_ui_font_bold;
    return ui_font();
}

static Font load_first_font(const char **paths, int size, bool *loaded) {
    for (size_t i = 0; paths[i]; i++) {
        if (!file_exists(paths[i])) continue;
        Font font = LoadFontEx(paths[i], size, NULL, 0);
        if (font.texture.id != 0) {
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
            *loaded = true;
            return font;
        }
    }
    *loaded = false;
    return GetFontDefault();
}

static void load_ui_fonts(void) {
    const char *regular[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        NULL,
    };
    const char *bold[] = {
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
        NULL,
    };
    g_ui_font = load_first_font(regular, 36, &g_ui_font_loaded);
    g_ui_font_bold = load_first_font(bold, 36, &g_ui_font_bold_loaded);
}

static void unload_ui_fonts(void) {
    if (g_ui_font_loaded) UnloadFont(g_ui_font);
    if (g_ui_font_bold_loaded) UnloadFont(g_ui_font_bold);
}

static void ui_draw_text(Font font, const char *text, int x, int y, int size, Color color) {
    DrawTextEx(font, text, (Vector2){(float)x, (float)y}, (float)size, 0.6f, color);
}

static int ui_measure_text(Font font, const char *text, int size) {
    return (int)MeasureTextEx(font, text, (float)size, 0.6f).x;
}

static void chat_free(ChatLog *chat) {
    for (size_t i = 0; i < chat->count; i++) free(chat->items[i].content);
    free(chat->items);
    chat->items = NULL;
    chat->count = 0;
    chat->cap = 0;
}

static bool chat_append(ChatLog *chat, const char *role, const char *content) {
    if (chat->count == chat->cap) {
        size_t next = chat->cap ? chat->cap * 2 : 32;
        ChatMessage *items = realloc(chat->items, next * sizeof(*items));
        if (!items) return false;
        chat->items = items;
        chat->cap = next;
    }
    snprintf(chat->items[chat->count].role, sizeof(chat->items[chat->count].role), "%s", role);
    chat->items[chat->count].content = str_dup(content);
    if (!chat->items[chat->count].content) return false;
    chat->count++;
    return true;
}

static bool clone_one_message(ChatMessage *dst, const ChatMessage *src) {
    const char *role = src->role;
    const char *content = src->content;
    if (strcmp(role, "assistant") == 0 || strcmp(role, "user") == 0) {
        snprintf(dst->role, sizeof(dst->role), "%s", role);
        dst->content = str_dup(content);
    } else {
        snprintf(dst->role, sizeof(dst->role), "user");
        StringBuilder sb;
        sb_init(&sb);
        sb_appendf(&sb, "[%s]\n%s", role, content ? content : "");
        dst->content = sb_steal(&sb);
    }
    return dst->content != NULL;
}

static ChatMessage *chat_clone_context(const ChatLog *chat, const char *summary, size_t cutoff_count, size_t *out_count) {
    size_t summary_count = summary && summary[0] ? 1 : 0;
    size_t start = summary_count && cutoff_count < chat->count ? cutoff_count : 0;
    size_t count = summary_count + (chat->count - start);
    ChatMessage *copy = calloc(count ? count : 1, sizeof(*copy));
    if (!copy) return NULL;
    size_t out = 0;
    if (summary_count) {
        snprintf(copy[out].role, sizeof(copy[out].role), "user");
        StringBuilder sb;
        sb_init(&sb);
        sb_append(&sb, "Compacted prior conversation summary. Use this as the authoritative context for earlier history:\n\n");
        sb_append(&sb, summary);
        copy[out].content = sb_steal(&sb);
        if (!copy[out].content) {
            free(copy);
            return NULL;
        }
        out++;
    }
    for (size_t i = start; i < chat->count; i++) {
        if (!clone_one_message(&copy[out], &chat->items[i])) {
            for (size_t j = 0; j < out; j++) free(copy[j].content);
            free(copy);
            return NULL;
        }
        out++;
    }
    *out_count = out;
    return copy;
}

static void messages_free(ChatMessage *messages, size_t count) {
    for (size_t i = 0; i < count; i++) free(messages[i].content);
    free(messages);
}

static long long estimate_message_tokens(const ChatMessage *messages, size_t count) {
    size_t chars = 0;
    for (size_t i = 0; i < count; i++) {
        chars += strlen(messages[i].role);
        chars += messages[i].content ? strlen(messages[i].content) : 0;
        chars += 8;
    }
    return (long long)(chars / 4 + count * 4);
}

static void reset_context_compaction_locked(void) {
    free(g_app.context_summary);
    g_app.context_summary = NULL;
    g_app.context_cutoff_count = 0;
}

static void chat_replace(ChatLog *chat, ChatMessage *messages, size_t count) {
    chat_free(chat);
    chat->items = messages;
    chat->count = count;
    chat->cap = count;
}

static void chat_append_ephemeral_locked(const char *role, const char *content) {
    chat_append(&g_app.chat, role, content);
}

static void store_message_locked(const char *role, const char *content) {
    if (g_app.store && g_app.thread_id) store_append_message(g_app.store, g_app.thread_id, role, content);
}

static void load_thread_list(void) {
    pthread_mutex_lock(&g_app.lock);
    char workspace[AGENT_PATH];
    ThreadStore *store = g_app.store;
    snprintf(workspace, sizeof(workspace), "%s", g_app.workspace);
    pthread_mutex_unlock(&g_app.lock);

    if (g_app.threads) {
        store_free_thread_list(g_app.threads);
        g_app.threads = NULL;
    }
    g_app.thread_count = 0;
    if (!store) return;
    g_app.threads = store_list_threads(store, workspace, &g_app.thread_count);
}

static void switch_to_thread(long long thread_id) {
    pthread_mutex_lock(&g_app.lock);
    bool busy = g_app.busy;
    ThreadStore *store = g_app.store;
    pthread_mutex_unlock(&g_app.lock);
    if (busy || !store) return;

    size_t message_count = 0;
    ChatMessage *messages = store_load_messages(store, thread_id, &message_count);
    ThreadUsage usage = store_thread_usage(store, thread_id);

    char title[128] = "Thread";
    for (size_t i = 0; i < g_app.thread_count; i++) {
        if (g_app.threads[i].id == thread_id) {
            snprintf(title, sizeof(title), "%s", g_app.threads[i].title);
            break;
        }
    }

    pthread_mutex_lock(&g_app.lock);
    g_app.thread_id = thread_id;
    g_app.usage = usage;
    reset_context_compaction_locked();
    snprintf(g_app.thread_title, sizeof(g_app.thread_title), "%s", title);
    chat_replace(&g_app.chat, messages, message_count);
    if (g_app.chat.count == 0) {
        chat_append_ephemeral_locked("system", "Thread ready. Messages are stored per workspace in the local SQLite store.");
    }
    snprintf(g_app.status, sizeof(g_app.status), "Switched to thread");
    pthread_mutex_unlock(&g_app.lock);
}

static void set_status(const char *status) {
    pthread_mutex_lock(&g_app.lock);
    snprintf(g_app.status, sizeof(g_app.status), "%s", status);
    pthread_mutex_unlock(&g_app.lock);
}

static void append_locked(const char *role, const char *content) {
    pthread_mutex_lock(&g_app.lock);
    chat_append(&g_app.chat, role, content);
    store_message_locked(role, content);
    pthread_mutex_unlock(&g_app.lock);
}

static char *one_line_preview(const char *prefix, const char *text, size_t max_chars) {
    StringBuilder sb;
    sb_init(&sb);
    if (prefix) sb_append(&sb, prefix);
    size_t prefix_len = sb.len;
    const char *src = text ? text : "";
    bool last_space = false;
    while (*src && sb.len < max_chars) {
        char c = *src++;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ') {
            if (last_space) continue;
            last_space = true;
        } else {
            last_space = false;
        }
        sb_append_n(&sb, &c, 1);
    }
    if (*src) sb_append(&sb, "...");
    if (sb.len == prefix_len) sb_append(&sb, "(empty)");
    return sb_steal(&sb);
}

static bool replace_local_with_summary(ChatLog *local, const char *summary) {
    messages_free(local->items, local->count);
    local->items = calloc(1, sizeof(*local->items));
    if (!local->items) {
        local->count = 0;
        local->cap = 0;
        return false;
    }
    snprintf(local->items[0].role, sizeof(local->items[0].role), "user");
    StringBuilder sb;
    sb_init(&sb);
    sb_append(&sb, "Compacted prior conversation summary. Use this as the authoritative context for earlier history:\n\n");
    sb_append(&sb, summary);
    local->items[0].content = sb_steal(&sb);
    local->count = 1;
    local->cap = 1;
    return local->items[0].content != NULL;
}

static void compact_context_if_needed(WorkerArgs *args, ChatLog *local) {
    long long estimated = estimate_message_tokens(local->items, local->count);
    if (estimated < CONTEXT_COMPACT_THRESHOLD_TOKENS) return;

    set_status("Compacting context...");
    const char *summary_prompt =
        "You are maintaining context for an agentic coding harness. "
        "Summarize the conversation so future requests can continue without the old raw history. "
        "Include the user's original/current request, important constraints, repository/project facts, "
        "files inspected or edited, tool results that matter, current progress, unresolved problems, "
        "and the exact next steps. Be concise but complete.";

    ProviderResult summary = openrouter_chat(&args->agent, summary_prompt, local->items, local->count);
    if (summary.error) {
        StringBuilder err;
        sb_init(&err);
        sb_appendf(&err, "Context compaction failed: %s", summary.error);
        append_locked("system", err.data);
        sb_free(&err);
    }
    if (!summary.content || summary.content[0] == '\0') {
        provider_result_free(&summary);
        return;
    }

    pthread_mutex_lock(&g_app.lock);
    if (g_app.store && args->thread_id) {
        store_add_usage(g_app.store, args->thread_id, &summary);
        g_app.usage = store_thread_usage(g_app.store, args->thread_id);
    }
    free(g_app.context_summary);
    g_app.context_summary = str_dup(summary.content);
    g_app.context_cutoff_count = g_app.chat.count;
    pthread_mutex_unlock(&g_app.lock);

    replace_local_with_summary(local, summary.content);
    append_locked("system", "Context compacted. Older visible thread history is preserved but will no longer be sent to the provider.");
    provider_result_free(&summary);
}

static void *agent_worker(void *userdata) {
    WorkerArgs *args = userdata;
    ChatLog local = {.items = args->messages, .count = args->count, .cap = args->count};
    char *system_prompt = agent_build_system_prompt(&args->agent, args->workspace);

    for (int i = 0; i < TOOL_ITERATIONS; i++) {
        compact_context_if_needed(args, &local);
        set_status("Calling OpenRouter...");
        ProviderResult result = openrouter_chat(&args->agent, system_prompt, local.items, local.count);
        if (result.error) {
            StringBuilder err;
            sb_init(&err);
            sb_appendf(&err, "Provider error: %s", result.error);
            append_locked("system", err.data);
            sb_free(&err);
        }
        if (!result.content) {
            provider_result_free(&result);
            break;
        }

        pthread_mutex_lock(&g_app.lock);
        if (g_app.store && args->thread_id) {
            store_add_usage(g_app.store, args->thread_id, &result);
            g_app.usage = store_thread_usage(g_app.store, args->thread_id);
        }
        pthread_mutex_unlock(&g_app.lock);

        EditToolCall edit_call = {0};
        bool has_edit = edit_extract_tool_call(result.content, &edit_call);
        char *command = has_edit ? NULL : bash_extract_tool_call(result.content);
        if (has_edit) {
            char *edit_preview = edit_tool_preview(&edit_call);
            char *assistant_preview = one_line_preview("", edit_preview, TOOL_PREVIEW_CHARS);
            append_locked("assistant", assistant_preview);
            free(assistant_preview);
            free(edit_preview);
        } else if (command && command[0] != '\0') {
            char *assistant_preview = one_line_preview("bash-tool: ", command, TOOL_PREVIEW_CHARS);
            append_locked("assistant", assistant_preview);
            free(assistant_preview);
        } else {
            append_locked("assistant", result.content);
        }
        chat_append(&local, "assistant", result.content);

        provider_result_free(&result);
        if (!has_edit && (!command || command[0] == '\0')) {
            free(command);
            break;
        }

        if (has_edit) {
            char *visible_raw = edit_tool_preview(&edit_call);
            char *visible = one_line_preview("Running ", visible_raw, TOOL_PREVIEW_CHARS);
            append_locked("tool", visible);
            free(visible);
            free(visible_raw);

            set_status("Running edit-file tool...");
            char *edit_output = edit_apply_tool_call(args->workspace, &edit_call);
            edit_tool_call_free(&edit_call);

            StringBuilder tool_msg;
            sb_init(&tool_msg);
            sb_append(&tool_msg, "Edit output:\n");
            sb_append(&tool_msg, edit_output);
            append_locked("tool", tool_msg.data);
            chat_append(&local, "user", tool_msg.data);
            free(edit_output);
            sb_free(&tool_msg);
            continue;
        }

        char *visible = one_line_preview("Running bash tool: ", command, TOOL_PREVIEW_CHARS);
        append_locked("tool", visible);
        free(visible);

        set_status("Running bash tool...");
        char *tool_output = bash_run_tool(args->workspace, command, TOOL_TIMEOUT_SECONDS);
        free(command);

        StringBuilder tool_msg;
        sb_init(&tool_msg);
        sb_append(&tool_msg, "Bash output:\n");
        sb_append(&tool_msg, tool_output);
        append_locked("tool", tool_msg.data);
        chat_append(&local, "user", tool_msg.data);
        free(tool_output);
        sb_free(&tool_msg);
    }

    free(system_prompt);
    messages_free(local.items, local.count);
    agent_definition_free(&args->agent);
    free(args);

    pthread_mutex_lock(&g_app.lock);
    g_app.busy = false;
    snprintf(g_app.status, sizeof(g_app.status), "Ready");
    pthread_mutex_unlock(&g_app.lock);
    return NULL;
}

static void load_thread_into_app(bool create_new) {
    pthread_mutex_lock(&g_app.lock);
    char workspace[AGENT_PATH];
    char agent_name[MAX_AGENT_NAME];
    snprintf(workspace, sizeof(workspace), "%s", g_app.workspace);
    snprintf(agent_name, sizeof(agent_name), "%s", g_app.agent_name);
    ThreadStore *store = g_app.store;
    pthread_mutex_unlock(&g_app.lock);

    if (!store) return;

    char title[128] = {0};
    long long thread_id = create_new
        ? store_new_thread(store, workspace, agent_name, "New thread")
        : store_open_latest_thread(store, workspace, agent_name, title, sizeof(title));
    if (create_new) snprintf(title, sizeof(title), "New thread");

    size_t message_count = 0;
    ChatMessage *messages = store_load_messages(store, thread_id, &message_count);
    ThreadUsage usage = store_thread_usage(store, thread_id);

    pthread_mutex_lock(&g_app.lock);
    g_app.thread_id = thread_id;
    g_app.usage = usage;
    reset_context_compaction_locked();
    snprintf(g_app.thread_title, sizeof(g_app.thread_title), "%s", title[0] ? title : "Thread");
    chat_replace(&g_app.chat, messages, message_count);
    if (g_app.chat.count == 0) {
        chat_append_ephemeral_locked("system", "Thread ready. Messages are stored per workspace in the local SQLite store.");
    }
    snprintf(g_app.status, sizeof(g_app.status), "%s", create_new ? "New thread created" : "Thread loaded");
    pthread_mutex_unlock(&g_app.lock);
    load_thread_list();
}

static void load_agent_into_app(void) {
    pthread_mutex_lock(&g_app.lock);
    char workspace[AGENT_PATH];
    char agent_name[MAX_AGENT_NAME];
    snprintf(workspace, sizeof(workspace), "%s", g_app.workspace);
    snprintf(agent_name, sizeof(agent_name), "%s", g_app.agent_name);
    pthread_mutex_unlock(&g_app.lock);

    AgentDefinition next;
    agent_definition_init(&next);
    char error[MAX_STATUS] = {0};
    char config_summary[MAX_STATUS] = {0};
    config_load_for_workspace(workspace, config_summary, sizeof(config_summary));
    bool ok = agent_load(&next, workspace, agent_name, error, sizeof(error));
    const char *model_override = config_model_override();
    if (model_override) snprintf(next.model, sizeof(next.model), "%s", model_override);

    pthread_mutex_lock(&g_app.lock);
    agent_definition_free(&g_app.agent);
    g_app.agent = next;
    if (error[0] != '\0') snprintf(g_app.status, sizeof(g_app.status), "%s", error);
    else if (config_summary[0] != '\0') snprintf(g_app.status, sizeof(g_app.status), "%s; agent loaded", config_summary);
    else snprintf(g_app.status, sizeof(g_app.status), "%s", ok ? "Agent loaded" : "Agent load failed");
    pthread_mutex_unlock(&g_app.lock);
}

static void start_send(void) {
    pthread_mutex_lock(&g_app.lock);
    if (g_app.busy || g_app.input[0] == '\0') {
        pthread_mutex_unlock(&g_app.lock);
        return;
    }
    char *trimmed = str_trim_dup(g_app.input);
    if (!trimmed || trimmed[0] == '\0') {
        free(trimmed);
        pthread_mutex_unlock(&g_app.lock);
        return;
    }
    chat_append(&g_app.chat, "user", trimmed);
    g_app.input[0] = '\0';

    WorkerArgs *args = calloc(1, sizeof(*args));
    if (!args) {
        free(trimmed);
        pthread_mutex_unlock(&g_app.lock);
        return;
    }
    snprintf(args->workspace, sizeof(args->workspace), "%s", g_app.workspace);
    args->agent = g_app.agent;
    args->agent.system_prompt = str_dup(g_app.agent.system_prompt);
    args->messages = chat_clone_context(&g_app.chat, g_app.context_summary, g_app.context_cutoff_count, &args->count);
    args->thread_id = g_app.thread_id;
    store_message_locked("user", trimmed);
    g_app.busy = true;
    snprintf(g_app.status, sizeof(g_app.status), "Queued");
    free(trimmed);
    pthread_mutex_unlock(&g_app.lock);

    if (!args->messages) {
        free(args->agent.system_prompt);
        free(args);
        set_status("Could not clone chat messages");
        pthread_mutex_lock(&g_app.lock);
        g_app.busy = false;
        pthread_mutex_unlock(&g_app.lock);
        return;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, agent_worker, args) != 0) {
        messages_free(args->messages, args->count);
        agent_definition_free(&args->agent);
        free(args);
        set_status("Could not start worker thread");
        pthread_mutex_lock(&g_app.lock);
        g_app.busy = false;
        pthread_mutex_unlock(&g_app.lock);
        return;
    }
    pthread_detach(thread);
}

static void start_new_thread(void) {
    pthread_mutex_lock(&g_app.lock);
    bool busy = g_app.busy;
    pthread_mutex_unlock(&g_app.lock);
    if (!busy) load_thread_into_app(true);
}

static bool point_in(Rectangle r) {
    return CheckCollisionPointRec(GetMousePosition(), r);
}

static bool button(Rectangle r, const char *label, bool enabled) {
    Color bg = enabled ? (point_in(r) ? (Color){198, 214, 191, 255} : (Color){176, 199, 168, 255}) : (Color){120, 120, 120, 255};
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, (Color){36, 44, 37, 255});
    int w = ui_measure_text(ui_font_bold(), label, 18);
    ui_draw_text(ui_font_bold(), label, (int)(r.x + (r.width - w) / 2), (int)(r.y + 9), 18, (Color){23, 31, 24, 255});
    return enabled && IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && point_in(r);
}

static void text_box(Rectangle r, char *buf, size_t cap, bool *focused, const char *placeholder) {
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) *focused = point_in(r);
    DrawRectangleRec(r, *focused ? (Color){252, 249, 232, 255} : (Color){230, 226, 207, 255});
    DrawRectangleLinesEx(r, 1, *focused ? (Color){91, 112, 82, 255} : (Color){80, 79, 70, 255});
    const char *shown = buf[0] ? buf : placeholder;
    ui_draw_text(ui_font(), shown, (int)r.x + 8, (int)r.y + 9, 18, buf[0] ? (Color){31, 34, 28, 255} : (Color){111, 109, 94, 255});

    if (!*focused) return;
    int ch = GetCharPressed();
    while (ch > 0) {
        size_t len = strlen(buf);
        if (ch >= 32 && ch < 127 && len + 1 < cap) {
            buf[len] = (char)ch;
            buf[len + 1] = '\0';
        }
        ch = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE)) {
        size_t len = strlen(buf);
        if (len > 0) buf[len - 1] = '\0';
    }
}

static void chat_input(Rectangle r) {
    if (g_app.renaming) return;
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) g_app.focus_chat = point_in(r);
    DrawRectangleRec(r, (Color){252, 249, 232, 255});
    DrawRectangleLinesEx(r, 1, g_app.focus_chat ? (Color){91, 112, 82, 255} : (Color){80, 79, 70, 255});
    ui_draw_text(ui_font(), g_app.input[0] ? g_app.input : "Message the agent. Enter sends, Shift+Enter inserts newline.", (int)r.x + 8, (int)r.y + 8, 18, g_app.input[0] ? (Color){31, 34, 28, 255} : (Color){111, 109, 94, 255});
    if (!g_app.focus_chat) return;

    int ch = GetCharPressed();
    while (ch > 0) {
        size_t len = strlen(g_app.input);
        if (ch >= 32 && ch < 127 && len + 1 < sizeof(g_app.input)) {
            g_app.input[len] = (char)ch;
            g_app.input[len + 1] = '\0';
        }
        ch = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE)) {
        size_t len = strlen(g_app.input);
        if (len > 0) g_app.input[len - 1] = '\0';
    }
    if (IsKeyPressed(KEY_ENTER)) {
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
            size_t len = strlen(g_app.input);
            if (len + 1 < sizeof(g_app.input)) {
                g_app.input[len] = '\n';
                g_app.input[len + 1] = '\0';
            }
        } else {
            start_send();
        }
    }
}

static int draw_wrapped(Font font, const char *text, Rectangle rect, int font_size, Color color) {
    float x = rect.x;
    float y = rect.y;
    float line_h = (float)font_size + 6.0f;
    const char *p = text ? text : "";
    char word[512];
    char line[2048] = {0};
    while (*p) {
        if (*p == '\n') {
            DrawTextEx(font, line, (Vector2){x, y}, (float)font_size, 0.6f, color);
            y += line_h;
            line[0] = '\0';
            p++;
            continue;
        }
        size_t n = 0;
        while (p[n] && p[n] != ' ' && p[n] != '\n' && n + 1 < sizeof(word)) n++;
        memcpy(word, p, n);
        word[n] = '\0';
        char candidate[2600];
        snprintf(candidate, sizeof(candidate), "%s%s%s", line, line[0] ? " " : "", word);
        if (MeasureTextEx(font, candidate, (float)font_size, 0.6f).x > rect.width && line[0]) {
            DrawTextEx(font, line, (Vector2){x, y}, (float)font_size, 0.6f, color);
            y += line_h;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            strncpy(line, candidate, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
        }
        p += n;
        while (*p == ' ') p++;
    }
    if (line[0]) {
        DrawTextEx(font, line, (Vector2){x, y}, (float)font_size, 0.6f, color);
        y += line_h;
    }
    return (int)(y - rect.y);
}

static float min_scroll_for_area(Rectangle area) {
    if (g_app.chat_content_height <= area.height) return 0.0f;
    return area.height - g_app.chat_content_height;
}

static void clamp_chat_scroll(Rectangle area) {
    float min_scroll = min_scroll_for_area(area);
    if (g_app.scroll > 0.0f) g_app.scroll = 0.0f;
    if (g_app.scroll < min_scroll) g_app.scroll = min_scroll;
}

static Rectangle scrollbar_track(Rectangle area) {
    return (Rectangle){area.x + area.width - 12, area.y + 6, 8, area.height - 12};
}

static Rectangle scrollbar_thumb(Rectangle area) {
    Rectangle track = scrollbar_track(area);
    if (g_app.chat_content_height <= area.height) return (Rectangle){track.x, track.y, track.width, track.height};
    float visible_ratio = area.height / g_app.chat_content_height;
    float thumb_h = track.height * visible_ratio;
    if (thumb_h < 32.0f) thumb_h = 32.0f;
    float min_scroll = min_scroll_for_area(area);
    float t = min_scroll == 0.0f ? 0.0f : g_app.scroll / min_scroll;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return (Rectangle){track.x, track.y + (track.height - thumb_h) * t, track.width, thumb_h};
}

static void set_scroll_from_mouse(Rectangle area) {
    Rectangle track = scrollbar_track(area);
    Rectangle thumb = scrollbar_thumb(area);
    float movable = track.height - thumb.height;
    if (movable <= 0.0f) {
        g_app.scroll = 0.0f;
        return;
    }
    float y = GetMouseY() - track.y - thumb.height * 0.5f;
    float t = y / movable;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    g_app.scroll = min_scroll_for_area(area) * t;
}

static void handle_chat_scrollbar(Rectangle area) {
    if (IsKeyPressed(KEY_HOME)) g_app.scroll = 0.0f;
    if (IsKeyPressed(KEY_END)) g_app.scroll = min_scroll_for_area(area);

    Rectangle track = scrollbar_track(area);
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && point_in(track)) {
        g_app.dragging_scrollbar = true;
        set_scroll_from_mouse(area);
    }
    if (g_app.dragging_scrollbar) {
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) set_scroll_from_mouse(area);
        else g_app.dragging_scrollbar = false;
    }
    clamp_chat_scroll(area);
}

static void draw_chat_scrollbar(Rectangle area) {
    Rectangle track = scrollbar_track(area);
    DrawRectangleRounded(track, 0.8f, 8, (Color){214, 207, 181, 255});
    Rectangle thumb = scrollbar_thumb(area);
    Color thumb_color = g_app.dragging_scrollbar || point_in(thumb)
        ? (Color){96, 113, 86, 255}
        : (Color){126, 139, 115, 255};
    DrawRectangleRounded(thumb, 0.8f, 8, thumb_color);
}

static void draw_chat(Rectangle area) {
    BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);
    DrawRectangleRec(area, (Color){239, 234, 211, 255});
    Font font = ui_font();
    float y = area.y + 12 + g_app.scroll;
    float content_total = 12.0f;

    pthread_mutex_lock(&g_app.lock);
    for (size_t i = 0; i < g_app.chat.count; i++) {
        ChatMessage *m = &g_app.chat.items[i];
        Color accent = (Color){64, 83, 65, 255};
        if (strcmp(m->role, "assistant") == 0) accent = (Color){51, 78, 115, 255};
        else if (strcmp(m->role, "tool") == 0) accent = (Color){123, 83, 41, 255};
        else if (strcmp(m->role, "system") == 0) accent = (Color){126, 55, 55, 255};

        Rectangle card = {area.x + 14, y, area.width - 42, 40};
        DrawRectangleRounded(card, 0.08f, 8, (Color){252, 249, 232, 255});
        DrawRectangleRec((Rectangle){card.x, card.y, 5, card.height}, accent);
        ui_draw_text(ui_font_bold(), m->role, (int)card.x + 14, (int)card.y + 10, 18, accent);
        int h = draw_wrapped(font, m->content, (Rectangle){card.x + 14, card.y + 36, card.width - 28, area.height}, 18, (Color){31, 34, 28, 255});
        card.height = (float)h + 52;
        DrawRectangleLinesEx(card, 1, (Color){205, 199, 176, 255});
        y += card.height + 12;
        content_total += card.height + 12;
    }
    pthread_mutex_unlock(&g_app.lock);

    EndScissorMode();
    g_app.chat_content_height = content_total;
    clamp_chat_scroll(area);
    draw_chat_scrollbar(area);
}

static void close_context_menu(void) {
    g_app.context_menu_open = false;
    g_app.renaming = false;
    g_app.focus_rename = false;
    g_app.rename_buf[0] = '\0';
}

static void draw_context_menu(float w, float h) {
    if (!g_app.context_menu_open) return;
    float mx = g_app.context_menu_x;
    float my = g_app.context_menu_y;
    float mw = 140;
    float mh = 36;

    if (mx + mw > w) mx = w - mw;
    if (my + mh > h) my = h - mh;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;

    Rectangle menu_bg = {mx, my, mw, mh};
    DrawRectangleRec(menu_bg, (Color){45, 55, 47, 255});
    DrawRectangleLinesEx(menu_bg, 1, (Color){80, 90, 82, 255});

    Rectangle rename_btn = {mx + 4, my + 4, mw - 8, mh - 8};
    if (button(rename_btn, "Rename", !g_app.busy)) {
        for (size_t i = 0; i < g_app.thread_count; i++) {
            if (g_app.threads[i].id == g_app.context_thread_id) {
                snprintf(g_app.rename_buf, sizeof(g_app.rename_buf), "%s", g_app.threads[i].title);
                break;
            }
        }
        g_app.renaming = true;
        g_app.context_menu_open = false;
    }

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !point_in(menu_bg)) {
        close_context_menu();
    }
}

static void draw_rename_dialog(float w, float h) {
    if (!g_app.renaming) return;
    float dw = 460;
    float dh = 148;
    float dx = (w - dw) / 2.0f;
    float dy = (h - dh) / 2.0f;
    Rectangle dlg = {dx, dy, dw, dh};
    DrawRectangleRec(dlg, (Color){38, 51, 41, 255});
    DrawRectangleLinesEx(dlg, 1, (Color){91, 112, 82, 255});

    ui_draw_text(ui_font_bold(), "Rename Thread", (int)dx + 14, (int)dy + 10, 20, (Color){252, 249, 232, 255});

    Rectangle input = {dx + 18, dy + 50, dw - 36, 44};
    text_box(input, g_app.rename_buf, sizeof(g_app.rename_buf), &g_app.focus_rename, "Thread name");

    Rectangle ok = {dx + dw - 178, dy + dh - 46, 78, 36};
    Rectangle cancel = {dx + dw - 90, dy + dh - 46, 76, 36};
    if (button(ok, "OK", g_app.rename_buf[0] != '\0')) {
        pthread_mutex_lock(&g_app.lock);
        ThreadStore *store = g_app.store;
        pthread_mutex_unlock(&g_app.lock);
        if (store && g_app.context_thread_id && g_app.rename_buf[0]) {
            store_rename_thread(store, g_app.context_thread_id, g_app.rename_buf);
            if (g_app.thread_id == g_app.context_thread_id) {
                pthread_mutex_lock(&g_app.lock);
                snprintf(g_app.thread_title, sizeof(g_app.thread_title), "%s", g_app.rename_buf);
                pthread_mutex_unlock(&g_app.lock);
            }
            load_thread_list();
        }
        close_context_menu();
    }
    if (button(cancel, "Cancel", true)) {
        close_context_menu();
    }
    if (IsKeyPressed(KEY_ESCAPE)) close_context_menu();
}

static void draw_sidebar(float w, float h) {
    Rectangle sidebar = {0, 0, w, h};
    DrawRectangleRec(sidebar, (Color){28, 38, 30, 255});
    DrawRectangleRec((Rectangle){w - 1, 0, 1, h}, (Color){50, 60, 52, 255});

    ui_draw_text(ui_font_bold(), "Threads", 14, 14, 20, (Color){252, 249, 232, 255});

    Rectangle new_btn = {10, 44, w - 20, 34};
    if (button(new_btn, "+ New", !g_app.busy)) start_new_thread();

    float y = 92;
    for (size_t i = 0; i < g_app.thread_count; i++) {
        ThreadInfo *t = &g_app.threads[i];
        bool selected = t->id == g_app.thread_id;
        Rectangle row = {4, y, w - 8, 36};
        Color bg = selected ? (Color){64, 83, 65, 255} : (point_in(row) ? (Color){38, 51, 41, 255} : (Color){28, 38, 30, 255});
        DrawRectangleRec(row, bg);
        if (selected) DrawRectangleRec((Rectangle){row.x, row.y, 3, row.height}, (Color){176, 199, 168, 255});

        char title[128];
        snprintf(title, sizeof(title), "%s", t->title);
        int tw = ui_measure_text(ui_font(), title, 15);
        if (tw > w - 24) {
            size_t len = strlen(title);
            while (len > 3 && ui_measure_text(ui_font(), title, 15) > w - 32) {
                len--;
                title[len] = '\0';
            }
            if (len > 0 && len < strlen(t->title)) strcat(title, "...");
        }
        ui_draw_text(ui_font(), title, (int)row.x + 10, (int)row.y + 10, 15, selected ? (Color){252, 249, 232, 255} : (Color){200, 196, 176, 255});

        if (!g_app.busy && IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && point_in(row)) {
            switch_to_thread(t->id);
        }
        if (!g_app.busy && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && point_in(row)) {
            g_app.context_menu_open = true;
            g_app.context_thread_id = t->id;
            g_app.context_menu_x = GetMouseX();
            g_app.context_menu_y = GetMouseY();
        }
        y += 40;
    }

    Rectangle handle = {w - 6, 0, 6, h};
    DrawRectangleRec(handle, (Color){45, 55, 47, 255});
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && point_in(handle)) {
        g_app.dragging_sidebar = true;
    }
    if (g_app.dragging_sidebar) {
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            g_app.sidebar_width = GetMouseX();
            if (g_app.sidebar_width < 160) g_app.sidebar_width = 160;
            if (g_app.sidebar_width > 400) g_app.sidebar_width = 400;
        } else {
            g_app.dragging_sidebar = false;
        }
    }

    draw_context_menu(w, h);
}

static void open_workspace(void) {
    pthread_mutex_lock(&g_app.lock);
    if (dir_exists(g_app.workspace_input)) {
        snprintf(g_app.workspace, sizeof(g_app.workspace), "%s", g_app.workspace_input);
        snprintf(g_app.status, sizeof(g_app.status), "Workspace opened");
        pthread_mutex_unlock(&g_app.lock);
        load_agent_into_app();
        load_thread_into_app(false);
    } else {
        snprintf(g_app.status, sizeof(g_app.status), "Folder does not exist");
        pthread_mutex_unlock(&g_app.lock);
    }
}

static int check_config_cli(const char *workspace) {
    char summary[MAX_STATUS] = {0};
    config_load_for_workspace(workspace, summary, sizeof(summary));
    printf("%s\n", summary);
    printf("OPENROUTER_API_KEY: %s\n", config_has_api_key() ? "loaded" : "missing");
    printf("OPENROUTER_MODEL: %s\n", config_model_override() ? config_model_override() : "(not set)");
    const char *base_url = getenv("OPENROUTER_BASE_URL");
    printf("OPENROUTER_BASE_URL: %s\n", base_url && base_url[0] ? base_url : "(default)");
    return config_has_api_key() ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--check-config") == 0) {
        const char *workspace = argc >= 3 ? argv[2] : ".";
        return check_config_cli(workspace);
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1180, 820, "C-Code");
    load_ui_fonts();
    SetTargetFPS(60);

    pthread_mutex_init(&g_app.lock, NULL);
    char store_error[MAX_STATUS] = {0};
    g_app.store = store_open(store_error, sizeof(store_error));
    g_app.sidebar_width = 220;
    agent_definition_init(&g_app.agent);
    char *cwd = current_working_dir();
    snprintf(g_app.workspace, sizeof(g_app.workspace), "%s", cwd ? cwd : ".");
    snprintf(g_app.workspace_input, sizeof(g_app.workspace_input), "%s", g_app.workspace);
    snprintf(g_app.agent_name, sizeof(g_app.agent_name), "default");
    snprintf(g_app.status, sizeof(g_app.status), "Ready");
    free(cwd);
    load_agent_into_app();
    load_thread_into_app(false);
    if (!g_app.store && store_error[0]) {
        pthread_mutex_lock(&g_app.lock);
        chat_append_ephemeral_locked("system", store_error);
        pthread_mutex_unlock(&g_app.lock);
    }
    pthread_mutex_lock(&g_app.lock);
    chat_append_ephemeral_locked("system", "Open a folder, configure OPENROUTER_API_KEY in env/.env/openrouter.json, and chat. OPENROUTER_MODEL can be configured in the same place. Bash tool calls use ```bash-tool fenced blocks and run in the opened workspace.");
    pthread_mutex_unlock(&g_app.lock);

    while (!WindowShouldClose()) {
        int w = GetScreenWidth();
        int h = GetScreenHeight();
        float sx = g_app.sidebar_width;
        float mx = sx + 24;
        float mw = w - sx - 48;
        Rectangle chat_area = {mx, 154, mw, (float)h - 278};
        if (GetMouseWheelMove() != 0 && point_in(chat_area)) g_app.scroll += GetMouseWheelMove() * 48.0f;
        handle_chat_scrollbar(chat_area);

        BeginDrawing();
        ClearBackground((Color){33, 43, 35, 255});
        DrawRectangleGradientV(sx, 0, w - sx, h, (Color){38, 51, 41, 255}, (Color){103, 93, 62, 255});
        ui_draw_text(ui_font_bold(), "C-Code", (int)mx, 22, 34, (Color){252, 249, 232, 255});
        ui_draw_text(ui_font(), "raylib GUI + markdown agents + OpenRouter + bash tool", (int)mx + 1, 62, 18, (Color){219, 214, 190, 255});

        Rectangle folder = {mx, 96, mw - 430, 40};
        Rectangle open = {mx + mw - 396, 96, 78, 40};
        Rectangle new_thread = {mx + mw - 310, 96, 70, 40};
        Rectangle agent = {mx + mw - 232, 96, 100, 40};
        Rectangle reload = {mx + mw - 120, 96, 96, 40};
        text_box(folder, g_app.workspace_input, sizeof(g_app.workspace_input), &g_app.focus_workspace, "Folder path");
        text_box(agent, g_app.agent_name, sizeof(g_app.agent_name), &g_app.focus_agent, "agent");
        if (button(open, "Open", !g_app.busy)) open_workspace();
        if (button(new_thread, "New", !g_app.busy)) start_new_thread();
        if (button(reload, "Reload", !g_app.busy)) load_agent_into_app();

        draw_sidebar(sx, h);

        draw_chat(chat_area);

        Rectangle input = {mx, (float)h - 104, mw - 98, 56};
        Rectangle send = {mx + mw - 98, (float)h - 104, 86, 56};
        chat_input(input);
        if (button(send, g_app.busy ? "Busy" : "Send", !g_app.busy)) start_send();
        draw_rename_dialog((float)w, (float)h);

        pthread_mutex_lock(&g_app.lock);
        char status_msg[180];
        char agent_name[80];
        char model_name[120];
        char thread_title[80];
        ThreadUsage usage = g_app.usage;
        strncpy(status_msg, g_app.status, sizeof(status_msg) - 1);
        status_msg[sizeof(status_msg) - 1] = '\0';
        strncpy(agent_name, g_app.agent.name, sizeof(agent_name) - 1);
        agent_name[sizeof(agent_name) - 1] = '\0';
        strncpy(model_name, g_app.agent.model, sizeof(model_name) - 1);
        model_name[sizeof(model_name) - 1] = '\0';
        strncpy(thread_title, g_app.thread_title, sizeof(thread_title) - 1);
        thread_title[sizeof(thread_title) - 1] = '\0';
        pthread_mutex_unlock(&g_app.lock);
        char status[MAX_STATUS];
        snprintf(status, sizeof(status), "%s | Thread: %s | Agent: %s | Model: %s | Tokens: %lld | $%.5f",
                 status_msg, thread_title, agent_name, model_name, usage.total_tokens, usage.cost_usd);
        ui_draw_text(ui_font(), status, (int)mx, h - 34, 18, (Color){252, 249, 232, 255});
        EndDrawing();
    }

    pthread_mutex_lock(&g_app.lock);
    chat_free(&g_app.chat);
    agent_definition_free(&g_app.agent);
    reset_context_compaction_locked();
    store_close(g_app.store);
    if (g_app.threads) store_free_thread_list(g_app.threads);
    pthread_mutex_unlock(&g_app.lock);
    pthread_mutex_destroy(&g_app.lock);
    unload_ui_fonts();
    CloseWindow();
    return 0;
}
