#include "provider_openrouter.h"

#include "config.h"
#include "common.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CurlBuffer {
    char *data;
    size_t len;
} CurlBuffer;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t n = size * nmemb;
    CurlBuffer *buf = userdata;
    char *next = realloc(buf->data, buf->len + n + 1);
    if (!next) return 0;
    buf->data = next;
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

static char *build_body(const AgentDefinition *agent, const char *system_prompt, const ChatMessage *messages, size_t count) {
    const char *configured_model = config_model_override();
    char *model = json_escape(configured_model ? configured_model : agent->model);
    char *sys = json_escape(system_prompt);
    StringBuilder sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"model\":\"%s\",\"temperature\":%s,\"messages\":[", model, agent->temperature);
    sb_appendf(&sb, "{\"role\":\"system\",\"content\":\"%s\"}", sys);
    free(model);
    free(sys);
    for (size_t i = 0; i < count; i++) {
        char *content = json_escape(messages[i].content);
        char role[16];
        snprintf(role, sizeof(role), "%s", messages[i].role[0] ? messages[i].role : "user");
        sb_appendf(&sb, ",{\"role\":\"%s\",\"content\":\"%s\"}", role, content);
        free(content);
    }
    sb_append(&sb, "]}");
    return sb_steal(&sb);
}

ProviderResult openrouter_chat(const AgentDefinition *agent, const char *system_prompt, const ChatMessage *messages, size_t count) {
    ProviderResult result = {0};
    const char *api_key = getenv("OPENROUTER_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        result.error = str_dup("OPENROUTER_API_KEY is not set");
        result.content = str_dup("Set OPENROUTER_API_KEY in the environment, then send again. The provider is OpenRouter-compatible and posts to /api/v1/chat/completions.");
        return result;
    }

    const char *base_url = getenv("OPENROUTER_BASE_URL");
    if (!base_url || base_url[0] == '\0') base_url = "https://openrouter.ai/api/v1/chat/completions";

    CURL *curl = curl_easy_init();
    if (!curl) {
        result.error = str_dup("curl_easy_init failed");
        return result;
    }

    char *body = build_body(agent, system_prompt, messages, count);
    CurlBuffer response = {0};
    struct curl_slist *headers = NULL;
    StringBuilder auth;
    sb_init(&auth);
    sb_appendf(&auth, "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.data);
    headers = curl_slist_append(headers, "HTTP-Referer: http://localhost/agentic-c");
    headers = curl_slist_append(headers, "X-Title: Agentic C");

    curl_easy_setopt(curl, CURLOPT_URL, base_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "agentic-c/0.1");

    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (code != CURLE_OK) {
        result.error = str_dup(curl_easy_strerror(code));
    } else if (status < 200 || status >= 300) {
        char *message = json_extract_string_for_key(response.data, "message");
        StringBuilder err;
        sb_init(&err);
        sb_appendf(&err, "OpenRouter HTTP %ld", status);
        if (message) sb_appendf(&err, ": %s", message);
        result.error = sb_steal(&err);
        free(message);
    } else {
        result.content = json_extract_string_for_key(response.data, "content");
        if (!result.content) result.error = str_dup("Could not parse assistant content from provider response");
    }

    if (!result.content && response.data) {
        char *message = json_extract_string_for_key(response.data, "message");
        if (message) {
            result.content = message;
        }
    }

    free(response.data);
    free(body);
    sb_free(&auth);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

void provider_result_free(ProviderResult *result) {
    free(result->content);
    free(result->error);
    result->content = NULL;
    result->error = NULL;
}
