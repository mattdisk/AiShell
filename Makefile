CC      = cc
CFLAGS  = -Wall -Wextra -I/opt/homebrew/include -Isrc -pthread $(CFLAGS_EXTRA)
LDFLAGS = -L/opt/homebrew/lib -largtable3 -pthread
TARGET  = aishell
BUILD   = build

DEMO_SERVER     = tools/tiny_http_server
DEMO_SERVER_SRC = tools/tiny_http_server.c

SRCS = \
  src/main.c \
  src/token.c \
  src/command.c \
  src/registry.c \
  src/exec.c \
  src/ast.c \
  src/parser.c \
  src/ast_print.c \
  src/alias.c \
  src/history.c \
  src/builtins/astmode.c \
  src/builtins/help.c \
  src/builtins/ls.c \
  src/builtins/stat.c \
  src/builtins/cat.c \
  src/builtins/head.c \
  src/builtins/cp.c \
  src/builtins/mv.c \
  src/builtins/rm.c \
  src/builtins/mkdir.c \
  src/builtins/rmdir.c \
  src/builtins/touch.c \
  src/builtins/rg.c \
  src/builtins/edit_line.c \
  src/builtins/edit_replace.c \
  src/builtins/jobs.c \
  src/builtins/ps.c \
  src/builtins/kill.c \
  src/builtins/wait.c \
  src/builtins/cd.c \
  src/builtins/pwd.c \
  src/builtins/env.c \
  src/builtins/export.c \
  src/builtins/unset.c \
  src/builtins/type.c \
  src/net/socket_client.c \
  src/builtins/http.c \
  src/builtins/pkg.c \
  src/builtins/vi.c \
  src/builtins/less.c \
  src/builtins/history.c \
  src/builtins/alias.c \
  src/builtins/echo.c \
  src/builtins/printf.c \
  src/builtins/sleep.c \
  src/builtins/date.c \
  src/builtins/true.c \
  src/builtins/false.c \
  src/ai/ai_generate.c \
  src/ai/ai_config.c \
  src/ai/ai_json.c \
  src/ai/ai_validate.c \
  src/ai/claude_client.c \
  src/ai/mcp_server.c

OBJS = $(SRCS:src/%.c=$(BUILD)/%.o)

.PHONY: all clean docs demo demo-server

all: clean
	$(MAKE) $(TARGET)

demo-server: $(DEMO_SERVER)

$(DEMO_SERVER): $(DEMO_SERVER_SRC)
	$(CC) $(CFLAGS) -o $@ $<

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD) $(TARGET) $(DEMO_SERVER)

docs: $(TARGET)
	@mkdir -p docs
	./$(TARGET) --gen-docs docs

# Build with DEMO_TRACE enabled: prints [demo] lines to stderr showing
# which commands run in pthreads vs the main thread vs child processes.
demo:
	$(MAKE) clean
	$(MAKE) CFLAGS_EXTRA="-DDEMO_TRACE"
