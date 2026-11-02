/*
 * Defines shared constants, structures and message types.
 * Declares runtime objects used across the source files.
 * Provides the function interfaces between modules.
 */
#ifndef TERMINATOR_H
#define TERMINATOR_H

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <limits.h>
#include <glob.h>
#include <dirent.h>
#include <time.h>
#include <stdatomic.h>
#include <stddef.h>
#include <pthread.h>
#include <spawn.h>
#include <stdint.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_TABS 8
#define MAX_JOBS 64
#define JOB_GROUPS 256
typedef struct {
    atomic_int groups[JOB_GROUPS];
} JobGroups;
typedef struct {
    atomic_int roots[MAX_JOBS];
    JobGroups groups[MAX_JOBS];
} TabJobRegistry;
#define INPUT_SIZE 508
#define COMMAND_SIZE 8192
#define QUEUE_SIZE 64
#define MAX_LINES 1000
#define LINE_SIZE 600
#define HISTORY_SIZE 10000
#define HISTORY_DISPLAY_SIZE 1000
#define PAYLOAD_SIZE PATH_MAX

enum {
    REQUEST_COMMAND = 1,
    REQUEST_INTERRUPT,
    REQUEST_STOP,
    REQUEST_SET_STATUS,
    REQUEST_FRAGMENT,
    REQUEST_INPUT,
    REQUEST_EOF,
    REQUEST_SUSPEND
};
enum { REPLY_OUTPUT = 1, REPLY_DONE, REPLY_CWD, REPLY_STARTED };
typedef struct {
    int type;
    char text[INPUT_SIZE];
} Request;

_Static_assert(sizeof(Request) <= 512, "Request exceeds portable PIPE_BUF");
typedef struct {
    int type, value;
    char text[PAYLOAD_SIZE];
} Reply;
typedef struct {
    int count, start;
    char entries[HISTORY_SIZE][COMMAND_SIZE];
} History;

#define HISTORY_TEXT_SIZE (HISTORY_DISPLAY_SIZE * (COMMAND_SIZE + 16))
typedef struct {
    char text[HISTORY_TEXT_SIZE];
} HistorySnapshot;
#define CAPTURE_SIZE (1024 * 1024)
#define NESTING_LIMIT 32
#define VARIABLE_SIZE 65536
typedef struct {
    size_t used;
    char text[VARIABLE_SIZE];
} VariableStore;
typedef struct {
    int request_fd, reply_fd, directory_fd, slot;
    VariableStore variables;
    char cwd[PATH_MAX];
} Worker;

/* ------------------------------------------------------------------ */
/* Shared definitions - module interfaces                             */
/* ------------------------------------------------------------------ */
extern int process_mode;
extern char executable_path[4096];
extern int registry_fd;
extern pthread_mutex_t descriptor_mutex;
extern TabJobRegistry *job_registries;
extern HistorySnapshot *history_snapshots;
extern History *history_data;
extern sem_t *history_sem;
extern int gui_fd;
extern long descriptor_limit;
extern char initial_cwd[PATH_MAX];
extern char history_warning[512];

int set_flags(int fd, int nonblock);
int make_pipe(int p[2], int nonblocking_read);
int write_all(int fd, const void *data, size_t size);
int temporary_fd(void);
long long watch_now(void);
int command_completeness(const char *text);
int internal_runner(void);
void worker_main(Worker *argument);

void history_lock(void);
void history_unlock(void);
int history_init(void);
int history_add(const char *command);
void history_snapshot(int slot);
void history_destroy(void);
const char *history_entry(int chronological_index);

void history_refresh(void);

typedef struct {
    char cwd[PATH_MAX];
    int last_status, exit_requested;
    const char *history_text;
    VariableStore variables;
    char **arguments;
    int argument_count;
    const char *argument_zero;
} Shell;
typedef struct {
    int exit_requested;
    char cwd[PATH_MAX];
    VariableStore variables;
} ShellResult;
#define SHELL_STATE_HEADER (offsetof(ShellResult, variables) + offsetof(VariableStore, text))

extern JobGroups *active_job_groups;
int run_shell(Shell *shell, const char *command, int depth);
int lexical_completeness(const char *text);
int remove_background(char *text);
void shell_signals(void);
int shell_result_send(int fd, const Shell *shell);
int shell_result_complete(const ShellResult *result, size_t received);

typedef struct {
    int used, busy, status, request_fd, reply_fd, partial, scroll, input_length, cursor;
    int history_position, history_count, selection_active, anchor_line, anchor_col, end_line, end_col;
    int search_mode, search_saved_cursor;
    char search_saved[COMMAND_SIZE];
    char **completion_matches;
    int completion_count, completion_start, completion_end, completion_quote;
    char completion_digits[24];
    unsigned long generation;
    pid_t worker_pid, command_group;
    pthread_t worker_thread;
    char input[COMMAND_SIZE], cwd[PATH_MAX], draft[COMMAND_SIZE];
    char (*browse_history)[COMMAND_SIZE];
    char continued[COMMAND_SIZE];
    size_t continued_used;
    Request outgoing[QUEUE_SIZE];
    int queue_head, queue_count;
    char lines[MAX_LINES][LINE_SIZE];
    int line_count, line_start, line_length;
    Reply incoming;
    size_t incoming_used;
} Tab;

void remember_line(Tab *tab, const char *text);
void append_output(Tab *tab, const char *text);
void completion_clear(Tab *tab);
void completion_begin(Tab *tab);
void completion_digit(Tab *tab, char digit);
void completion_confirm(Tab *tab);
void history_search(Tab *tab, const char *term);

extern volatile sig_atomic_t shell_interrupted;
pid_t shell_wait(pid_t pid, int *status);
void inherited_environment(Shell *shell);
int extract_substitution(const char *text, size_t *position, int backtick, char **body);
int run_command(Shell *shell, const char *command, int depth);
int script_syntax_status(const char *text);
int script_assignment_prefix(const char *text);
int script_test(char **argv);
int execute_script_file(Shell *shell, const char *path, char **arguments, int count, int source);
int script_path(const char *name);

int gui_run(void);

#endif
