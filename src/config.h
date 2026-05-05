#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

bool config_load_for_workspace(const char *workspace, char *summary, size_t summary_size);
const char *config_model_override(void);
bool config_has_api_key(void);

#endif
