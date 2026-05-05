#ifndef AGENT_H
#define AGENT_H

#include <stdbool.h>
#include <stddef.h>

#define AGENT_FIELD 128
#define AGENT_PATH 1024

typedef struct AgentDefinition {
    char name[AGENT_FIELD];
    char provider[AGENT_FIELD];
    char model[AGENT_FIELD];
    char temperature[32];
    char path[AGENT_PATH];
    char *system_prompt;
} AgentDefinition;

void agent_definition_init(AgentDefinition *def);
void agent_definition_free(AgentDefinition *def);
bool agent_load(AgentDefinition *def, const char *workspace, const char *agent_name, char *error, size_t error_size);
char *agent_build_system_prompt(const AgentDefinition *def, const char *workspace);

#endif
