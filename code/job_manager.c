/*
 * Runs each tab worker in thread or process mode.
 * Communicates with the GUI through command and output pipes.
 * Launches command runners and manages signals and jobs.
 */
#include "terminator.h"

int temporary_fd(void) {
    char path[] = "/tmp/terminator-XXXXXX";
    pthread_mutex_lock(&descriptor_mutex);
    int fd = mkstemp(path);
    if (fd >= 0) {
        unlink(path);
        if (set_flags(fd, 0) < 0) {
            int e = errno;
            close(fd);
            fd = -1; errno = e;
        }
    }
    int saved = errno;
    pthread_mutex_unlock(&descriptor_mutex);
    errno = saved;
    return fd;
}
int set_flags(int fd, int nonblock) {
    int f = fcntl(fd, F_GETFD);
    if (f == -1 || fcntl(fd, F_SETFD, f | FD_CLOEXEC) == -1) return -1;
    if (nonblock) { f = fcntl(fd, F_GETFL); if (f == -1 || fcntl(fd, F_SETFL, f | O_NONBLOCK) == -1) return -1; }
    return 0;
}
int make_pipe(int p[2], int nonblocking_read) {
    pthread_mutex_lock(&descriptor_mutex);
    int result = pipe(p);
    if (result == 0 && (set_flags(p[0], nonblocking_read) < 0 || set_flags(p[1], 0) < 0)) {
        int error = errno;
        close(p[0]); close(p[1]);
        errno = error; result = -1;
    }
    int saved = errno;
    pthread_mutex_unlock(&descriptor_mutex);
    errno = saved;
    return result;
}
int write_all(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n > 0) {
            p += n; size -= (size_t)n;
        } else if (n == -1 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

/* ---------- worker-to-GUI output ---------- */
static int reply_send(Worker *w, int type, int value, const char *text) {
    Reply r = {0};
    r.type = type; r.value = value;
    if (text) snprintf(r.text, sizeof(r.text), "%s", text);
    return write_all(w->reply_fd, &r, sizeof(r));
}
static int worker_error(Worker *w, const char *operation) {
    char message[PAYLOAD_SIZE];
    snprintf(message, sizeof(message), "terminator: %s: %s\n", operation, strerror(errno));
    return reply_send(w, REPLY_OUTPUT, 0, message);
}
static int change_directory(Worker *w, const char *path) {
    char candidate[PATH_MAX], resolved[PATH_MAX];
    if (!path) path = getenv("HOME");
    if (!path) { errno = ENOENT; return -1; }
    int n;
    if (path[0] == '/') n = snprintf(candidate, sizeof(candidate), "%s", path);
    else n = snprintf(candidate, sizeof(candidate), "%s/%s", w->cwd, path);
    if (n < 0 || (size_t)n >= sizeof(candidate)) { errno = ENAMETOOLONG; return -1; }
    if (!realpath(candidate, resolved)) return -1;
    int fd = open(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd == -1) return -1;
    if (set_flags(fd, 0) == -1) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    close(w->directory_fd);
    w->directory_fd = fd;
    snprintf(w->cwd, sizeof(w->cwd), "%s", resolved);
    return 0;
}
/* ------------------------------------------------------------------ */
/* Execution modes - thread and process workers                       */
/* ------------------------------------------------------------------ */
static _Thread_local int peer_job_fds[192], peer_job_fd_count;

typedef struct {
    int slot, job, last_status;
    VariableStore variables;
    size_t history_length;
    char cwd[PATH_MAX], command[COMMAND_SIZE];
} RunnerStart;

static int read_exact(int fd, void *buffer, size_t length) {
    char *p = buffer;
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n > 0) {
            p += n; length -= (size_t)n;
        } else if (n < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

int internal_runner(void) {
    RunnerStart start;
    if (read_exact(3, &start, sizeof(start)) < 0 || start.slot < 0 || start.slot >= MAX_TABS || start.job < 0 || start.job >= MAX_JOBS ||
        start.history_length >= HISTORY_TEXT_SIZE)
        return 126;
    start.cwd[PATH_MAX - 1] = '\0'; start.command[COMMAND_SIZE - 1] = '\0';
    char *history = malloc(start.history_length + 1);
    if (!history || read_exact(3, history, start.history_length) < 0) { free(history); return 126; }
    history[start.history_length] = '\0';
    close(3);
    job_registries = mmap(NULL, MAX_TABS * sizeof(*job_registries), PROT_READ | PROT_WRITE, MAP_SHARED, 5, 0);
    close(5);
    if (job_registries == MAP_FAILED) { free(history); return 126; }
    active_job_groups = &job_registries[start.slot].groups[start.job];
    atomic_store(&job_registries[start.slot].roots[start.job], (int)getpid());
    if (fchdir(6) < 0) { free(history); return 126; }
    close(6);

    if (set_flags(4, 0) < 0) { free(history); return 126; }
    shell_signals();
    Shell shell = {0};
    snprintf(shell.cwd, sizeof(shell.cwd), "%s", start.cwd);
    shell.last_status = start.last_status; shell.history_text = history;
    shell.variables = start.variables;
    int status = run_shell(&shell, start.command, 0);
    if (shell_result_send(4, &shell) < 0) status = 1;
    close(4); free(history);
    munmap(job_registries, MAX_TABS * sizeof(*job_registries));
    return status;
}

static pid_t spawn_thread_job(Worker *w, const char *command, int last_status, int *output_fd, int *state_fd, int *input_fd,
                              JobGroups *groups) {
    history_lock();
    char *history_copy = strdup(history_snapshots[w->slot].text);
    history_unlock();
    if (!history_copy) return -1;
    int out[2] = {-1, -1}, state[2] = {-1, -1}, in[2] = {-1, -1};
    int boot = -1, copies[7];
    for (int i = 0; i < 7; i++) copies[i] = -1;
    pid_t pid = -1;
    if (make_pipe(out, 1) < 0 || make_pipe(state, 1) < 0 || make_pipe(in, 0) < 0 || set_flags(in[1], 1) < 0) goto fail;
    boot = temporary_fd();
    if (boot < 0) goto fail;
    RunnerStart start = {0};
    start.slot = w->slot; start.variables = w->variables;
    start.job = (int)(groups - job_registries[w->slot].groups); start.last_status = last_status;
    snprintf(start.cwd, sizeof(start.cwd), "%s", w->cwd);
    snprintf(start.command, sizeof(start.command), "%s", command);
    start.history_length = strlen(history_copy);
    if (write_all(boot, &start, sizeof(start)) < 0 || write_all(boot, history_copy, start.history_length) < 0 ||
        lseek(boot, 0, SEEK_SET) < 0)
        goto fail;
    int originals[] = {in[0], out[1], out[1], boot, state[1], registry_fd, w->directory_fd};
    for (int i = 0; i < 7; i++) { copies[i] = fcntl(originals[i], F_DUPFD_CLOEXEC, 64); if (copies[i] < 0) goto fail; }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int e = posix_spawn_file_actions_init(&actions);
    if (e) { errno = e; goto fail; }
    e = posix_spawnattr_init(&attributes);
    if (e) { posix_spawn_file_actions_destroy(&actions); errno = e; goto fail; }
    for (int i = 0; i < 7 && !e; i++) e = posix_spawn_file_actions_adddup2(&actions, copies[i], i);
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTSTP);
    if (!e) e = posix_spawnattr_setsigmask(&attributes, &empty);
    if (!e) e = posix_spawnattr_setsigdefault(&attributes, &defaults);
    if (!e) e = posix_spawnattr_setpgroup(&attributes, 0);
    if (!e) e = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char *arguments[] = {executable_path, "--terminator-internal-runner", NULL};
    if (!e) {
        pthread_mutex_lock(&descriptor_mutex);
        e = posix_spawn(&pid, executable_path, &actions, &attributes, arguments, environ);
        pthread_mutex_unlock(&descriptor_mutex);
    }
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (e) { errno = e; pid = -1; goto fail; }
    for (int i = 0; i < 7; i++) close(copies[i]);
    free(history_copy); close(boot);
    close(out[1]); close(state[1]);
    close(in[0]);
    atomic_store(&job_registries[w->slot].roots[start.job], (int)pid);
    *output_fd = out[0];
    *state_fd = state[0];
    *input_fd = in[1];
    return pid;
fail:;
    int saved = errno;
    free(history_copy);
    for (int i = 0; i < 7; i++)
        if (copies[i] >= 0) close(copies[i]);
    if (boot >= 0) close(boot);
    for (int i = 0; i < 2; i++) {
        if (out[i] >= 0) close(out[i]);
        if (state[i] >= 0) close(state[i]);
        if (in[i] >= 0) close(in[i]);
    }
    errno = saved;
    return -1;
}

static pid_t spawn_shell(Worker *w, const char *command, int last_status, int *output_fd, int *state_fd, int *input_fd, JobGroups *groups) {
    if (!process_mode) return spawn_thread_job(w, command, last_status, output_fd, state_fd, input_fd, groups);
    char *history_copy = strdup(history_snapshots[w->slot].text);
    if (!history_copy) return -1;
    int out[2], state[2], in[2];
    if (make_pipe(out, 1) == -1) { free(history_copy); return -1; }
    if (make_pipe(state, 1) == -1) {
        close(out[0]); close(out[1]);
        free(history_copy);
        return -1;
    }
    if (make_pipe(in, 0) == -1) {
        close(out[0]); close(out[1]);
        close(state[0]); close(state[1]);
        free(history_copy);
        return -1;
    }
    if (set_flags(in[1], 1) == -1) {
        close(out[0]); close(out[1]);
        close(state[0]); close(state[1]);
        close(in[0]); close(in[1]);
        free(history_copy);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        active_job_groups = groups;
        ptrdiff_t index = groups - job_registries[w->slot].groups;
        atomic_store(&job_registries[w->slot].roots[index], (int)getpid());
        for (int i = 0; i < peer_job_fd_count; i++) close(peer_job_fds[i]);
        close(out[0]); close(state[0]);
        close(in[1]);
        if (setpgid(0, 0) == -1 || fchdir(w->directory_fd) == -1) _exit(126);
        struct sigaction action = {0};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGPIPE, &action, NULL);
        if (dup2(out[1], 1) == -1 || dup2(out[1], 2) == -1) _exit(126);
        if (out[1] > 2) close(out[1]);
        close(w->request_fd); close(w->reply_fd);
        close(w->directory_fd);
        if (dup2(in[0], 0) == -1) _exit(126);
        if (in[0] > 2) close(in[0]);
        shell_signals();
        Shell shell = {0};
        snprintf(shell.cwd, sizeof(shell.cwd), "%s", w->cwd);
        shell.last_status = last_status; shell.history_text = history_copy;
        shell.variables = w->variables;
        int status = run_shell(&shell, command, 0);
        if (shell_result_send(state[1], &shell) < 0) status = 1;
        close(state[1]);
        _exit(status);
    }
    int error = errno;
    free(history_copy); close(out[1]);
    close(state[1]); close(in[0]);
    if (pid == -1) {
        close(out[0]); close(state[0]);
        close(in[1]);
        errno = error;
        return -1;
    }
    ptrdiff_t index = groups - job_registries[w->slot].groups;
    atomic_store(&job_registries[w->slot].roots[index], (int)pid);
    setpgid(pid, pid);
    *output_fd = out[0];
    *state_fd = state[0];
    *input_fd = in[1];
    return pid;
}

typedef struct {
    int used, id, stopped, finished, status, output, state, input, input_eof, allow_state;
    pid_t pid, group;
    char command[COMMAND_SIZE], pending[COMMAND_SIZE * 4];
    size_t input_used, state_used;
    ShellResult result;
    JobGroups *groups;
} Job;
/* ------------------------------------------------------------------ */
/* Execution modes - thread and process workers                       */
/* ------------------------------------------------------------------ */
/* Signal the whole job group, including pipeline and watch children. */
static void signal_job(Job *job, int number) {
    if (job->group > 0) kill(-job->group, number);
    if (job->groups)
        for (int i = 0; i < JOB_GROUPS; i++) {
            int group = atomic_load(&job->groups->groups[i]);
            if (group > 0) kill(-group, number);
        }
}
static void job_close(Job *job) {
    if (job->output >= 0) close(job->output);
    if (job->state >= 0) close(job->state);
    if (job->input >= 0) close(job->input);
    if (job->groups)
        for (int g = 0; g < JOB_GROUPS; g++) atomic_store(&job->groups->groups[g], 0);
    memset(job, 0, sizeof(*job));
}
/* Job notifications include the job number, state and command. */
static int job_message(Worker *w, Job *job, const char *state) {
    char text[COMMAND_SIZE + 128];
    snprintf(text, sizeof(text), "[%d]+ %s %s\n", job->id, state, job->command);
    return reply_send(w, REPLY_OUTPUT, 0, text);
}
static Job *find_job(Job *jobs, const char *spec) {
    if (!spec || !strcmp(spec, "%+") || !strcmp(spec, "%%")) {
        Job *best = NULL;
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].used && !jobs[i].finished && (!best || jobs[i].id > best->id)) best = &jobs[i];
        return best;
    }
    if (*spec == '%') spec++;
    if (!*spec) return NULL;
    char *end;
    errno = 0;
    long id = strtol(spec, &end, 10);
    if (errno || *end || id <= 0 || id > INT_MAX) return NULL;
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used && !jobs[i].finished && jobs[i].id == id) return &jobs[i];
    return NULL;
}

int remove_background(char *text) {
    size_t n = strlen(text);
    while (n && strchr(" \t\n", text[n - 1])) n--;
    if (!n || text[n - 1] != '&') return 0;
    size_t slashes = 0, pos = n - 1;
    while (pos && text[pos - 1] == '\\') { slashes++; pos--; }
    if (slashes % 2) return 0;

    text[n - 1] = '\0';
    return 1;
}
/* ------------------------------------------------------------------ */
/* Task 8 - signals and job control                                   */
/* ------------------------------------------------------------------ */
static void job_flush_input(Job *job) {
    if (job->input >= 0 && job->input_used) {
        ssize_t written = write(job->input, job->pending, job->input_used);
        if (written > 0) {
            job->input_used -= (size_t)written;
            memmove(job->pending, job->pending + written, job->input_used);
        } else if (written < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(job->input);
            job->input = -1; job->input_used = 0;
        }
    }
    if (job->input >= 0 && job->input_eof && !job->input_used) { close(job->input); job->input = -1; }
}

static int job_forward_output(Worker *worker, Job *job) {
    for (int batch = 0; job->output >= 0 && batch < 8; batch++) {
        char text[PAYLOAD_SIZE];
        ssize_t length = read(job->output, text, sizeof(text) - 1);
        if (length > 0) {
            text[length] = '\0';
            if (reply_send(worker, REPLY_OUTPUT, 0, text) < 0) return -1;
        } else if (length < 0 && errno == EINTR) {
            continue;
        } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            close(job->output);
            job->output = -1;
        }
    }
    return 0;
}

static void job_receive_state(Job *job) {
    if (job->state < 0) return;
    char discard[64];
    size_t remaining = job->state_used < sizeof(job->result) ? sizeof(job->result) - job->state_used : 0;
    void *destination = remaining ? (char *)&job->result + job->state_used : discard;
    ssize_t length = read(job->state, destination, remaining ? remaining : sizeof(discard));
    if (length > 0) {
        if (remaining) job->state_used += (size_t)length;
    } else if (length == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(job->state);
        job->state = -1;
    }
}

/* ------------------------------------------------------------------ */
/* worker event loop                                                  */
/* ------------------------------------------------------------------ */
void worker_main(Worker *argument) {
    Worker w = *argument;
    free(argument);
    Job *jobs = calloc(MAX_JOBS, sizeof(*jobs));
    if (!jobs) {
        worker_error(&w, "jobs allocation");
        close(w.directory_fd); close(w.request_fd);
        close(w.reply_fd);
        return;
    }
    Job *foreground = NULL;
    int next_id = 1, last_status = 0, alive = 1, stopping = 0;
    Request request;
    size_t request_used = 0;
    char command[COMMAND_SIZE];
    size_t command_used = 0;
    int command_overflow = 0;
    while (alive && !stopping) {
        int active = 0;
        for (int i = 0; i < MAX_JOBS && alive; i++) {
            Job *j = &jobs[i];
            if (!j->used) continue;
            active = 1;
            job_flush_input(j);
            if (job_forward_output(&w, j) < 0) alive = 0;
            job_receive_state(j);
            if (!j->finished) {
                int status;
                /* Reap finished children and detect stopped or continued jobs. */
                pid_t result = waitpid(j->pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
                if (result == j->pid) {
                    if (WIFSTOPPED(status)) {
                        j->stopped = 1;
                        signal_job(j, SIGSTOP);
                        if (job_message(&w, j, "Stopped") == -1) alive = 0;
                        if (foreground == j) {
                            foreground = NULL; last_status = 128 + SIGTSTP;
                            if (reply_send(&w, REPLY_DONE, last_status, NULL) == -1) alive = 0;
                        }
                    } else if (WIFCONTINUED(status)) j->stopped = 0;
                    else {
                        j->finished = 1; j->stopped = 0;
                        j->status = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
                        signal_job(j, SIGKILL);
                    }
                } else if (result == -1 && errno != EINTR) {
                    j->finished = 1; j->status = 1;
                }
            }
            if (j->finished && j->output < 0 && j->state < 0) {
                if (foreground == j) {
                    last_status = j->status; foreground = NULL;
                    if (j->allow_state && shell_result_complete(&j->result, j->state_used)) {
                        w.variables = j->result.variables; j->result.cwd[PATH_MAX - 1] = '\0';
                        if (strcmp(j->result.cwd, w.cwd)) {
                            if (change_directory(&w, j->result.cwd) == -1) {
                                worker_error(&w, "cd");
                                last_status = 1;
                            } else if (reply_send(&w, REPLY_CWD, 0, w.cwd) == -1) alive = 0;
                        }
                        if (j->result.exit_requested) stopping = 1;
                    }
                    if (reply_send(&w, REPLY_DONE, last_status, NULL) == -1) alive = 0;
                } else {
                    char state[64];
                    snprintf(state, sizeof(state), "Done (%d)", j->status);
                    if (job_message(&w, j, state) == -1) alive = 0;
                }
                signal_job(j, SIGKILL);
                atomic_store(&job_registries[w.slot].roots[i], 0);
                job_close(j);
            }
        }
        if (!alive || stopping) break;
        struct pollfd fds[1 + 3 * MAX_JOBS];
        int count = 1;
        fds[0] = (struct pollfd){w.request_fd, POLLIN, 0};
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].used) {
                Job *j = &jobs[i];
                fds[count++] = (struct pollfd){j->output, POLLIN, 0};
                fds[count++] = (struct pollfd){j->state, POLLIN, 0};
                fds[count++] = (struct pollfd){j->input_used ? j->input : -1, POLLOUT, 0};
            }
        /* Sleep until a request or job pipe is ready; periodically reap jobs. */
        if (poll(fds, (nfds_t)count, active ? 100 : -1) == -1) { if (errno == EINTR) continue; break; }
        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        ssize_t n = read(w.request_fd, (char *)&request + request_used, sizeof(request) - request_used);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
        request_used += (size_t)n;
        if (request_used < sizeof(request)) continue;
        request_used = 0; request.text[INPUT_SIZE - 1] = '\0';
        if (request.type == REQUEST_STOP) break;
        if (request.type == REQUEST_INTERRUPT || request.type == REQUEST_SUSPEND) {
            if (foreground) {
                if (request.type == REQUEST_SUSPEND) {
                    reply_send(&w, REPLY_OUTPUT, 0, "^Z\n");

                    signal_job(foreground, SIGSTOP);
                } else {
                    const char *name = foreground->command + strspn(foreground->command, " \t\n");
                    if (strncmp(name, "multiWatch", 10) || (name[10] && !strchr(" \t\n[", name[10])))
                        reply_send(&w, REPLY_OUTPUT, 0, "^C\n");
                    signal_job(foreground, SIGINT);
                }
            }
            continue;
        }
        if (request.type == REQUEST_INPUT || request.type == REQUEST_EOF) {
            Job *j = foreground;
            if (j && j->input >= 0 && !j->input_eof) {
                size_t length = strlen(request.text);
                if (length <= sizeof(j->pending) - j->input_used) {
                    memcpy(j->pending + j->input_used, request.text, length);
                    j->input_used += length;
                } else reply_send(&w, REPLY_OUTPUT, 0, "Input queue full; wait before sending more input.\n");
                if (request.type == REQUEST_EOF) j->input_eof = 1;
            }
            continue;
        }
        if (foreground) continue;
        if (request.type == REQUEST_SET_STATUS) { last_status = 0; reply_send(&w, REPLY_DONE, 0, NULL); continue; }
        if (request.type != REQUEST_FRAGMENT && request.type != REQUEST_COMMAND) continue;
        size_t length = strlen(request.text);
        if (command_used + length >= sizeof(command)) command_overflow = 1;
        else if (!command_overflow) { memcpy(command + command_used, request.text, length); command_used += length; }
        if (request.type == REQUEST_FRAGMENT) continue;
        command[command_used] = '\0'; command_used = 0;
        if (command_overflow) {
            command_overflow = 0;
            reply_send(&w, REPLY_OUTPUT, 0, "terminator: command is too long\n");
            last_status = 2;
            reply_send(&w, REPLY_DONE, last_status, NULL);
            continue;
        }
        if (lexical_completeness(command)) {
            reply_send(&w, REPLY_OUTPUT, 0, "terminator: unsupported or incomplete syntax\n");
            last_status = 2;
            reply_send(&w, REPLY_DONE, last_status, NULL);
            continue;
        }
        int background = remove_background(command);
        char copy[COMMAND_SIZE];
        snprintf(copy, sizeof(copy), "%s", command);
        char *save = NULL, *name = strtok_r(copy, " \t\n", &save);
        if (name && (!strcmp(name, "jobs") || !strcmp(name, "fg") || !strcmp(name, "bg"))) {
            char *spec = strtok_r(NULL, " \t\n", &save), *extra = strtok_r(NULL, " \t\n", &save);
            last_status = 0;
            if (background || extra || (!strcmp(name, "jobs") && spec)) {
                reply_send(&w, REPLY_OUTPUT, 0, "Usage: jobs | fg [%job] | bg [%job] (standalone commands)\n");
                last_status = 2;
            } else if (!strcmp(name, "jobs")) {
                int any = 0;
                for (int i = 0; i < MAX_JOBS; i++)
                    if (jobs[i].used) {
                        any = 1;
                        job_message(&w, &jobs[i], jobs[i].finished ? "Done" : jobs[i].stopped ? "Stopped" : "Running");
                    }
                if (!any) reply_send(&w, REPLY_OUTPUT, 0, "No jobs.\n");
            } else {
                Job *j = find_job(jobs, spec);
                if (!j) {
                    reply_send(&w, REPLY_OUTPUT, 0, "terminator: no such job\n");
                    last_status = 1;
                } else if (!strcmp(name, "bg")) {
                    if (!j->stopped) {
                        reply_send(&w, REPLY_OUTPUT, 0, "terminator: job is already running\n");
                        last_status = 1;
                    } else {
                        j->stopped = 0;
                        signal_job(j, SIGCONT);
                        char text[COMMAND_SIZE + 64];
                        snprintf(text, sizeof(text), "[%d]+ %s &\n", j->id, j->command);
                        reply_send(&w, REPLY_OUTPUT, 0, text);
                    }
                } else {
                    foreground = j; j->stopped = 0;
                    reply_send(&w, REPLY_OUTPUT, 0, j->command);
                    reply_send(&w, REPLY_OUTPUT, 0, "\n");
                    reply_send(&w, REPLY_STARTED, (int)j->group, NULL);
                    signal_job(j, SIGCONT);
                }
            }
            if (!foreground) reply_send(&w, REPLY_DONE, last_status, NULL);
            continue;
        }
        Job *j = NULL;
        for (int i = 0; i < MAX_JOBS; i++)
            if (!jobs[i].used) { j = &jobs[i]; break; }
        if (!j || next_id == INT_MAX) {
            reply_send(&w, REPLY_OUTPUT, 0, "terminator: job limit reached (64 active jobs)\n");
            last_status = 1;
            reply_send(&w, REPLY_DONE, last_status, NULL);
            continue;
        }
        memset(j, 0, sizeof(*j));
        j->output = j->state = j->input = -1; j->groups = &job_registries[w.slot].groups[j - jobs];
        for (int g = 0; g < JOB_GROUPS; g++) atomic_store(&j->groups->groups[g], 0);
        if (!atomic_is_lock_free(&j->groups->groups[0])) {
            reply_send(&w, REPLY_OUTPUT, 0, "terminator: platform lacks lock-free shared job atomics\n");
            job_close(j);
            reply_send(&w, REPLY_DONE, 1, NULL);
            continue;
        }
        peer_job_fd_count = 0;
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].used) {
                int descriptors[] = {jobs[i].output, jobs[i].state, jobs[i].input};
                for (int k = 0; k < 3; k++)
                    if (descriptors[k] >= 0) peer_job_fds[peer_job_fd_count++] = descriptors[k];
            }
        j->pid = spawn_shell(&w, command, last_status, &j->output, &j->state, &j->input, j->groups);
        if (j->pid < 0) {
            worker_error(&w, "fork/pipe");
            job_close(j);
            last_status = 1;
            reply_send(&w, REPLY_DONE, 1, NULL);
            continue;
        }
        j->used = 1; j->id = next_id++;
        j->group = j->pid; j->allow_state = !background;
        snprintf(j->command, sizeof(j->command), "%s", command);
        if (background) {

            close(j->input);
            j->input = -1; j->input_eof = 1;
            char text[COMMAND_SIZE + 80];
            snprintf(text, sizeof(text), "[%d] %ld %s &\n", j->id, (long)j->pid, j->command);
            reply_send(&w, REPLY_OUTPUT, 0, text);
            last_status = 0;
            reply_send(&w, REPLY_DONE, 0, NULL);
        } else {
            foreground = j;
            reply_send(&w, REPLY_STARTED, (int)j->group, NULL);
        }
    }

    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used) signal_job(&jobs[i], SIGKILL);
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used) {
            if (!jobs[i].finished)
                while (waitpid(jobs[i].pid, NULL, 0) == -1 && errno == EINTR) {}
            atomic_store(&job_registries[w.slot].roots[i], 0);
            job_close(&jobs[i]);
        }
    free(jobs); close(w.directory_fd);
    close(w.request_fd); close(w.reply_fd);
}
