#ifndef AISHELL_MCP_SERVER_H
#define AISHELL_MCP_SERVER_H

#include <stddef.h>
#include "ai/ai_config.h"

/* Opaque server handle returned by mcp_server_start(). */
typedef struct mcp_server mcp_server_t;

typedef enum {
    MCP_SERVER_OK = 0,
    MCP_SERVER_DISABLED,        /* mcp_enabled == 0 in config */
    MCP_SERVER_INVALID_CONFIG,  /* e.g. empty token */
    MCP_SERVER_BIND_FAILED,
    MCP_SERVER_LISTEN_FAILED,
    MCP_SERVER_THREAD_FAILED,
} mcp_server_status_t;

mcp_server_status_t mcp_server_start(const ai_config_t *cfg,
                                      mcp_server_t **out);
int                 mcp_server_actual_port(const mcp_server_t *server);
void                mcp_server_stop(mcp_server_t *server);
const char         *mcp_server_status_name(mcp_server_status_t status);

/*
 * Write a temporary Claude Code MCP config file for the given server.
 * Creates /tmp/aishell_mcp_<pid>_XXXXXX with mode 0600.
 * path_out receives the NUL-terminated path.
 * Returns 0 on success, -1 on error.
 * The caller must unlink the file when it is no longer needed.
 */
int mcp_server_write_config(const mcp_server_t *server,
                             const ai_config_t  *cfg,
                             char               *path_out,
                             size_t              path_out_len);

#endif /* AISHELL_MCP_SERVER_H */
