#ifndef TOOL_BASH_H
#define TOOL_BASH_H

char *bash_run_tool(const char *workspace, const char *command, int timeout_seconds);
char *bash_extract_tool_call(const char *assistant_message);

#endif
