#ifndef AISHELL_AI_VALIDATE_H
#define AISHELL_AI_VALIDATE_H

#include <stddef.h>

typedef enum {
    AI_VALID_OK = 0,
    AI_VALID_WARN,
    AI_VALID_BLOCKED
} ai_valid_level_t;

typedef struct {
    ai_valid_level_t level;
    char reason[256];
    char suggestion[256];  /* optional — empty when no suggestion is available */
} ai_validation_t;

ai_valid_level_t ai_validate_command(const char *command,
                                     const char *claude_risk,
                                     double claude_confidence,
                                     ai_validation_t *out);

const char *ai_valid_level_name(ai_valid_level_t level);

#endif /* AISHELL_AI_VALIDATE_H */
