#define _POSIX_C_SOURCE 200809L

#include "history.h"

#include <stdlib.h>
#include <string.h>

#define MAX_HISTORY 256

static char *history_items[MAX_HISTORY];
static int   history_count = 0;

void add_history(const char *line)
{
    if (line == NULL || line[0] == '\0') return;
    if (history_count == MAX_HISTORY) {
        free(history_items[0]);
        memmove(&history_items[0], &history_items[1],
                sizeof(char *) * (MAX_HISTORY - 1));
        history_count--;
    }
    history_items[history_count++] = strdup(line);
}

const char *history_item(int i)
{
    if (i < 0 || i >= history_count) return NULL;
    return history_items[i];
}

int history_size(void)
{
    return history_count;
}

void history_free(void)
{
    for (int i = 0; i < history_count; i++) free(history_items[i]);
    history_count = 0;
}
