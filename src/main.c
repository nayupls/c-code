#include "agent.h"
#include "config.h"
#include "common.h"
#include "provider_openrouter.h"
#include "tool_bash.h"

#include <pthread.h>
#include <raylib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INPUT 8192
#define MAX_STATUS 512
#define MAX_AGENT_NAME 64
#define TOOL_ITERATIONS 4

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
} WorkerArgs;

typedef struct AppState {
    pthread_mutex_t lock;
    ChatLog chat;
    AgentDefinition agent;
    char workspace[AGENT_PATH];
    char workspace_input[AGENT_PATH];
    char agent_name[MAX_AGENT_NAME];
    char input[MAX_INPUT];
    char status[MAX_STATUS];
    bool busy;
    bool focus_workspace;
    bool focus_agent;
    bool focus_chat;
    float scroll;
} AppState;

static AppState g_app;

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

static ChatMessage *chat_clone(const ChatLog *chat, size_t *out_count) {
    ChatMessage *copy = calloc(chat->count, sizeof(*copy));
    if (!copy) return NULL;
    for (size_t i = 0; i < chat->count; i++) {
        snprintf(copy[i].role, sizeof(copy[i].role), "%s", chat->items[i].role);
        copy[i].content = str_dup(chat->items[i].content);
        if (!copy[i].content) {
            for (size_t j = 0; j < i; j++) free(copy[j].content);
            free(copy);
            return NULL;
        }
    }
    *out_count = chat->count;
    return copy;
}

static void messages_free(ChatMessage *messages, size_t count) {
    for (size_t i = 0; i < count; i++) free(messages[i].content);
    free(messages);
}

static void set_status(const char *status) {
    pthread_mutex_lock(&g_app.lock);
    snprintf(g_app.status, sizeof(g_app.status), "%s", status);
    pthread_mutex_unlock(&g_app.lock);
}

static void append_locked(const char *role, const char *content) {
    pthread_mutex_lock(&g_app.lock);
    chat_append(&g_app.chat, role, content);
    pthread_mutex_unlock(&g_app.lock);
}

static void *agent_worker(void *userdata) {
    WorkerArgs *args = userdata;
    ChatLog local = {.items = args->messages, .count = args->count, .cap = args->count};
    char *system_prompt = agent_build_system_prompt(&args->agent, args->workspace);

    for (int i = 0; i < TOOL_ITERATIONS; i++) {
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

        append_locked("assistant", result.content);
        chat_append(&local, "assistant", result.content);

        char *command = bash_extract_tool_call(result.content);
        provider_result_free(&result);
        if (!command || command[0] == '\0') {
            free(command);
            break;
        }

        StringBuilder visible;
        sb_init(&visible);
        sb_appendf(&visible, "Running bash tool:\n%s", command);
        append_locked("tool", visible.data);
        sb_free(&visible);

        set_status("Running bash tool...");
        char *tool_output = bash_run_tool(args->workspace, command, 45);
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
    args->messages = chat_clone(&g_app.chat, &args->count);
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

static bool point_in(Rectangle r) {
    return CheckCollisionPointRec(GetMousePosition(), r);
}

static bool button(Rectangle r, const char *label, bool enabled) {
    Color bg = enabled ? (point_in(r) ? (Color){198, 214, 191, 255} : (Color){176, 199, 168, 255}) : (Color){120, 120, 120, 255};
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, (Color){36, 44, 37, 255});
    int w = MeasureText(label, 18);
    DrawText(label, (int)(r.x + (r.width - w) / 2), (int)(r.y + 9), 18, (Color){23, 31, 24, 255});
    return enabled && IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && point_in(r);
}

static void text_box(Rectangle r, char *buf, size_t cap, bool *focused, const char *placeholder) {
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) *focused = point_in(r);
    DrawRectangleRec(r, *focused ? (Color){252, 249, 232, 255} : (Color){230, 226, 207, 255});
    DrawRectangleLinesEx(r, 1, *focused ? (Color){91, 112, 82, 255} : (Color){80, 79, 70, 255});
    const char *shown = buf[0] ? buf : placeholder;
    DrawText(shown, (int)r.x + 8, (int)r.y + 9, 18, buf[0] ? (Color){31, 34, 28, 255} : (Color){111, 109, 94, 255});

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
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) g_app.focus_chat = point_in(r);
    DrawRectangleRec(r, (Color){252, 249, 232, 255});
    DrawRectangleLinesEx(r, 1, g_app.focus_chat ? (Color){91, 112, 82, 255} : (Color){80, 79, 70, 255});
    DrawText(g_app.input[0] ? g_app.input : "Message the agent. Enter sends, Shift+Enter inserts newline.", (int)r.x + 8, (int)r.y + 8, 18, g_app.input[0] ? (Color){31, 34, 28, 255} : (Color){111, 109, 94, 255});
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
            DrawTextEx(font, line, (Vector2){x, y}, (float)font_size, 1, color);
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
        if (MeasureTextEx(font, candidate, (float)font_size, 1).x > rect.width && line[0]) {
            DrawTextEx(font, line, (Vector2){x, y}, (float)font_size, 1, color);
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
        DrawTextEx(font, line, (Vector2){x, y}, (float)font_size, 1, color);
        y += line_h;
    }
    return (int)(y - rect.y);
}

static void draw_chat(Rectangle area) {
    BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);
    DrawRectangleRec(area, (Color){239, 234, 211, 255});
    Font font = GetFontDefault();
    float y = area.y + 12 + g_app.scroll;

    pthread_mutex_lock(&g_app.lock);
    for (size_t i = 0; i < g_app.chat.count; i++) {
        ChatMessage *m = &g_app.chat.items[i];
        Color accent = (Color){64, 83, 65, 255};
        if (strcmp(m->role, "assistant") == 0) accent = (Color){51, 78, 115, 255};
        else if (strcmp(m->role, "tool") == 0) accent = (Color){123, 83, 41, 255};
        else if (strcmp(m->role, "system") == 0) accent = (Color){126, 55, 55, 255};

        Rectangle card = {area.x + 14, y, area.width - 28, 40};
        DrawRectangleRounded(card, 0.08f, 8, (Color){252, 249, 232, 255});
        DrawRectangleRec((Rectangle){card.x, card.y, 5, card.height}, accent);
        DrawText(m->role, (int)card.x + 14, (int)card.y + 10, 18, accent);
        int h = draw_wrapped(font, m->content, (Rectangle){card.x + 14, card.y + 36, card.width - 28, area.height}, 18, (Color){31, 34, 28, 255});
        card.height = (float)h + 52;
        DrawRectangleLinesEx(card, 1, (Color){205, 199, 176, 255});
        y += card.height + 12;
    }
    pthread_mutex_unlock(&g_app.lock);

    EndScissorMode();
}

static void open_workspace(void) {
    pthread_mutex_lock(&g_app.lock);
    if (dir_exists(g_app.workspace_input)) {
        snprintf(g_app.workspace, sizeof(g_app.workspace), "%s", g_app.workspace_input);
        chat_free(&g_app.chat);
        chat_append(&g_app.chat, "system", "Workspace opened. Agent definitions are loaded from .agents/agents/<name>.md when available.");
        snprintf(g_app.status, sizeof(g_app.status), "Workspace opened");
        pthread_mutex_unlock(&g_app.lock);
        load_agent_into_app();
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
    InitWindow(1180, 820, "Agentic C");
    SetTargetFPS(60);

    pthread_mutex_init(&g_app.lock, NULL);
    agent_definition_init(&g_app.agent);
    char *cwd = current_working_dir();
    snprintf(g_app.workspace, sizeof(g_app.workspace), "%s", cwd ? cwd : ".");
    snprintf(g_app.workspace_input, sizeof(g_app.workspace_input), "%s", g_app.workspace);
    snprintf(g_app.agent_name, sizeof(g_app.agent_name), "default");
    snprintf(g_app.status, sizeof(g_app.status), "Ready");
    free(cwd);
    load_agent_into_app();
    chat_append(&g_app.chat, "system", "Open a folder, configure OPENROUTER_API_KEY in env/.env/openrouter.json, and chat. OPENROUTER_MODEL can be configured in the same place. Bash tool calls use ```bash-tool fenced blocks and run in the opened workspace.");

    while (!WindowShouldClose()) {
        int w = GetScreenWidth();
        int h = GetScreenHeight();
        if (GetMouseWheelMove() != 0) g_app.scroll += GetMouseWheelMove() * 32.0f;
        if (g_app.scroll > 0) g_app.scroll = 0;

        BeginDrawing();
        ClearBackground((Color){33, 43, 35, 255});
        DrawRectangleGradientV(0, 0, w, h, (Color){38, 51, 41, 255}, (Color){103, 93, 62, 255});
        DrawText("Agentic C", 24, 22, 34, (Color){252, 249, 232, 255});
        DrawText("raylib GUI + markdown agents + OpenRouter + bash tool", 25, 60, 18, (Color){219, 214, 190, 255});

        Rectangle folder = {24, 96, (float)w - 340, 40};
        Rectangle open = {(float)w - 306, 96, 86, 40};
        Rectangle agent = {(float)w - 208, 96, 92, 40};
        Rectangle reload = {(float)w - 104, 96, 80, 40};
        text_box(folder, g_app.workspace_input, sizeof(g_app.workspace_input), &g_app.focus_workspace, "Folder path");
        text_box(agent, g_app.agent_name, sizeof(g_app.agent_name), &g_app.focus_agent, "agent");
        if (button(open, "Open", !g_app.busy)) open_workspace();
        if (button(reload, "Reload", !g_app.busy)) load_agent_into_app();

        Rectangle chat_area = {24, 154, (float)w - 48, (float)h - 278};
        draw_chat(chat_area);

        Rectangle input = {24, (float)h - 104, (float)w - 146, 56};
        Rectangle send = {(float)w - 110, (float)h - 104, 86, 56};
        chat_input(input);
        if (button(send, g_app.busy ? "Busy" : "Send", !g_app.busy)) start_send();

        pthread_mutex_lock(&g_app.lock);
        char status_msg[180];
        char agent_name[80];
        char model_name[120];
        strncpy(status_msg, g_app.status, sizeof(status_msg) - 1);
        status_msg[sizeof(status_msg) - 1] = '\0';
        strncpy(agent_name, g_app.agent.name, sizeof(agent_name) - 1);
        agent_name[sizeof(agent_name) - 1] = '\0';
        strncpy(model_name, g_app.agent.model, sizeof(model_name) - 1);
        model_name[sizeof(model_name) - 1] = '\0';
        pthread_mutex_unlock(&g_app.lock);
        char status[MAX_STATUS];
        snprintf(status, sizeof(status), "%s | Agent: %s | Model: %s", status_msg, agent_name, model_name);
        DrawText(status, 24, h - 34, 18, (Color){252, 249, 232, 255});
        EndDrawing();
    }

    pthread_mutex_lock(&g_app.lock);
    chat_free(&g_app.chat);
    agent_definition_free(&g_app.agent);
    pthread_mutex_unlock(&g_app.lock);
    pthread_mutex_destroy(&g_app.lock);
    CloseWindow();
    return 0;
}
