#ifndef TOOL_EDIT_H
#define TOOL_EDIT_H

#include <stdbool.h>

typedef enum EditToolMode {
    EDIT_TOOL_NONE = 0,
    EDIT_TOOL_REPLACE,
    EDIT_TOOL_WRITE,
} EditToolMode;

typedef struct EditToolCall {
    char *path;
    char *old_text;
    char *new_text;
    char *content;
    EditToolMode mode;
} EditToolCall;

bool edit_extract_tool_call(const char *assistant_message, EditToolCall *call);
char *edit_apply_tool_call(const char *workspace, const EditToolCall *call);
char *edit_tool_preview(const EditToolCall *call);
void edit_tool_call_free(EditToolCall *call);

#endif
