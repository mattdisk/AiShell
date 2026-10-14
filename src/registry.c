#include "registry.h"
#include <stdio.h>
#include <string.h>

#define MAX_COMMANDS 128

static cmd_spec_t table[MAX_COMMANDS];
static int        table_count = 0;

void registry_add(const char *name, const char *description, cmd_run_fn run)
{
    if (table_count >= MAX_COMMANDS) return;
    table[table_count].name        = name;
    table[table_count].description = description;
    table[table_count].run         = run;
    table_count++;
}

cmd_spec_t *find_command(const char *name)
{
    for (int i = 0; i < table_count; i++) {
        if (strcmp(table[i].name, name) == 0) return &table[i];
    }
    return NULL;
}

void registry_for_each(void (*fn)(const cmd_spec_t *spec))
{
    for (int i = 0; i < table_count; i++) fn(&table[i]);
}

void registry_print_all(void)
{
    printf("Usage:\n");
    printf("  help\n");
    printf("  help COMMAND\n");
    printf("  COMMAND -h\n");
    printf("  COMMAND --help\n");
    printf("  quit | exit\n\n");
    printf("Commands:\n");
    for (int i = 0; i < table_count; i++) {
        printf("  %-22s %s\n", table[i].name, table[i].description);
    }
}
