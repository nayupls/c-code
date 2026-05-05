#ifndef STORE_H
#define STORE_H

#include "provider_openrouter.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct ThreadStore ThreadStore;

typedef struct ThreadUsage {
    long long prompt_tokens;
    long long completion_tokens;
    long long total_tokens;
    double cost_usd;
} ThreadUsage;

typedef struct ThreadInfo {
    long long id;
    char title[128];
} ThreadInfo;

ThreadStore *store_open(char *error, size_t error_size);
void store_close(ThreadStore *store);
long long store_open_latest_thread(ThreadStore *store, const char *workspace, const char *agent_name, char *title, size_t title_size);
long long store_new_thread(ThreadStore *store, const char *workspace, const char *agent_name, const char *title);
bool store_append_message(ThreadStore *store, long long thread_id, const char *role, const char *content);
bool store_add_usage(ThreadStore *store, long long thread_id, const ProviderResult *result);
ChatMessage *store_load_messages(ThreadStore *store, long long thread_id, size_t *count);
ThreadUsage store_thread_usage(ThreadStore *store, long long thread_id);
ThreadInfo *store_list_threads(ThreadStore *store, const char *workspace, size_t *count);
void store_free_thread_list(ThreadInfo *items);
bool store_rename_thread(ThreadStore *store, long long thread_id, const char *title);

#endif
