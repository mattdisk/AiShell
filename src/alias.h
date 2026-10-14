#ifndef AISHELL_ALIAS_H
#define AISHELL_ALIAS_H

const char *lookup_alias(const char *name);
int         alias_add(const char *name, const char *value);
int         alias_remove(const char *name);
void        alias_each(void (*fn)(const char *name, const char *value, void *data), void *data);
void        alias_free(void);

#endif
