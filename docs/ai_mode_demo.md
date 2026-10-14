aishell main process
  |
  | fork/exec + pipes
  v
claude CLI / Claude Code
  |
  | HTTP MCP client, controlled by --mcp-config
  v
aishell local MCP server at 127.0.0.1:<port>/mcp


                    terminal / user
                         |
                         v
                aishell main process
                         |
          +--------------+--------------+
          |                             |
          | fork/exec claude            | starts/listens
          v                             v
   claude CLI process          aishell MCP server process/thread
          |                             ^
          | HTTP over TCP socket        |
          | 127.0.0.1:<port>/mcp        |
          +-----------------------------+



# (1) Simple test cases

## 1  
@ show my current directory  
pwd

## 2  
@ list files in the current directory with details  
ls -l

## 3  
@ show the first 20 lines of README  
head -n 20 README.md

## 4  
@ search the source code for TODO or FIXME comments using one regex pattern  
rg -n "TODO|FIXME" src

## 5  
@ list all C source files under the src directory  
find src -type f -name '*.c'

## 6  
@ show the last 5 git commits in one-line format  
git log --oneline -n 5

## 7  
@ delete everything in the build directory forcefully   # safety demo, do not execute  
rm -r build

## 8  
@ give me a story  
@ not a shell task: Telling a story is not a shell command task. AiShell can only execute shell commands — it cannot generate creative content like stories.

@ what is the capital of France  
@ not a shell task: This is a general knowledge question, not a shell command task. The capital of France is Paris.

# (2) Complicated test cases

## 1  
@ count how many C source files are under each src subdirectory  
find src -name "*.c" -mindepth 2 | awk -F/ '{print $2}' | sort | uniq -c

## 2  
@ count static declarations in each C source file under src  
find src -type f -name "*.c" -exec grep -c "static" {} +

## 3  
@ show C files under src sorted by most recently modified  
find src -name "*.c" | xargs ls -lt  
find src -name "*.c" -type f -exec stat -f "%m %N" {} + | sort -rn

## 4  
@ show the 10 largest C source files under src  
find src -name "*.c" -print | xargs -n 1 wc -c | sort -rn | head -n 10

## 5  
@ count lines in all C source files under src  
find src -type f -name "*.c" | xargs wc -l

## 6  
@ find where MCP tool names are defined in the source code  
grep -R -n -e "tool_name" -e "get_shell_context" -e "validate_command" -e "list_aishell_builtins" -e "get_command_help" src/ai/mcp_server.c

## 7  
@ show the longest 10 lines in C source files under src  
find src -name "*.c" -print | xargs awk '{ print length, $0 }' | sort -rn | head -n 10




@ what tools can the MCP server use?

This will fail.

@ list all tools I can use in this shell

help # This works.