/*
 * Parses commands, quoting, variables and substitutions.
 * Executes built-ins, external commands and script conditions.
 * Implements redirection, pipelines and multiWatch.
 */
#include "terminator.h"

/* ------------------------------------------------------------------ */
/* Task 2 - built-in and external commands                            */
/* ------------------------------------------------------------------ */
#define EXPANSION_SIZE 65536
#define ARGUMENT_LIMIT 1024

int shell_result_send(int fd, const Shell *shell) {
    ShellResult result = {0};
    result.exit_requested = shell->exit_requested;
    snprintf(result.cwd, sizeof(result.cwd), "%s", shell->cwd);
    result.variables = shell->variables;
    return write_all(fd, &result, SHELL_STATE_HEADER + result.variables.used);
}
int shell_result_complete(const ShellResult *result, size_t received) {
    return received >= SHELL_STATE_HEADER && result->variables.used <= sizeof(result->variables.text) &&
           received == SHELL_STATE_HEADER + result->variables.used;
}

volatile sig_atomic_t shell_interrupted;
static void shell_interrupt(int number) {
    (void)number;
    shell_interrupted = 1;
}
void shell_signals(void) {
    struct sigaction action = {0};
    action.sa_handler = shell_interrupt;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    shell_interrupted = 0;
}
pid_t shell_wait(pid_t pid, int *status) {
    for (;;) {
        if (shell_interrupted) kill(pid, SIGKILL);
        pid_t result = waitpid(pid, status, 0);
        if (result < 0 && errno == EINTR) continue;
        return result;
    }
}
static const char *variable_get(Shell *shell, const char *name) {
    size_t length = strlen(name);
    for (size_t i = 0; i < shell->variables.used;) {
        const char *entry = shell->variables.text + i;
        if (!strncmp(entry, name, length) && entry[length] == '=') return entry + length + 1;
        i += strlen(entry) + 1;
    }
    if (!strcmp(name, "PWD")) return shell->cwd;
    return getenv(name);
}
void inherited_environment(Shell *shell) {
    for (size_t i = 0; i < shell->variables.used;) {
        const char *entry = shell->variables.text + i, *equal = strchr(entry, '=');
        if (!equal) break;
        size_t length = (size_t)(equal - entry);
        char name[COMMAND_SIZE];
        if (length < sizeof(name)) {
            memcpy(name, entry, length);
            name[length] = 0;
            if (getenv(name)) setenv(name, equal + 1, 1);
        }
        i += strlen(entry) + 1;
    }
}
static int variable_set(Shell *shell, const char *name, size_t length, const char *value) {
    size_t old = 0, index = shell->variables.used;
    for (size_t i = 0; i < shell->variables.used;) {
        const char *entry = shell->variables.text + i;
        size_t n = strlen(entry) + 1;
        if (!strncmp(entry, name, length) && entry[length] == '=') { index = i; old = n; break; }
        i += n;
    }
    size_t size = length + 1 + strlen(value) + 1;
    if (shell->variables.used - old + size > sizeof(shell->variables.text)) {
        dprintf(2, "terminator: variable store limit (64 KiB) exceeded\n");
        return -1;
    }
    if (old) {
        memmove(shell->variables.text + index, shell->variables.text + index + old, shell->variables.used - index - old);
        shell->variables.used -= old;
    }
    char *destination = shell->variables.text + shell->variables.used;
    memcpy(destination, name, length);
    destination[length] = '=';
    strcpy(destination + length + 1, value);
    shell->variables.used += size;
    return 0;
}

JobGroups *active_job_groups;
static int job_group_add(pid_t group) {
    if (!active_job_groups) return 0;
    for (int i = 0; i < JOB_GROUPS; i++) {
        int empty = 0;
        if (atomic_compare_exchange_strong(&active_job_groups->groups[i], &empty, (int)group)) return 0;
    }
    errno = E2BIG;
    return -1;
}
static void job_group_remove(pid_t group) {
    if (active_job_groups)
        for (int i = 0; i < JOB_GROUPS; i++) {
            int old = (int)group;
            atomic_compare_exchange_strong(&active_job_groups->groups[i], &old, 0);
        }
}

static volatile sig_atomic_t watch_stop;
static void watch_signal(int number) { watch_stop = number; }
typedef struct {
    char *command;
    pid_t pid, group;
    int fd, finished;
    long long next_run;
} WatchCommand;
long long watch_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void watch_free(WatchCommand *items, size_t count) {
    for (size_t i = 0; i < count; i++) free(items[i].command);
    free(items);
}

static int watch_parse(const char *text, WatchCommand **out, size_t *count) {
    *out = NULL;
    *count = 0;
    while (*text == ' ' || *text == '\t' || *text == '\n') text++;
    if (*text++ != '[') return -1;
    for (;;) {
        while (*text == ' ' || *text == '\t' || *text == '\n') text++;
        if (*text != '"') goto invalid;
        text++;
        char *command = malloc(strlen(text) + 1);
        if (!command) goto invalid;
        size_t used = 0;
        while (*text && *text != '"') {
            if (*text == '\\' && (text[1] == '"' || text[1] == '\\')) text++;
            command[used++] = *text++;
        }
        if (*text != '"') { free(command); goto invalid; }
        text++;
        command[used] = '\0';
        size_t nonspace = strspn(command, " \t\n");
        if (!command[nonspace]) { free(command); goto invalid; }
        WatchCommand *grown = realloc(*out, (*count + 1) * sizeof(**out));
        if (!grown) { free(command); goto invalid; }
        *out = grown;
        (*out)[*count] = (WatchCommand){.command = command, .pid = -1, .group = -1, .fd = -1};
        (*count)++;
        while (*text == ' ' || *text == '\t' || *text == '\n') text++;
        if (*text == ',') { text++; continue; }
        if (*text != ']') goto invalid;
        text++;
        while (*text == ' ' || *text == '\t' || *text == '\n') text++;
        if (*text) goto invalid;
        return 0;
    }
invalid:
    watch_free(*out, *count);
    *out = NULL;
    *count = 0;
    return -1;
}
static int watch_launch(WatchCommand *items, size_t count, size_t index, Shell *shell, int depth) {
    int p[2];
    if (make_pipe(p, 1) == -1) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (setpgid(0, 0) == -1) _exit(126);
        struct sigaction action = {0};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
        sigaction(SIGPIPE, &action, NULL);
        for (size_t i = 0; i < count; i++)
            if (items[i].fd >= 0) close(items[i].fd);

        if (dup2(p[1], 1) == -1 || dup2(p[1], 2) == -1) _exit(126);
        if (p[1] > 2) close(p[1]);
        Shell inner = *shell;
        inner.exit_requested = 0;
        _exit(run_shell(&inner, items[index].command, depth + 1));
    }
    int error = errno;
    close(p[1]);
    if (pid == -1) { close(p[0]); errno = error; return -1; }

    if (setpgid(pid, pid) == -1 && errno != EACCES && errno != ESRCH) {
        error = errno;
        kill(pid, SIGKILL);
        close(p[0]);
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {}
        errno = error;
        return -1;
    }
    if (job_group_add(pid) == -1) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        close(p[0]);
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {}
        return -1;
    }
    items[index].pid = pid; items[index].group = pid;
    items[index].fd = p[0]; items[index].finished = 0;
    return 0;
}
/* Print every watch output block with its command and timestamp. */
static int watch_print(const char *command, const char *data, size_t length) {
    static const char rule[] = "----------------------------------------------------\n";
    if (dprintf(1, "\"%s\", %lld:\n", command, (long long)time(NULL)) < 0 || write_all(1, rule, sizeof(rule) - 1) == -1 ||
        write_all(1, data, length) == -1)
        return -1;
    if (length && data[length - 1] != '\n' && write_all(1, "\n", 1) == -1) return -1;
    return write_all(1, rule, sizeof(rule) - 1);
}
/* ------------------------------------------------------------------ */
/* Task 6 - multiWatch: parallel monitoring                           */
/* ------------------------------------------------------------------ */
static int execute_multiwatch(Shell *shell, const char *list, int depth) {
    WatchCommand *items = NULL;
    size_t count = 0;
    if (watch_parse(list, &items, &count) == -1) {
        dprintf(2, "Usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n");
        return 2;
    }

    for (size_t i = 0; i < count; i++)
        if (command_completeness(items[i].command)) {
            dprintf(2, "multiWatch: incomplete or unsupported command: %s\n", items[i].command);
            watch_free(items, count);
            return 2;
        }
    struct pollfd *fds = calloc(count, sizeof(*fds));
    if (!fds) { watch_free(items, count); return 1; }
    struct sigaction action = {0}, old_int, old_term, old_hup, old_pipe;
    action.sa_handler = watch_signal;
    sigemptyset(&action.sa_mask);
    watch_stop = 0;
    sigaction(SIGINT, &action, &old_int);
    sigaction(SIGTERM, &action, &old_term);
    sigaction(SIGHUP, &action, &old_hup);
    struct sigaction ignore = {0};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, &old_pipe);
    int status = 0;
    while (!watch_stop) {
        long long now = watch_now();
        if (now < 0) { status = 1; break; }
        int timeout = -1;
        for (size_t i = 0; i < count && !watch_stop; i++) {
            WatchCommand *w = &items[i];
            if (w->pid > 0) {
                pid_t result = waitpid(w->pid, NULL, WNOHANG);
                if (result == w->pid) {
                    w->pid = -1; w->finished = 1;
                    w->next_run = now + 2000;
                } else if (result == -1 && errno != EINTR) {
                    status = 1;
                    goto done;
                }
            }
            if (w->pid < 0 && w->fd < 0 && now >= w->next_run) {

                if (w->group > 0) { kill(-w->group, SIGKILL); job_group_remove(w->group); }
                w->group = -1;
                if (watch_launch(items, count, i, shell, depth) == -1) {
                    dprintf(2, "multiWatch: launch: %s\n", strerror(errno));
                    status = 1;
                    goto done;
                }
            }
            fds[i] = (struct pollfd){w->fd, POLLIN, 0};
            if (w->pid < 0 && w->fd < 0) {
                long long delay = w->next_run - now;
                int ms = delay <= 0 ? 0 : delay > INT_MAX ? INT_MAX : (int)delay;
                if (timeout < 0 || ms < timeout) timeout = ms;
            }

            if (w->pid > 0 && (timeout < 0 || timeout > 100)) timeout = 100;
        }
        if (watch_stop) break;
        int ready = poll(fds, (nfds_t)count, timeout);
        if (ready < 0) { if (errno == EINTR) continue; status = 1; break; }
        for (size_t i = 0; i < count && !watch_stop; i++) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
            char buffer[4096];
            ssize_t n = read(items[i].fd, buffer, sizeof(buffer));
            if (n > 0) {

                if (watch_print(items[i].command, buffer, (size_t)n) == -1) { status = 1; goto done; }
            } else if (n == 0) {
                close(items[i].fd);
                items[i].fd = -1;
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                status = 1;
                goto done;
            }
        }
    }
done:

    for (size_t i = 0; i < count; i++) {
        if (items[i].group > 0) { kill(-items[i].group, SIGKILL); job_group_remove(items[i].group); }
        if (items[i].fd >= 0) close(items[i].fd);
    }
    for (size_t i = 0; i < count; i++)
        if (items[i].pid > 0)
            while (waitpid(items[i].pid, NULL, 0) == -1 && errno == EINTR) {}
    if (watch_stop == SIGINT) {
        write_all(1, "^C\n", 3);
        status = 130; shell_interrupted = 1;
    } else if (watch_stop) status = 128 + watch_stop;
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGHUP, &old_hup, NULL);
    sigaction(SIGPIPE, &old_pipe, NULL);
    free(fds);
    watch_free(items, count);
    return status;
}

/* ------------------------------------------------------------------ */
/* Task 3 - lexer: quoting, multiline input and substitutions         */
/* ------------------------------------------------------------------ */
static int scan_context(const char *text, size_t *position, int terminator, int depth) {
    if (depth > NESTING_LIMIT) return -1;
    int quote = 0, word = 0, after_pipe = 0;
    for (size_t i = *position; text[i];) {
        unsigned char c = (unsigned char)text[i];
        if (quote == '\'') { if (c == '\'') quote = 0; i++; continue; }
        if (c == '\\') {
            if (!text[i + 1]) return 1;
            if (text[i + 1] != '\n') word = 1;
            if (quote == '"' && !strchr("$`\"\\\n", text[i + 1])) i++;
            else i += 2;
            continue;
        }
        if (c == '$' && text[i + 1] == '(') {
            word = 1;
            size_t inner = i + 2;
            int result = scan_context(text, &inner, ')', depth + 1);
            if (result) return result;
            i = inner;
            continue;
        }
        if (c == '`') {
            word = 1;
            size_t end = i + 1;
            for (; text[end] && text[end] != '`'; end++) {
                if (text[end] == '\\') { if (!text[end + 1]) return 1; end++; }
            }
            if (!text[end]) return 1;

            i = end + 1;
            continue;
        }
        if (c == '$' && text[i + 1] == '{') {
            word = 1;
            size_t end = i + 2;
            while (text[end] && text[end] != '}') end++;
            if (!text[end]) return 1;
            i = end + 1;
            continue;
        }
        if (c == '"') {
            word = 1; quote = quote == '"' ? 0 : '"';
            i++;
            continue;
        }
        if (!quote && c == '\'') {
            word = 1; quote = '\'';
            i++;
            continue;
        }
        if (!quote && terminator && c == (unsigned char)terminator) {
            if (after_pipe && !word) return -1;
            *position = i + 1;
            return 0;
        }
        if (!quote && c == '|') {
            if (!word) return -1;
            word = 0; after_pipe = 1;
            i++;
            continue;
        }
        if (!quote && !terminator && c == '&' && word && !text[i + 1 + strspn(text + i + 1, " \t\n")]) return 0;
        if (!quote && c == ';') {
            if (!word || (after_pipe && !word)) return -1;
            word = 0; after_pipe = 0;
            i++;
            continue;
        }
        if (!quote && c == '#' && (i == *position || strchr(" \t\n;|", text[i - 1]))) {
            while (text[i] && text[i] != '\n') i++;
            continue;
        }
        if (!quote && strchr("&()", c)) return -1;
        if (!quote && c == '\n' && !(after_pipe && !word)) {
            word = 0; after_pipe = 0;
        } else if (c != ' ' && c != '\t' && c != '\n') word = 1;
        i++;
    }
    return quote || terminator || (after_pipe && !word) ? 1 : 0;
}
int lexical_completeness(const char *text) {
    size_t position = 0;
    return scan_context(text, &position, 0, 0);
}
int command_completeness(const char *text) {
    int lexical = lexical_completeness(text);
    return lexical ? lexical : script_syntax_status(text);
}
int extract_substitution(const char *text, size_t *position, int backtick, char **body) {
    size_t start = *position + (backtick ? 1 : 2), end;
    if (backtick) {
        end = start;
        while (text[end] && text[end] != '`') { if (text[end] == '\\' && text[end + 1]) end++; end++; }
        if (!text[end]) return -1;
        *body = malloc(end - start + 1);
        if (!*body) return -1;
        size_t used = 0;
        for (size_t i = start; i < end; i++) {
            if (text[i] == '\\' && i + 1 < end && strchr("$`\\", text[i + 1])) i++;
            (*body)[used++] = text[i];
        }
        (*body)[used] = '\0';
        *position = end + 1;
    } else {
        end = start;
        if (scan_context(text, &end, ')', 0)) return -1;
        *body = strndup(text + start, end - start - 1);
        if (!*body) return -1;
        *position = end;
    }
    return 0;
}
/* Run the substitution in a child and capture its standard output. */
static char *capture_substitution(Shell *shell, const char *body, int depth, int *sub_status) {
    int p[2];
    if (make_pipe(p, 0) == -1) return NULL;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) == -1) _exit(126);
        if (p[1] > 2) close(p[1]);
        Shell inner = *shell;
        inner.exit_requested = 0;
        int status = run_shell(&inner, body, depth + 1);
        _exit(status);
    }
    close(p[1]);
    if (pid == -1) { close(p[0]); return NULL; }
    char *captured = malloc(CAPTURE_SIZE + 1);
    size_t used = 0;
    int failed = !captured;
    char buffer[4096];
    for (;;) {
        if (shell_interrupted) { failed = 1; break; }
        ssize_t n = read(p[0], buffer, sizeof(buffer));
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; failed = 1; break; }
        if (!failed) {
            if (used + (size_t)n > CAPTURE_SIZE) failed = 1;
            else { memcpy(captured + used, buffer, (size_t)n); used += (size_t)n; }
        }
    }
    close(p[0]);
    int status = 0;
    pid_t waited;
    waited = shell_wait(pid, &status);
    *sub_status = waited == -1 ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    if (failed) { free(captured); errno = E2BIG; return NULL; }
    while (used && captured[used - 1] == '\n') used--;
    captured[used] = '\0';
    return captured;
}

typedef struct {
    char text[EXPANSION_SIZE];
    unsigned char split[EXPANSION_SIZE], empty_quote[EXPANSION_SIZE], protected[EXPANSION_SIZE];
    size_t used;
} ExpandedWord;
static int word_add(ExpandedWord *word, const char *text, int split) {
    size_t length = strlen(text);
    if (word->used + length >= EXPANSION_SIZE) return -1;
    memcpy(word->text + word->used, text, length);
    memset(word->split + word->used, split, (size_t)length);
    memset(word->protected + word->used, !split, (size_t)length);
    word->used += length; word->text[word->used] = '\0';
    return 0;
}
static int char_add(ExpandedWord *word, char c) {
    char text[2] = {c, 0};
    return word_add(word, text, 0);
}
static int argument_add(char **argv, int *argc, const char *text, size_t length) {
    if (*argc >= ARGUMENT_LIMIT) return -1;
    argv[*argc] = strndup(text, length);
    if (!argv[*argc]) return -1;
    (*argc)++;
    argv[*argc] = NULL;
    return 0;
}
static int word_argument_add(char **argv, int *argc, const ExpandedWord *word, size_t start, size_t length) {
    int wildcard = 0;
    for (size_t i = start; i < start + length; i++)
        if (!word->protected[i] && strchr("*?[", word->text[i])) wildcard = 1;
    if (!wildcard) return argument_add(argv, argc, word->text + start, length);
    char *pattern = malloc(2 * length + 1);
    if (!pattern) return -1;
    size_t used = 0;
    for (size_t i = start; i < start + length; i++) {
        char c = word->text[i];
        if (c == '\\' || (word->protected[i] && strchr("*?[]", c))) pattern[used++] = '\\';
        pattern[used++] = c;
    }
    pattern[used] = '\0';
    glob_t matches = {0};
    int result = glob(pattern, 0, NULL, &matches);
    free(pattern);
    if (result == GLOB_NOMATCH) { globfree(&matches); return argument_add(argv, argc, word->text + start, length); }
    if (result) { globfree(&matches); return -1; }
    for (size_t i = 0; i < matches.gl_pathc; i++)
        if (argument_add(argv, argc, matches.gl_pathv[i], strlen(matches.gl_pathv[i])) == -1) {
            globfree(&matches);
            return -1;
        }
    globfree(&matches);
    return 0;
}
static void arguments_free(char **argv) {
    if (argv) { for (int i = 0; argv[i]; i++) free(argv[i]); free(argv); }
}
static int expansion(Shell *shell, const char *text, size_t *position, ExpandedWord *word, int quoted, int depth, int *sub_status) {
    size_t i = *position;
    if (text[i] == '`' || (text[i] == '$' && text[i + 1] == '(')) {
        char *body = NULL;
        if (extract_substitution(text, position, text[i] == '`', &body) == -1) return -1;
        char *value = capture_substitution(shell, body, depth, sub_status);
        free(body);
        if (!value) return -1;
        int result = word_add(word, value, !quoted);
        free(value);
        return result;
    }
    if (text[i] != '$') return -1;
    i++;
    char name[256];
    size_t used = 0;
    int braced = text[i] == '{';
    if (braced) i++;
    if (text[i] == '?' || text[i] == '#') {
        name[used++] = text[i++];
    } else if (text[i] >= '0' && text[i] <= '9') {
        do {
            if (used >= sizeof(name) - 1) return -1;
            name[used++] = text[i++];
        } while (braced && text[i] >= '0' && text[i] <= '9');
    } else {
        if (!((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z') || text[i] == '_')) {
            if (braced) return -1;
            *position = i;
            return char_add(word, '$');
        }
        while ((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z') || (text[i] >= '0' && text[i] <= '9') ||
               text[i] == '_') {
            if (used >= sizeof(name) - 1) return -1;
            name[used++] = text[i++];
        }
    }
    if (braced) {
        if (text[i] != '}') return -1;
        i++;
    }
    name[used] = '\0';
    *position = i;
    char status_text[32];
    const char *value;
    if (!strcmp(name, "?")) {
        snprintf(status_text, sizeof(status_text), "%d", shell->last_status);
        value = status_text;
    } else if (!strcmp(name, "#")) {
        snprintf(status_text, sizeof(status_text), "%d", shell->argument_count);
        value = status_text;
    } else if (name[0] >= '0' && name[0] <= '9') {
        char *end;
        errno = 0;
        unsigned long number = strtoul(name, &end, 10);
        if (errno || *end) value = "";
        else if (!number) value = shell->argument_zero ? shell->argument_zero : "terminator";
        else value = number <= (unsigned long)shell->argument_count ? shell->arguments[number - 1] : "";
    } else value = variable_get(shell, name);
    return word_add(word, value ? value : "", !quoted);
}
/* ------------------------------------------------------------------ */
/* Task 4 - input/output redirection                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    int target;
    char *path;
} Redirection;
typedef struct {
    Redirection items[ARGUMENT_LIMIT];
    int count;
} Redirections;
static void redirections_free(Redirections *redirections) {
    for (int i = 0; i < redirections->count; i++) free(redirections->items[i].path);
    redirections->count = 0;
}

/* Open redirections before execution; save descriptors for built-in restoration. */
static int redirections_apply(const Redirections *redirections, int saved[2]) {
    saved[0] = saved[1] = -1;
    if (!redirections->count) return 0;
    for (int i = 0; i < 2; i++) { saved[i] = dup(i); if (saved[i] < 0 || set_flags(saved[i], 0) == -1) goto failure; }
    for (int i = 0; i < redirections->count; i++) {
        const Redirection *r = &redirections->items[i];
        int flags = r->target == 0 ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC;
        int fd = open(r->path, flags, 0666);
        if (fd < 0) { dprintf(2, "terminator: %s: %s\n", r->path, strerror(errno)); return -1; }
        if (dup2(fd, r->target) < 0) { close(fd); goto failure; }
        close(fd);
    }
    return 0;
failure:
    dprintf(2, "terminator: redirection: %s\n", strerror(errno));
    return -1;
}
static void redirections_restore(int saved[2]) {
    for (int i = 0; i < 2; i++)
        if (saved[i] >= 0) { dup2(saved[i], i); close(saved[i]); saved[i] = -1; }
}
/* ---------- expansion and argument parsing ---------- */
static char **parse_arguments(Shell *shell, const char *text, int depth, int *sub_status, Redirections *redirections, int assignment_only) {
    char **argv = calloc(ARGUMENT_LIMIT + 1, sizeof(*argv));
    ExpandedWord *word = malloc(sizeof(*word));
    if (!argv || !word) { free(argv); free(word); return NULL; }
    int argc = 0, pending = -1;
    size_t i = 0;
    while (text[i]) {
        while (text[i] == ' ' || text[i] == '\t' || text[i] == '\n') i++;
        if (!text[i]) break;
        if (text[i] == '<' || text[i] == '>') {
            if (pending >= 0 || text[i + 1] == '<' || text[i + 1] == '>') goto syntax_error;
            pending = text[i] == '<' ? 0 : 1;
            i++;
            continue;
        }
        int before = argc, assignment = assignment_only && pending < 0;
        if (assignment && !script_assignment_prefix(text + i)) goto syntax_error;
        memset(word, 0, sizeof(*word));
        int quote = 0;
        if (text[i] == '~' && (text[i + 1] == '/' || !text[i + 1] || text[i + 1] == ' ' || text[i + 1] == '\t')) {
            const char *home = variable_get(shell, "HOME");
            if (home) { if (word_add(word, home, 0) == -1) goto error; i++; }
        }
        while (text[i]) {
            char c = text[i];
            if (quote == '\'') {
                if (c == '\'') {
                    quote = 0; word->empty_quote[word->used] = 1;
                } else if (char_add(word, c) == -1) goto error;
                i++;
                continue;
            }
            if (c == '\\') {
                char next = text[i + 1];
                if (!next) goto error;
                if (quote == '"' && !strchr("$`\"\\\n", next)) {
                    if (char_add(word, c) == -1) goto error;
                    i++;
                    continue;
                }
                if (next != '\n' && char_add(word, next) == -1) goto error;
                i += 2;
                continue;
            }
            if (c == '"') {
                quote = quote == '"' ? 0 : '"'; word->empty_quote[word->used] = 1;
                i++;
                continue;
            }
            if (!quote && c == '\'') {
                quote = '\''; word->empty_quote[word->used] = 1;
                i++;
                continue;
            }
            if (c == '$' || c == '`') {
                if (expansion(shell, text, &i, word, quote == '"', depth, sub_status) == -1) goto error;
                continue;
            }
            if (!quote && (c == ' ' || c == '\t' || c == '\n')) break;
            if (!quote && (c == '<' || c == '>')) break;
            if (!quote && strchr("|&;()", c)) goto syntax_error;
            if (char_add(word, c) == -1) goto error;
            if (!quote) word->protected[word->used - 1] = 0;
            i++;
        }
        if (quote) goto error;
        if (assignment) {
            char *equal = strchr(word->text, '=');
            if (!equal || variable_set(shell, word->text, (size_t)(equal - word->text), equal + 1) < 0) goto error;
            continue;
        }
        size_t start = 0;
        int preserve_empty = 0;
        for (size_t j = 0; j <= word->used; j++) {
            if (word->empty_quote[j]) preserve_empty = 1;
            int delimiter = j == word->used || (word->split[j] && strchr(" \t\n", word->text[j]));
            if (delimiter) {
                if ((j > start || preserve_empty) && word_argument_add(argv, &argc, word, start, j - start) == -1) goto error;
                start = j + 1; preserve_empty = 0;
            }
        }
        if (pending >= 0) {
            if (argc - before != 1 || redirections->count >= ARGUMENT_LIMIT) goto syntax_error;
            Redirection *r = &redirections->items[redirections->count++];
            r->target = pending; r->path = argv[before];
            argv[before] = NULL; argc = before;
            pending = -1;
        }
    }
    if (pending >= 0) goto syntax_error;
    free(word);
    return argv;
syntax_error:
    dprintf(2, "terminator: invalid redirection; use < filename or > filename (one filename)\n");
error:
    free(word);
    arguments_free(argv);
    return NULL;
}

static size_t next_separator(const char *text, size_t start, int include_pipe) {
    int quote = 0, after_pipe = 0;
    size_t i = start;
    while (text[i]) {
        char c = text[i];
        if (quote == '\'') { if (c == '\'') quote = 0; i++; continue; }
        if (c != ' ' && c != '\t' && c != '\n' && c != '|' && c != '\\') after_pipe = 0;
        if (c == '\\') {
            if (text[i + 1] != '\n') after_pipe = 0;
            if (quote == '"' && !strchr("$`\"\\\n", text[i + 1])) i++;
            else i += 2;
            continue;
        }
        if (c == '$' && text[i + 1] == '(') {
            size_t end = i + 2;
            if (scan_context(text, &end, ')', 0)) return strlen(text);
            i = end;
            continue;
        }
        if (c == '`') {
            i++;
            while (text[i] && text[i] != '`') { if (text[i] == '\\' && text[i + 1]) i++; i++; }
            if (text[i]) i++;
            continue;
        }
        if (c == '$' && text[i + 1] == '{') {
            i += 2;
            while (text[i] && text[i] != '}') i++;
            if (text[i]) i++;
            continue;
        }
        if (c == '"') { quote = quote == '"' ? 0 : '"'; i++; continue; }
        if (!quote && c == '\'') { quote = '\''; i++; continue; }
        if (!quote && c == '|') {
            if (include_pipe) return i;
            after_pipe = 1;
            i++;
            continue;
        }
        if (!quote && c == '\n' && !after_pipe) return i;
        i++;
    }
    return i;
}

/* ------------------------------------------------------------------ */
/* Task 2 - built-in and external commands                            */
/* ------------------------------------------------------------------ */
/* Built-ins run here; external programs replace a child with execvp(). */
static int execute_simple(Shell *shell, char **argv, Redirections *redirections, int substitution_status, int direct_exec) {
    int saved[2];
    if (redirections_apply(redirections, saved) == -1) {
        redirections_restore(saved);
        shell->last_status = 1;
        return 1;
    }
    int status = shell->last_status;
    int argc = 0;
    while (argv[argc]) argc++;
    if (!argc) {
        if (substitution_status >= 0) status = substitution_status;
        else if (redirections->count || direct_exec) status = 0;
    } else if (!strcmp(argv[0], "cd")) {
        const char *path = argc == 2 ? argv[1] : variable_get(shell, "HOME");
        if (argc > 2) {
            dprintf(2, "terminator: cd: too many arguments\n");
            status = 2;
        } else if (!path || chdir(path) == -1) {
            dprintf(2, "terminator: cd: %s\n", path ? strerror(errno) : "HOME is not set");
            status = 1;
        } else if (!getcwd(shell->cwd, sizeof(shell->cwd))) {
            dprintf(2, "terminator: pwd: %s\n", strerror(errno));
            status = 1;
        } else {
            variable_set(shell, "PWD", 3, shell->cwd);
            status = 0;
        }
    } else if (!strcmp(argv[0], "source") || !strcmp(argv[0], ".")) {
        if (argc < 2) {
            dprintf(2, "Usage: source filename [arguments]\n");
            status = 2;
        } else status = execute_script_file(shell, argv[1], argv + 2, argc > 2 ? argc - 2 : -1, 1);
    } else if (!strcmp(argv[0], "bash") || !strcmp(argv[0], "/bin/bash")) {
        int first = argc > 1 && !strcmp(argv[1], "--") ? 2 : 1;
        if (argc <= first || argv[first][0] == '-') {
            dprintf(2, "Usage: bash script-file [arguments] (internal interpreter)\n");
            status = 2;
        } else status = execute_script_file(shell, argv[first], argv + first + 1, argc - first - 1, 0);
    } else if (!strcmp(argv[0], "[") || !strcmp(argv[0], "test")) {
        status = script_test(argv);
    } else if (!strcmp(argv[0], ":") || !strcmp(argv[0], "true")) status = 0;
    else if (!strcmp(argv[0], "false")) status = 1;
    else if (script_path(argv[0])) {
        if (access(argv[0], X_OK) < 0) {
            int error = errno;
            dprintf(2, "terminator: %s: %s\n", argv[0], strerror(error));
            status = error == ENOENT ? 127 : 126;
        } else status = execute_script_file(shell, argv[0], argv + 1, argc - 1, 0);
    } else if (!strcmp(argv[0], "pwd")) {
        if (argc != 1) {
            dprintf(2, "Usage at this stage: pwd\n");
            status = 2;
        } else {
            dprintf(1, "%s\n", shell->cwd);
            status = 0;
        }
    } else if (!strcmp(argv[0], "history")) {
        if (argc != 1) {
            dprintf(2, "Usage at this stage: history\n");
            status = 2;
        } else {
            const char *text = shell->history_text ? shell->history_text : "";
            status = write_all(1, text, strlen(text)) == -1 ? 1 : 0;
        }
    } else if (!strcmp(argv[0], "exit")) {
        if (argc > 2) {
            dprintf(2, "terminator: exit: too many arguments\n");
            status = 2;
        } else {
            char *end = NULL;
            long value = argc == 2 ? strtol(argv[1], &end, 10) : shell->last_status;
            if (argc == 2 && (!*argv[1] || *end)) {
                dprintf(2, "terminator: exit: numeric argument required\n");
                status = 2;
            } else {
                status = (unsigned char)value; shell->exit_requested = 1;
            }
        }
    } else {
        if (direct_exec) {
            inherited_environment(shell);
            execvp(argv[0], argv);
            int error = errno;
            if (error == ENOENT) dprintf(2, "terminator: %s: command not found\n", argv[0]);
            else dprintf(2, "terminator: %s: %s\n", argv[0], strerror(error));
            return error == ENOENT ? 127 : 126;
        }
        pid_t pid = fork();
        if (pid == 0) {
            inherited_environment(shell);
            execvp(argv[0], argv);
            int error = errno;
            if (error == ENOENT) dprintf(2, "terminator: %s: command not found\n", argv[0]);
            else dprintf(2, "terminator: %s: %s\n", argv[0], strerror(error));
            _exit(error == ENOENT ? 127 : 126);
        }
        if (pid == -1) {
            dprintf(2, "terminator: fork: %s\n", strerror(errno));
            status = 1;
        } else {
            int result = 0;
            pid_t waited;
            waited = shell_wait(pid, &result);
            status = waited == -1 ? 1 : WIFEXITED(result) ? WEXITSTATUS(result) : WIFSIGNALED(result) ? 128 + WTERMSIG(result) : 1;
        }
    }
    redirections_restore(saved);
    shell->last_status = status;
    return status;
}
typedef struct {
    char **argv;
    Redirections redirections;
    int substitution_status;
    VariableStore variables;
} PipelineStage;

static size_t pipeline_stage_start(const char *command, size_t start) {
    for (;;) {
        if (command[start] && strchr(" \t\n", command[start])) start++;
        else if (command[start] == '\\' && command[start + 1] == '\n') start += 2;
        else return start;
    }
}
/* ------------------------------------------------------------------ */
/* Task 5 - pipes and pipeline execution                              */
/* ------------------------------------------------------------------ */
/* Connect all stages before waiting so producers and consumers run together. */
static int execute_pipeline(Shell *shell, const char *command, int depth) {
    size_t count = 1;
    for (size_t start = 0;;) {
        size_t end = next_separator(command, start, 1);
        if (command[end] != '|') break;
        count++;
        start = end + 1; start = pipeline_stage_start(command, start);
    }
    PipelineStage *stages = calloc(count, sizeof(*stages));
    pid_t *children = calloc(count, sizeof(*children));
    if (!stages || !children) { free(stages); free(children); return 1; }
    int status = 2;
    size_t start = 0, launched = 0;
    int previous = -1;
    for (size_t i = 0; i < count; i++) {
        size_t end = next_separator(command, start, 1);
        char *part = strndup(command + start, end - start);
        if (!part) goto cleanup;
        stages[i].substitution_status = -1;
        Shell stage = *shell;
        stages[i].argv = parse_arguments(&stage, part, depth, &stages[i].substitution_status, &stages[i].redirections,
                                         script_assignment_prefix(part + strspn(part, " \t\n")));
        stages[i].variables = stage.variables;
        free(part);
        if (!stages[i].argv) goto cleanup;
        start = command[end] ? end + 1 : end; start = pipeline_stage_start(command, start);
    }
    for (size_t i = 0; i < count; i++) {
        int next[2] = {-1, -1};
        if (i + 1 < count && make_pipe(next, 0) == -1) goto launch_failed;
        pid_t pid = fork();
        if (pid == 0) {
            if (previous >= 0 && dup2(previous, 0) == -1) _exit(126);
            if (next[1] >= 0 && dup2(next[1], 1) == -1) _exit(126);
            if (previous >= 0) close(previous);
            if (next[0] >= 0) close(next[0]);
            if (next[1] >= 0) close(next[1]);
            Shell inner = *shell;
            inner.exit_requested = 0; inner.variables = stages[i].variables;
            int result = execute_simple(&inner, stages[i].argv, &stages[i].redirections, stages[i].substitution_status, 1);
            _exit(result);
        }
        if (previous >= 0) { close(previous); previous = -1; }
        if (next[1] >= 0) close(next[1]);
        if (pid == -1) { if (next[0] >= 0) close(next[0]); goto launch_failed; }
        previous = next[0]; children[launched++] = pid;
    }
    status = 0;
    for (size_t i = 0; i < launched; i++) {
        int result = 0;
        pid_t waited;
        waited = shell_wait(children[i], &result);
        if (i == launched - 1)
            status = waited == -1 ? 1 : WIFEXITED(result) ? WEXITSTATUS(result) : WIFSIGNALED(result) ? 128 + WTERMSIG(result) : 1;
    }
    goto cleanup;
launch_failed:
    dprintf(2, "terminator: pipeline: %s\n", strerror(errno));
    status = 1;
    if (previous >= 0) { close(previous); previous = -1; }
    for (size_t i = 0; i < launched; i++) kill(children[i], SIGKILL);
    for (size_t i = 0; i < launched; i++)
        while (waitpid(children[i], NULL, 0) == -1 && errno == EINTR) {}
cleanup:
    if (previous >= 0) close(previous);
    for (size_t i = 0; i < count; i++) { arguments_free(stages[i].argv); redirections_free(&stages[i].redirections); }
    free(stages); free(children);
    shell->last_status = status;
    return status;
}
int run_command(Shell *shell, const char *command, int depth) {
    if (depth > NESTING_LIMIT) { dprintf(2, "terminator: substitution nesting limit exceeded\n"); return 2; }

    const char *start_command = command + strspn(command, " \t\n");
    if (!strncmp(start_command, "multiWatch", 10) && (!start_command[10] || strchr(" \t\n[", start_command[10]))) {
        shell->last_status = execute_multiwatch(shell, start_command + 10, depth);
        return shell->last_status;
    }
    size_t pipeline_end = next_separator(command, 0, 1);
    if (command[pipeline_end] == '|') return execute_pipeline(shell, command, depth);
    int substitution_status = -1;
    Redirections redirections = {0};
    int assignment = script_assignment_prefix(start_command);
    char **argv = parse_arguments(shell, command, depth, &substitution_status, &redirections, assignment);
    if (!argv) {
        redirections_free(&redirections);
        if (shell_interrupted) return 130;
        dprintf(2, "terminator: invalid syntax or expansion size limit exceeded\n");
        return 2;
    }
    if (assignment && !argv[0]) shell->last_status = 0;
    int status = execute_simple(shell, argv, &redirections, substitution_status, 0);
    shell->last_status = status;
    redirections_free(&redirections);
    arguments_free(argv);
    return status;
}
