#define _POSIX_C_SOURCE 200809L

#include "alias.h"

#include <stdlib.h>
#include <string.h>

#define MAX_ALIASES 64

typedef struct {
    char *name;
    char *value;
} Alias;

static Alias aliases[MAX_ALIASES];
static int   alias_count = 0;

const char *lookup_alias(const char *name)
{
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) return aliases[i].value;
    }
    return NULL;
}

int alias_add(const char *name, const char *value)
{
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            free(aliases[i].value);
            aliases[i].value = strdup(value);
            return 0;
        }
    }
    if (alias_count >= MAX_ALIASES) return -1;
    aliases[alias_count].name  = strdup(name);
    aliases[alias_count].value = strdup(value);
    alias_count++;
    return 0;
}

int alias_remove(const char *name)
{
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            free(aliases[i].name);
            free(aliases[i].value);
            memmove(&aliases[i], &aliases[i + 1],
                    sizeof(Alias) * (size_t)(alias_count - i - 1));
            alias_count--;
            return 0;
        }
    }
    return -1;
}

void alias_each(void (*fn)(const char *name, const char *value, void *data), void *data)
{
    for (int i = 0; i < alias_count; i++) fn(aliases[i].name, aliases[i].value, data);
}

void alias_free(void)
{
    for (int i = 0; i < alias_count; i++) {
        free(aliases[i].name);
        free(aliases[i].value);
    }
    alias_count = 0;
}
