# Terminator

An X11 terminal and shell written in C, with independent tabs and two execution
modes: threads (default) and processes. Commands run as child processes, while
history is shared across tabs.

## Features

- **Task 1 — X11 GUI and tabs:** Window, scrolling and editable command input.
- **Task 2 — Built-in and external commands:** Command execution and last exit status through `$?`.
- **Task 3 — Multiline input and quoting:** Variables and nested command substitutions.
- **Task 4 — I/O redirection:** Input and output redirection using `<` and `>`.
- **Task 5 — Pipes:** Connect commands using pipelines (`|`).
- **Task 6 — multiWatch:** Monitor multiple commands in parallel.
- **Task 7 — Hotkeys:** Configurable editing shortcuts and X11 clipboard copy/paste.
- **Task 8 — Signals and job control:** Ctrl+C, Ctrl+Z, `&`, `jobs`, `fg` and `bg`.
- **Task 9 — Searchable history:** Persistent shared history and Ctrl+R search.
- **Task 10 — File-name auto-completion:** Complete filenames using Tab.
- **Task 11 — Running Bash scripts:** Internal interpretation of the required script subset, including nested
  `if`/`elif`/`else`, tests, positional arguments and `exit`.

## Project files

| File or folder | Purpose |
| --- | --- |
| `code/main.c` | Startup and execution-mode selection |
| `code/gui.c` | X11 window, tabs, editor and clipboard |
| `code/shell.c` | Parsing, expansion, commands, redirection, pipes and multiWatch |
| `code/shell_worker.c` | Workers, pipe communication, process launching and job control |
| `code/history.c` | Task 9: shared history, persistence and search |
| `code/shell_features.c` | Tasks 10–11: file-name completion and internal script interpretation |
| `code/terminator.h` | Shared types and function declarations |
| `Makefile` | Build and cleanup commands |
| `test/` | Three simple cases per task, grouped into three text files |

## Build and run on macOS

Install XQuartz and Apple's command-line tools. If needed, run
`xcode-select --install`. After first installing XQuartz, log out and back in.

In the normal Mac Terminal:

```bash
cd ~/Desktop/Terminator_Project
open -a XQuartz
make
./terminator
```

Enter commands inside the **Terminator - Tabs** window. For process mode:

```bash
./terminator useProcess=True
```

If the display cannot open, launch **Applications > Terminal** from XQuartz's
menu and run the executable from the project folder in that terminal.

## Build and run on Linux

On Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential libx11-dev pkg-config
cd Terminator_Project
make
./terminator
```

A running X server is required. Process mode uses the same command as on macOS.

## Controls

| Key | Action |
| --- | --- |
| Ctrl+T | Open a tab |
| Ctrl+Shift+W | Close a tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Left / Right | Move the cursor |
| Ctrl+A / Ctrl+E | Move to the start / end |
| Up / Down | Browse history |
| Ctrl+U / Ctrl+K / Ctrl+W | Delete to start / end / previous word |
| Ctrl+L | Clear the screen |
| Ctrl+C / Ctrl+Z | Interrupt / suspend the foreground job |
| Ctrl+D | Send EOF to a running command; close an empty idle tab |
| Ctrl+R | Search history |
| Tab | Complete a filename |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy / paste |

On Mac, use **Control**, not Command.

## Configuration and history

Custom shortcuts are stored in `~/.terminatorrc`, one setting per line:

```text
line_start=Ctrl+B
```

Run `bindings` to list shortcuts or `bindings reload` to reload the file.
Invalid or conflicting bindings fall back to defaults.

History is saved in `~/.terminator_history`. It retains 10,000 commands;
`history` displays the most recent 1,000.

## Notes

The shell supports ASCII text and line-buffered command input. It implements the
required script subset rather than full Bash; loops, functions and full-screen
terminal applications are not supported. Up to eight tabs can be opened.

Close running instances before rebuilding. Use `make clean` to remove generated
object files and the executable.

## Run the test cases

Open the text files in test/ with any text editor:

- task_1_to_5.txt
- task_6_to_8.txt
- task_9_to_11.txt

Run the cases in order inside **Terminator - Tabs**, entering commands one at a
time. Lines beginning with #task are headings. Lines beginning with Type, Press
or Run describe keyboard actions; follow them rather than entering those words.
Blank lines separate the three cases and the next task. For multiline quoted input,
press Enter after each line and continue at the > prompt. Output appears directly
in the coded window. These text files are instructions, not shell scripts.

Close the program and launch ./terminator useProcess=True to repeat in process
mode. Use the Mac Control key for shortcuts.

## Push to GitHub

Create an empty repository in your own GitHub account. In the normal Mac Terminal,
run these commands from the extracted project folder, replacing YOUR_USERNAME and
YOUR_REPOSITORY with your repository details:

```bash
git init
git add code Makefile README.md test .gitignore
git commit -m "Add Terminator shell and task tests"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/YOUR_REPOSITORY.git
git push -u origin main
```

Sign in if Git requests authentication. These commands are for a new empty
repository; for an existing repository, use its checkout and preserve its history.
