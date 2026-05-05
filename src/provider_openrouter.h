#ifndef PROVIDER_OPENROUTER_H
#define PROVIDER_OPENROUTER_H

#include "agent.h"

#include <stddef.h>

typedef struct ChatMessage {
    char role[16];
    char *content;
} ChatMessage;

typedef struct ProviderResult {
    char *content;
    char *error;
} ProviderResult;

ProviderResult openrouter_chat(const AgentDefinition *agent, const char *system_prompt, const ChatMessage *messages, size_t count);
void provider_result_free(ProviderResult *result);

#endif
