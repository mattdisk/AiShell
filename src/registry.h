#ifndef AISHELL_REGISTRY_H
#define AISHELL_REGISTRY_H

typedef int (*cmd_run_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *description;
    cmd_run_fn  run;
} cmd_spec_t;

void        registry_add(const char *name, const char *description, cmd_run_fn run);
cmd_spec_t *find_command(const char *name);
void        registry_for_each(void (*fn)(const cmd_spec_t *spec));
void        registry_print_all(void);

#endif
