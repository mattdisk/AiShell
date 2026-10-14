#ifndef AISHELL_HISTORY_H
#define AISHELL_HISTORY_H

void        add_history(const char *line);
const char *history_item(int i);
int         history_size(void);
void        history_free(void);

#endif
