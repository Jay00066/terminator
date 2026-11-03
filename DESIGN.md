# Terminator — Design

## 1. Program Structure

The GUI handles X11 events and drawing; a separate worker manages each tab.  
Workers exchange commands and output through pipes, keeping the window responsive during execution.  
Only the GUI accesses Xlib, which keeps window operations in one place.

## 2. Independent Tabs and Execution Modes

Each tab keeps its own directory, variables, exit status and jobs; a directory descriptor prevents changes from affecting other tabs.  
Thread mode uses POSIX threads and a separate runner launched with `posix_spawn()`; process mode uses `fork()` for tab workers.  
Shared memory (`mmap()`) and a semaphore protect shared history, while a mutex protects sensitive thread-mode launch operations.

## 3. Command Processing

A lexer separates words and operators while preserving quoting rules; expansion handles variables and command substitutions.  
External commands run through `fork()` and `execvp()`, and `waitpid()` collects their termination status.  
Output and shell-state changes return through separate pipes, allowing foreground directory and variable changes to persist in the tab.

## 4. Display and Input

Each tab stores its text and cursor position; Expose and resize events redraw this state.  
Text wraps according to the window width without inserting newlines into the command.  
`poll()` waits for input and output without busy-waiting, while a ring buffer bounds stored output.

## 5. Redirection and Pipelines

`open()` obtains file descriptors, and `dup2()` connects them to standard input or output; a required file-opening failure prevents execution.  
A pipeline of N commands starts N child processes connected by N−1 pipes before waiting for completion.  
Unused descriptors are closed so the stages can finish and readers can detect EOF.

## 6. Job Control and multiWatch

A job table records job states and process groups; `kill()` delivers signals to the complete job.  
multiWatch uses separate output pipes and `poll()` so a slow command does not delay another command’s output.  
Each command restarts after its completion delay, and a silent completion produces one empty output block.

## 7. History, Completion and Scripts

Shared history uses synchronization and file locking; search checks exact matches first, then finds the longest common substring using a suffix automaton.  
Filename completion scans the tab’s directory and replaces only the relevant input word, preserving the rest of the line.  
Scripts are checked and parsed into a syntax tree before internal execution; sourced scripts update the current tab, and cleanup closes descriptors and reaps children.

## 8. Code Files

### `main.c`

- Reads the requested execution mode.
- Initializes shared resources and starts the GUI.
- Handles the entry point for internal command runners.

### `gui.c`

- Creates the X11 window and draws each tab’s text.
- Handles typing, hotkeys, clipboard selection and resizing.
- Sends requests to workers and displays their replies.

### `shell.c`

- Parses command input, quoting, variables and substitutions.
- Runs built-ins, external commands, redirection and pipelines.
- Implements multiWatch and the common command-execution routines.

### `job_manager.c`

- Runs tab workers in thread mode or process mode.
- Tracks jobs, process groups and foreground command state.
- Handles worker communication, signals and job cleanup.

### `history.c`

- Maintains the shared command history and its saved file.
- Protects history access with synchronization and file locking.
- Finds exact matches and longest common substring matches.

### `completion_scripts.c`

- Finds filename matches and calculates their common prefix.
- Parses scripts and checks their syntax before execution.
- Interprets variables, arguments and conditional branches.

### `terminator.h`

- Declares the shared structures, constants and message types.
- Provides function declarations used across source files.
- Keeps module interfaces consistent without duplicate definitions.
