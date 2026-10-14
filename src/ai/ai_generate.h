#ifndef AISHELL_AI_GENERATE_H
#define AISHELL_AI_GENERATE_H

/*
 * at_command_handle -- handle a line entered with the @ prefix.
 *
 * nl_input points to the text AFTER the leading '@' character.
 * mcp_srv is the running MCP server instance, or NULL when not available.
 * When mcp_srv is non-NULL and a token is configured, a temporary Claude Code
 * MCP config is written for the duration of the Claude invocation so that
 * Claude can query AiShell's capabilities and validate proposed commands.
 */
struct mcp_server;   /* forward declaration — full type in ai/mcp_server.h */
void at_command_handle(const char *nl_input, struct mcp_server *mcp_srv);

#endif /* AISHELL_AI_GENERATE_H */
