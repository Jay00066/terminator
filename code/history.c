/*
 * Maintains command history shared by all tabs.
 * Protects shared data and persistent file updates.
 * Stores recent commands and performs history searches.
 */
#include "terminator.h"

HistorySnapshot *history_snapshots;
History *history_data;
sem_t *history_sem = SEM_FAILED;

/* ------------------------------------------------------------------ */
/* Task 9 - searchable shared history                                 */
/* ------------------------------------------------------------------ */
/* One shared semaphore protects history in both execution modes. */
void history_lock(void) {
    while (sem_wait(history_sem) == -1 && errno == EINTR) {}
}
void history_unlock(void) { sem_post(history_sem); }
static void history_persistence_init(void);

/* ------------------------------------------------------------------ */
/* Execution modes - thread and process workers                       */
/* ------------------------------------------------------------------ */
int history_init(void) {
    history_data = mmap(NULL, sizeof(*history_data), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (history_data == MAP_FAILED) { history_data = NULL; return -1; }
    history_snapshots = mmap(NULL, MAX_TABS * sizeof(*history_snapshots), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (history_snapshots == MAP_FAILED) { history_snapshots = NULL; goto fail; }
    char name[64];
    snprintf(name, sizeof(name), "/terminator-history-%ld", (long)getpid());
    history_sem = sem_open(name, O_CREAT | O_EXCL, 0600, 1);
    if (history_sem == SEM_FAILED) goto fail;
    sem_unlink(name);

    registry_fd = temporary_fd();
    if (registry_fd < 0 || ftruncate(registry_fd, MAX_TABS * sizeof(*job_registries)) < 0) goto fail;
    job_registries = mmap(NULL, MAX_TABS * sizeof(*job_registries), PROT_READ | PROT_WRITE, MAP_SHARED, registry_fd, 0);
    if (job_registries == MAP_FAILED) { job_registries = NULL; goto fail; }
    for (int tab = 0; tab < MAX_TABS; tab++) {
        for (int job = 0; job < MAX_JOBS; job++) {
            atomic_init(&job_registries[tab].roots[job], 0);
            for (int group = 0; group < JOB_GROUPS; group++) atomic_init(&job_registries[tab].groups[job].groups[group], 0);
        }
    }
    history_persistence_init();
    return 0;
fail:;
    int error = errno;
    history_destroy();
    errno = error;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Task 9 - searchable shared history                                 */
/* ------------------------------------------------------------------ */
static char history_path[PATH_MAX];
char history_warning[512];
static int history_file_lock = -1, history_file_dirty, history_stat_known;
static struct stat history_file_stat;
#define HISTORY_MAGIC "# Terminator history v1\n"
const char *history_entry(int chronological_index) {
    return history_data->entries[(history_data->start + chronological_index) % HISTORY_SIZE];
}
static void history_store(History *h, const char *command) {
    int index;
    if (h->count < HISTORY_SIZE) index = (h->start + h->count++) % HISTORY_SIZE;
    else { index = h->start; h->start = (h->start + 1) % HISTORY_SIZE; }
    snprintf(h->entries[index], COMMAND_SIZE, "%s", command);
}
/* Lock the history file when separate Terminator instances update it. */
static int history_file_guard(int lock) {
    if (history_file_lock < 0) { errno = ENOENT; return -1; }
    struct flock operation = {0};
    operation.l_type = lock ? F_WRLCK : F_UNLCK; operation.l_whence = SEEK_SET;
    int result;
    do { result = fcntl(history_file_lock, F_SETLKW, &operation); } while (result < 0 && errno == EINTR);
    return result;
}
static void history_note_error(const char *operation) {
    snprintf(history_warning, sizeof(history_warning), "history: %s: %s; commands remain available in memory.\n", operation,
             strerror(errno));
}
static void history_record_stat(void) { history_stat_known = stat(history_path, &history_file_stat) == 0; }
static int history_file_changed(void) {
    struct stat now;
    if (stat(history_path, &now) < 0) return 1;
    if (!history_stat_known) return 1;
#ifdef __APPLE__
    int same_time = now.st_mtimespec.tv_sec == history_file_stat.st_mtimespec.tv_sec &&
                    now.st_mtimespec.tv_nsec == history_file_stat.st_mtimespec.tv_nsec;
#else
    int same_time = now.st_mtim.tv_sec == history_file_stat.st_mtim.tv_sec && now.st_mtim.tv_nsec == history_file_stat.st_mtim.tv_nsec;
#endif
    return !same_time || now.st_size != history_file_stat.st_size || now.st_ino != history_file_stat.st_ino ||
           now.st_dev != history_file_stat.st_dev;
}
static int history_encode(const char *command, char *out, size_t capacity) {
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)command; *p; p++) {
        char escape = *p == '\n' ? 'n' : *p == '\r' ? 'r' : *p == '\t' ? 't' : *p == '\\' ? '\\' : 0;
        if (used + (escape ? 2 : 1) + 1 > capacity) { errno = EOVERFLOW; return -1; }
        if (escape) {
            out[used++] = '\\'; out[used++] = escape;
        } else out[used++] = (char)*p;
    }
    out[used++] = '\n';
    return (int)used;
}
static int history_decode(const char *record, size_t length, int escaped, char *command) {
    size_t used = 0;
    if (length && record[length - 1] == '\n') length--;
    if (length && record[length - 1] == '\r') length--;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)record[i];
        if (!c) return -1;
        if (escaped && c == '\\') {
            if (++i >= length) return -1;
            c = (unsigned char)record[i];
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c != '\\') return -1;
        }
        if (used >= COMMAND_SIZE - 2) return -1;
        command[used++] = (char)c;
    }
    command[used] = '\0';
    return (int)used;
}

/* ---------- persistent history ---------- */
static int history_file_load(void) {
    int fd = open(history_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) { history_stat_known = 0; return 0; }
        return -1;
    }
    FILE *file = fdopen(fd, "r");
    if (!file) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    History *loaded = calloc(1, sizeof(*loaded));
    if (!loaded) { fclose(file); return -1; }
    char *line = NULL, command[COMMAND_SIZE];
    size_t capacity = 0;
    ssize_t length;
    int first = 1, escaped = 0, result = 0, records = 0, normalize = 0;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        if (first) {
            first = 0;
            if (!strcmp(line, HISTORY_MAGIC)) { escaped = 1; continue; }
        }
        int decoded = history_decode(line, (size_t)length, escaped, command);
        if (decoded > 0) {
            history_store(loaded, command);
            records++;
        } else normalize = 1;
    }
    if (ferror(file)) { result = -1; if (!errno) errno = EIO; }
    int saved = errno;
    if (!result) {
        history_data->count = loaded->count; history_data->start = 0;
        for (int i = 0; i < loaded->count; i++)
            memcpy(history_data->entries[i], loaded->entries[(loaded->start + i) % HISTORY_SIZE], COMMAND_SIZE);
    }
    free(line); free(loaded);
    fclose(file);
    errno = saved;
    if (!result) { history_record_stat(); if (!escaped || normalize || records > HISTORY_SIZE) history_file_dirty = 1; }
    return result;
}

static int history_file_rewrite(void) {
    size_t size = strlen(HISTORY_MAGIC);
    for (int i = 0; i < history_data->count; i++) size += 2 * strlen(history_entry(i)) + 1;
    char *bytes = malloc(size ? size : 1);
    if (!bytes) return -1;
    size_t used = strlen(HISTORY_MAGIC);
    memcpy(bytes, HISTORY_MAGIC, used);
    for (int i = 0; i < history_data->count; i++) {
        int n = history_encode(history_entry(i), bytes + used, size - used);
        if (n < 0) { free(bytes); return -1; }
        used += (size_t)n;
    }
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", history_path) >= (int)sizeof(temporary)) {
        free(bytes);
        errno = ENAMETOOLONG;
        return -1;
    }
    pthread_mutex_lock(&descriptor_mutex);
    int fd = mkstemp(temporary);
    if (fd >= 0 && set_flags(fd, 0) < 0) {
        int e = errno;
        close(fd);
        unlink(temporary);
        fd = -1; errno = e;
    }
    int saved = errno;
    pthread_mutex_unlock(&descriptor_mutex);
    errno = saved;
    if (fd < 0) { free(bytes); return -1; }
    int result = write_all(fd, bytes, used);
    if (!result && fsync(fd) < 0) result = -1;
    saved = errno;
    if (close(fd) < 0 && !result) { result = -1; saved = errno; }
    if (!result && rename(temporary, history_path) < 0) { result = -1; saved = errno; }
    if (result) unlink(temporary);
    free(bytes);
    errno = saved;
    if (!result) { history_file_dirty = 0; history_record_stat(); }
    return result;
}
static int history_file_append(const char *command) {
    char record[2 * COMMAND_SIZE];
    int length = history_encode(command, record, sizeof(record));
    if (length < 0) return -1;
    int fd = open(history_path, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = write_all(fd, record, (size_t)length);
    if (!result && fsync(fd) < 0) result = -1;
    int saved = errno;
    if (close(fd) < 0 && !result) { result = -1; saved = errno; }
    errno = saved;
    if (!result) history_record_stat();
    return result;
}
static void history_persistence_init(void) {
    history_path[0] = history_warning[0] = '\0'; history_file_dirty = history_stat_known = 0;
    const char *override = getenv("TERMINATOR_HISTORY"), *home = getenv("HOME");
    int n = override ? snprintf(history_path, sizeof(history_path), "%s", override)
            : home   ? snprintf(history_path, sizeof(history_path), "%s/.terminator_history", home)
                     : -1;
    if (n < 0 || n >= (int)sizeof(history_path)) { errno = ENAMETOOLONG; history_note_error("history path"); return; }
    char lock_path[PATH_MAX];
    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", history_path) >= (int)sizeof(lock_path)) {
        errno = ENAMETOOLONG;
        history_note_error("lock path");
        return;
    }
    history_file_lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (history_file_lock < 0) { history_note_error("open history lock file"); return; }
    if (history_file_guard(1) < 0) { history_note_error("lock history file"); return; }
    if (history_file_load() < 0 || history_file_rewrite() < 0) history_note_error("load/save history file");
    history_file_guard(0);
}
int history_add(const char *command) {
    if (!command[strspn(command, " \t\r\n")]) return 0;
    history_lock();
    int result = 0, guarded = history_file_guard(1) == 0;
    if (!guarded) {
        history_note_error("lock history file");
        result = -1;
    } else if (!history_file_dirty && history_file_changed() && history_file_load() < 0) {
        history_note_error("reload history file");
        result = -1;
    }
    int full = history_data->count == HISTORY_SIZE;
    history_store(history_data, command);
    if (guarded && !result) {
        if (full || history_file_dirty || !history_stat_known) result = history_file_rewrite();
        else result = history_file_append(command);
        if (result) { history_file_dirty = 1; history_note_error("save history file"); }
    } else history_file_dirty = 1;
    if (guarded) history_file_guard(0);
    history_unlock();
    return result;
}

void history_snapshot(int slot) {
    history_lock();
    size_t used = 0;
    char *text = history_snapshots[slot].text;
    text[0] = '\0';
    int first = history_data->count > HISTORY_DISPLAY_SIZE ? history_data->count - HISTORY_DISPLAY_SIZE : 0;
    for (int i = first; i < history_data->count; i++) {
        int n = snprintf(text + used, HISTORY_TEXT_SIZE - used, "%d  %s\n", i + 1, history_entry(i));
        if (n < 0 || (size_t)n >= HISTORY_TEXT_SIZE - used) break;
        used += (size_t)n;
    }
    history_unlock();
}
void history_destroy(void) {
    if (history_file_lock >= 0) close(history_file_lock);
    if (registry_fd >= 0) close(registry_fd);
    if (history_sem != SEM_FAILED) sem_close(history_sem);
    if (history_data) munmap(history_data, sizeof(*history_data));
    if (history_snapshots) munmap(history_snapshots, MAX_TABS * sizeof(*history_snapshots));
    if (job_registries) munmap(job_registries, MAX_TABS * sizeof(*job_registries));
    history_file_lock = registry_fd = -1; history_sem = SEM_FAILED;
    history_data = NULL; history_snapshots = NULL;
    job_registries = NULL;
}

void history_refresh(void) {
    if (!history_file_dirty && history_file_guard(1) == 0) {
        if (history_file_changed() && history_file_load() < 0) history_note_error("reload history for search");
        history_file_guard(0);
    }
}

typedef struct {
    int next[256], link, length;
} HistorySearchState;
/* Index the term so substring matching scans each stored command once. */
static HistorySearchState *history_search_index(const char *term, int *state_count) {
    size_t length = strlen(term);
    HistorySearchState *states = malloc((2 * length + 1) * sizeof(*states));
    if (!states) return NULL;
    memset(states[0].next, 0xff, sizeof(states[0].next));
    states[0].link = -1; states[0].length = 0;
    int count = 1, last = 0;
    for (const unsigned char *p = (const unsigned char *)term; *p; p++) {
        int current = count++;
        memset(states[current].next, 0xff, sizeof(states[current].next));
        states[current].length = states[last].length + 1; states[current].link = 0;
        int parent = last;
        while (parent >= 0 && states[parent].next[*p] < 0) {
            states[parent].next[*p] = current; parent = states[parent].link;
        }
        if (parent >= 0) {
            int q = states[parent].next[*p];
            if (states[parent].length + 1 == states[q].length) states[current].link = q;
            else {
                int clone = count++;
                states[clone] = states[q]; states[clone].length = states[parent].length + 1;
                while (parent >= 0 && states[parent].next[*p] == q) {
                    states[parent].next[*p] = clone; parent = states[parent].link;
                }
                states[q].link = states[current].link = clone;
            }
        }
        last = current;
    }
    *state_count = count;
    return states;
}
static int history_substring_length(const HistorySearchState *states, const char *command) {
    int state = 0, length = 0, best = 0;
    for (const unsigned char *p = (const unsigned char *)command; *p; p++) {
        while (state && states[state].next[*p] < 0) { state = states[state].link; length = states[state].length; }
        if (states[state].next[*p] >= 0) {
            state = states[state].next[*p];
            length++;
        } else {
            state = 0; length = 0;
        }
        if (length > best) best = length;
    }
    return best;
}
/* Prefer an exact match; otherwise show every best substring match. */
void history_search(Tab *t, const char *term) {
    history_lock();

    history_refresh();
    if (*term)
        for (int i = history_data->count - 1; i >= 0; i--) {
            if (!strcmp(history_entry(i), term)) {
                append_output(t, history_entry(i));
                append_output(t, "\n");
                history_unlock();
                return;
            }
        }
    int state_count = 0;
    HistorySearchState *index = *term ? history_search_index(term, &state_count) : NULL;
    (void)state_count;
    if (*term && !index) { remember_line(t, "history: insufficient memory for search."); history_unlock(); return; }
    int best = 2, lengths[HISTORY_SIZE];
    for (int i = 0; i < history_data->count; i++) {
        lengths[i] = index ? history_substring_length(index, history_entry(i)) : 0;
        if (lengths[i] > best) best = lengths[i];
    }
    if (best > 2) {
        for (int i = history_data->count - 1; i >= 0; i--)
            if (lengths[i] == best) { append_output(t, history_entry(i)); append_output(t, "\n"); }
    } else remember_line(t, "No match for search term in history");
    free(index);
    history_unlock();
}
