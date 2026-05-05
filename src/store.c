#include "store.h"

#include "common.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ThreadStore {
    sqlite3 *db;
};

static bool exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return false;
    }
    return true;
}

static char *store_dir(void) {
    const char *xdg = getenv("XDG_DATA_HOME");
    StringBuilder sb;
    sb_init(&sb);
    if (xdg && xdg[0]) {
        sb_appendf(&sb, "%s/c-code", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) home = ".";
        sb_appendf(&sb, "%s/.local/share/c-code", home);
    }
    return sb_steal(&sb);
}

static bool ensure_parent_dirs(const char *dir) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            ensure_dir(tmp);
            *p = '/';
        }
    }
    return ensure_dir(tmp);
}

ThreadStore *store_open(char *error, size_t error_size) {
    char *dir = store_dir();
    if (!ensure_parent_dirs(dir)) {
        snprintf(error, error_size, "Could not create sqlite store directory");
        free(dir);
        return NULL;
    }

    char path[1200];
    snprintf(path, sizeof(path), "%s/threads.sqlite", dir);
    free(dir);

    ThreadStore *store = calloc(1, sizeof(*store));
    if (!store) return NULL;
    if (sqlite3_open(path, &store->db) != SQLITE_OK) {
        snprintf(error, error_size, "Could not open sqlite store");
        store_close(store);
        return NULL;
    }

    exec_sql(store->db, "PRAGMA journal_mode=WAL;");
    exec_sql(store->db,
        "CREATE TABLE IF NOT EXISTS projects ("
        "id INTEGER PRIMARY KEY,"
        "path TEXT NOT NULL UNIQUE,"
        "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
        ");"
        "CREATE TABLE IF NOT EXISTS threads ("
        "id INTEGER PRIMARY KEY,"
        "project_id INTEGER NOT NULL,"
        "agent_name TEXT NOT NULL,"
        "title TEXT NOT NULL,"
        "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "updated_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "FOREIGN KEY(project_id) REFERENCES projects(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY,"
        "thread_id INTEGER NOT NULL,"
        "role TEXT NOT NULL,"
        "content TEXT NOT NULL,"
        "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "FOREIGN KEY(thread_id) REFERENCES threads(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS usage_events ("
        "id INTEGER PRIMARY KEY,"
        "thread_id INTEGER NOT NULL,"
        "prompt_tokens INTEGER NOT NULL DEFAULT 0,"
        "completion_tokens INTEGER NOT NULL DEFAULT 0,"
        "total_tokens INTEGER NOT NULL DEFAULT 0,"
        "cost_usd REAL NOT NULL DEFAULT 0,"
        "created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "FOREIGN KEY(thread_id) REFERENCES threads(id)"
        ");");
    return store;
}

void store_close(ThreadStore *store) {
    if (!store) return;
    if (store->db) sqlite3_close(store->db);
    free(store);
}

static long long project_id(ThreadStore *store, const char *workspace) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO projects(path) VALUES (?) "
                      "ON CONFLICT(path) DO UPDATE SET updated_at=unixepoch() "
                      "RETURNING id;";
    if (sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
    long long id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

long long store_new_thread(ThreadStore *store, const char *workspace, const char *agent_name, const char *title) {
    if (!store) return 0;
    long long pid = project_id(store, workspace);
    if (!pid) return 0;
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(store->db, "INSERT INTO threads(project_id, agent_name, title) VALUES (?, ?, ?);", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, pid);
    sqlite3_bind_text(stmt, 2, agent_name ? agent_name : "default", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, title ? title : "Thread", -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return sqlite3_last_insert_rowid(store->db);
}

long long store_open_latest_thread(ThreadStore *store, const char *workspace, const char *agent_name, char *title, size_t title_size) {
    if (!store) return 0;
    long long pid = project_id(store, workspace);
    if (!pid) return 0;

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(store->db, "SELECT id, title FROM threads WHERE project_id=? ORDER BY updated_at DESC, id DESC LIMIT 1;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, pid);
    long long id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
        const unsigned char *t = sqlite3_column_text(stmt, 1);
        if (title && title_size) snprintf(title, title_size, "%s", t ? (const char *)t : "Thread");
    }
    sqlite3_finalize(stmt);
    if (!id) {
        id = store_new_thread(store, workspace, agent_name, "Main thread");
        if (title && title_size) snprintf(title, title_size, "Main thread");
    }
    return id;
}

bool store_append_message(ThreadStore *store, long long thread_id, const char *role, const char *content) {
    if (!store || !thread_id) return false;
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(store->db, "INSERT INTO messages(thread_id, role, content) VALUES (?, ?, ?);", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, thread_id);
    sqlite3_bind_text(stmt, 2, role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(store->db, "UPDATE threads SET updated_at=unixepoch() WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, thread_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return ok;
}

bool store_add_usage(ThreadStore *store, long long thread_id, const ProviderResult *result) {
    if (!store || !thread_id || !result) return false;
    if (result->total_tokens <= 0 && result->cost_usd <= 0.0) return true;
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(store->db, "INSERT INTO usage_events(thread_id, prompt_tokens, completion_tokens, total_tokens, cost_usd) VALUES (?, ?, ?, ?, ?);", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, thread_id);
    sqlite3_bind_int64(stmt, 2, result->prompt_tokens);
    sqlite3_bind_int64(stmt, 3, result->completion_tokens);
    sqlite3_bind_int64(stmt, 4, result->total_tokens);
    sqlite3_bind_double(stmt, 5, result->cost_usd);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

ChatMessage *store_load_messages(ThreadStore *store, long long thread_id, size_t *count) {
    if (count) *count = 0;
    if (!store || !thread_id) return NULL;
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(store->db, "SELECT role, content FROM messages WHERE thread_id=? ORDER BY id ASC;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, thread_id);
    ChatMessage *items = NULL;
    size_t len = 0, cap = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (len == cap) {
            cap = cap ? cap * 2 : 32;
            ChatMessage *next = realloc(items, cap * sizeof(*items));
            if (!next) break;
            items = next;
        }
        const unsigned char *role = sqlite3_column_text(stmt, 0);
        const unsigned char *content = sqlite3_column_text(stmt, 1);
        snprintf(items[len].role, sizeof(items[len].role), "%s", role ? (const char *)role : "system");
        items[len].content = str_dup(content ? (const char *)content : "");
        len++;
    }
    sqlite3_finalize(stmt);
    if (count) *count = len;
    return items;
}

ThreadUsage store_thread_usage(ThreadStore *store, long long thread_id) {
    ThreadUsage usage = {0};
    if (!store || !thread_id) return usage;
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(store->db, "SELECT COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(completion_tokens),0), COALESCE(SUM(total_tokens),0), COALESCE(SUM(cost_usd),0) FROM usage_events WHERE thread_id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, thread_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        usage.prompt_tokens = sqlite3_column_int64(stmt, 0);
        usage.completion_tokens = sqlite3_column_int64(stmt, 1);
        usage.total_tokens = sqlite3_column_int64(stmt, 2);
        usage.cost_usd = sqlite3_column_double(stmt, 3);
    }
    sqlite3_finalize(stmt);
    return usage;
}

ThreadInfo *store_list_threads(ThreadStore *store, const char *workspace, size_t *count) {
    if (count) *count = 0;
    if (!store || !workspace) return NULL;
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(store->db,
        "SELECT t.id, t.title FROM threads t "
        "JOIN projects p ON t.project_id = p.id "
        "WHERE p.path = ? ORDER BY t.updated_at DESC;", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
    ThreadInfo *items = NULL;
    size_t len = 0, cap = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (len == cap) {
            cap = cap ? cap * 2 : 8;
            ThreadInfo *next = realloc(items, cap * sizeof(*items));
            if (!next) break;
            items = next;
        }
        items[len].id = sqlite3_column_int64(stmt, 0);
        const unsigned char *t = sqlite3_column_text(stmt, 1);
        snprintf(items[len].title, sizeof(items[len].title), "%s", t ? (const char *)t : "Thread");
        len++;
    }
    sqlite3_finalize(stmt);
    if (count) *count = len;
    return items;
}

void store_free_thread_list(ThreadInfo *items) {
    free(items);
}

bool store_rename_thread(ThreadStore *store, long long thread_id, const char *title) {
    if (!store || !thread_id || !title || !title[0]) return false;
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(store->db, "UPDATE threads SET title=?, updated_at=unixepoch() WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, thread_id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}
