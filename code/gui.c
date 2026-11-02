/*
 * Creates and redraws the X11 window and independent tabs.
 * Handles input, hotkeys, clipboard, search and completion.
 * Sends commands to workers and displays their output.
 */
#include "terminator.h"

#ifndef TERMINATOR_HEADLESS_TEST
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#endif

/* ------------------------------------------------------------------ */
/* Task 1 - X11 GUI and tabs                                          */
/* ------------------------------------------------------------------ */
static void *worker_thread_entry(void *argument) {
    worker_main(argument);
    return NULL;
}
static Tab tabs[MAX_TABS];
static int active_tab;

enum { TEXT_LEFT = 10, OUTPUT_TOP = 55, OUTPUT_BASELINE = 70, ROW_HEIGHT = 20, BOTTOM_SPACE = 100 };

static int visible_rows(int height) {
    int rows = (height - BOTTOM_SPACE) / ROW_HEIGHT;
    return rows > 0 ? rows : 0;
}

static int first_visible_line(const Tab *tab, int visible) {
    int first = tab->line_count - tab->scroll - visible;
    return first > 0 ? first : 0;
}

/* ------------------------------------------------------------------ */
/* Task 7 - hotkeys: editing, config and clipboard                    */
/* ------------------------------------------------------------------ */
static void editor_history_reset(Tab *t) {
    free(t->browse_history);
    t->browse_history = NULL; t->history_count = 0;
    t->history_position = 0; t->draft[0] = '\0';
}
static void editor_reset(Tab *t) {
    completion_clear(t);
    t->input_length = 0; t->cursor = 0;
    t->input[0] = '\0';
    editor_history_reset(t);
}
static int editor_insert(Tab *t, const char *text, size_t size) {
    completion_clear(t);
    if ((t->search_mode ? 0 : t->continued_used) + (size_t)t->input_length + size > COMMAND_SIZE - 2) return -1;
    if (t->cursor < 0) t->cursor = 0;
    if (t->cursor > t->input_length) t->cursor = t->input_length;
    memmove(t->input + t->cursor + size, t->input + t->cursor, (size_t)(t->input_length - t->cursor) + 1);
    memcpy(t->input + t->cursor, text, size);
    t->cursor += (int)size; t->input_length += (int)size;
    return 0;
}
static void editor_delete(Tab *t, int start, int end) {
    completion_clear(t);
    if (start < 0) start = 0;
    if (end > t->input_length) end = t->input_length;
    if (start >= end) return;
    memmove(t->input + start, t->input + end, (size_t)(t->input_length - end) + 1);
    t->input_length -= end - start;
    if (t->cursor >= end) t->cursor -= end - start;
    else if (t->cursor > start) t->cursor = start;
}
static void editor_previous_word(Tab *t) {
    int start = t->cursor;
    while (start > 0 && strchr(" \t\n", t->input[start - 1])) start--;
    while (start > 0 && !strchr(" \t\n", t->input[start - 1])) start--;
    editor_delete(t, start, t->cursor);
}

static void editor_history(Tab *t, int direction) {
    if (t->busy || t->search_mode) return;
    completion_clear(t);
    if (!t->browse_history) {
        if (direction > 0) return;
        history_lock();
        int count = history_data->count;
        if (count > HISTORY_DISPLAY_SIZE) count = HISTORY_DISPLAY_SIZE;
        char (*copy)[COMMAND_SIZE] = count ? malloc((size_t)count * sizeof(*copy)) : NULL;
        if (copy)
            for (int i = 0; i < count; i++) memcpy(copy[i], history_entry(history_data->count - count + i), sizeof(*copy));
        history_unlock();
        if (!copy) return;
        t->browse_history = copy; t->history_count = count;
        t->history_position = count;
        snprintf(t->draft, sizeof(t->draft), "%s", t->input);
    }
    int next = t->history_position + direction;
    if (next < 0 || next > t->history_count) return;
    t->history_position = next;
    const char *text = next == t->history_count ? t->draft : t->browse_history[next];
    snprintf(t->input, sizeof(t->input), "%s", text);
    t->input_length = (int)strlen(t->input); t->cursor = t->input_length;
}
/* ------------------------------------------------------------------ */
/* Task 1 - X11 GUI and tabs                                          */
/* ------------------------------------------------------------------ */
static char *screen_line(Tab *t, int row) { return t->lines[(t->line_start + row) % MAX_LINES]; }
static void screen_clear(Tab *t) {
    t->line_count = t->line_start = t->line_length = 0; t->partial = t->scroll = t->selection_active = 0;
}
/* A shell notification starts on a fresh output line. */
void remember_line(Tab *t, const char *text) {
    if (t->line_count == MAX_LINES) {
        t->line_start = (t->line_start + 1) % MAX_LINES;
        t->line_count--;
        if (t->selection_active) {
            if (t->anchor_line == 0 || t->end_line == 0) t->selection_active = 0;
            else { t->anchor_line--; t->end_line--; }
        }
    }
    char *line = screen_line(t, t->line_count++);
    snprintf(line, LINE_SIZE, "%s", text);
    t->line_length = (int)strlen(line);
}
/* ---------- output routing ---------- */
void append_output(Tab *t, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (!t->partial || !t->line_count) { remember_line(t, ""); t->partial = 1; }
        if (*p == '\n') { t->partial = 0; continue; }
        if ((*p < 32 && *p != '\t') || *p == 127) continue;
        if (t->line_length == LINE_SIZE - 1) remember_line(t, "");
        char *line = screen_line(t, t->line_count - 1);
        line[t->line_length++] = *p == '\t' ? ' ' : (char)*p; line[t->line_length] = '\0';
    }
}

static void history_search_begin(Tab *t) {
    if (t->busy || t->search_mode) return;
    completion_clear(t);
    memmove(t->search_saved, t->input, (size_t)t->input_length + 1);
    t->search_saved_cursor = t->cursor; t->search_mode = 1;
    t->input[0] = '\0'; t->input_length = t->cursor = 0;
    t->selection_active = 0; t->scroll = 0;
}
static void history_search_restore(Tab *t) {
    memmove(t->input, t->search_saved, strlen(t->search_saved) + 1);
    t->input_length = (int)strlen(t->input); t->cursor = t->search_saved_cursor;
    t->search_mode = 0;
}
static void history_search_submit(Tab *t) {
    t->partial = 0;
    append_output(t, "Enter search term: ");
    append_output(t, t->input);
    append_output(t, "\n");
    history_search(t, t->input);
    history_search_restore(t);
    t->partial = 0; t->scroll = 0;
}

/* ------------------------------------------------------------------ */
/* Execution modes - thread and process workers                       */
/* ------------------------------------------------------------------ */
static int flush_requests(Tab *t) {

    while (t->queue_count) {
        Request *r = &t->outgoing[t->queue_head];
        ssize_t n = write(t->request_fd, r, sizeof(*r));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n != (ssize_t)sizeof(*r)) return -1;
        t->queue_head = (t->queue_head + 1) % QUEUE_SIZE;
        t->queue_count--;
    }
    return 0;
}
static int request_send(Tab *t, int type, const char *text) {
    size_t length = text ? strlen(text) : 0;
    int frames = (int)(length / (INPUT_SIZE - 1)) + 1;
    if (type != REQUEST_COMMAND && type != REQUEST_INPUT && frames != 1) { errno = EMSGSIZE; return -1; }
    if (frames > QUEUE_SIZE - t->queue_count) { errno = EAGAIN; return -1; }
    errno = 0;
    long limit = fpathconf(t->request_fd, _PC_PIPE_BUF);
    if (limit == -1 && errno) return -1;
    if (limit != -1 && sizeof(Request) > (size_t)limit) { errno = EMSGSIZE; return -1; }
    size_t offset = 0;
    for (int i = 0; i < frames; i++) {
        int index = (t->queue_head + t->queue_count) % QUEUE_SIZE;
        Request *r = &t->outgoing[index];
        memset(r, 0, sizeof(*r));
        r->type = type == REQUEST_COMMAND && i != frames - 1 ? REQUEST_FRAGMENT : type;
        size_t chunk = length - offset;
        if (chunk > INPUT_SIZE - 1) chunk = INPUT_SIZE - 1;
        if (chunk) memcpy(r->text, text + offset, chunk);
        offset += chunk;
        t->queue_count++;
    }
    return flush_requests(t);
}
/* Each tab gets its own worker and command/reply pipes. */
static int new_tab(void) {
    int slot;
    for (slot = 0; slot < MAX_TABS && tabs[slot].used; slot++) {}
    if (slot == MAX_TABS) { errno = ENOSPC; return -1; }
    int commands[2], replies[2];
    if (make_pipe(commands, 1) == -1) return -1;
    if (make_pipe(replies, 1) == -1) { close(commands[0]); close(commands[1]); return -1; }
    if (set_flags(commands[1], 1) == -1) {
        close(commands[0]); close(commands[1]);
        close(replies[0]); close(replies[1]);
        return -1;
    }
    Worker *w = calloc(1, sizeof(*w));
    if (!w) {
        close(commands[0]); close(commands[1]);
        close(replies[0]); close(replies[1]);
        return -1;
    }
    w->request_fd = commands[0]; w->reply_fd = replies[1];
    w->slot = slot;
    snprintf(w->cwd, sizeof(w->cwd), "%s", initial_cwd);
    w->directory_fd = open(initial_cwd, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (w->directory_fd == -1 || set_flags(w->directory_fd, 0) == -1) {
        if (w->directory_fd >= 0) close(w->directory_fd);
        free(w); close(commands[0]);
        close(commands[1]); close(replies[0]);
        close(replies[1]);
        return -1;
    }
    static unsigned long generation;
    Tab *t = &tabs[slot];
    memset(t, 0, sizeof(*t));
    t->generation = ++generation; t->request_fd = commands[1];
    t->reply_fd = replies[0];
    snprintf(t->cwd, sizeof(t->cwd), "%s", initial_cwd);
    if (process_mode) {
        pid_t pid = fork();
        if (pid == 0) {

            for (long fd = 3; fd < descriptor_limit; fd++)
                if (fd != w->request_fd && fd != w->reply_fd && fd != w->directory_fd) close((int)fd);
            gui_fd = -1;
            worker_main(w);
            _exit(0);
        }
        if (pid < 0) {
            close(w->directory_fd); free(w);
            close(commands[0]); close(commands[1]);
            close(replies[0]); close(replies[1]);
            return -1;
        }
        t->worker_pid = pid;
        close(w->directory_fd); free(w);
        close(commands[0]); close(replies[1]);
    } else {
        int error = pthread_create(&t->worker_thread, NULL, worker_thread_entry, w);
        if (error) {
            close(w->directory_fd); free(w);
            close(commands[0]); close(commands[1]);
            close(replies[0]); close(replies[1]);
            errno = error;
            return -1;
        }
    }
    t->used = 1; active_tab = slot;
    return slot;
}
static void close_tab(int slot) {
    Tab *t = &tabs[slot];
    if (!t->used) return;

    request_send(t, REQUEST_STOP, NULL);
    close(t->request_fd);

    set_flags(t->reply_fd, 1);

    for (int j = 0; j < MAX_JOBS; j++) {
        int root = atomic_load(&job_registries[slot].roots[j]);
        if (root > 0) kill(-root, SIGKILL);
        for (int g = 0; g < JOB_GROUPS; g++) {
            int group = atomic_load(&job_registries[slot].groups[j].groups[g]);
            if (group > 0) kill(-group, SIGKILL);
        }
    }

    char discard[8192];
    for (;;) {
        ssize_t n = read(t->reply_fd, discard, sizeof(discard));
        if (n > 0) continue;
        if (n == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd p = {t->reply_fd, POLLIN, 0};
            poll(&p, 1, 100);
            continue;
        }
        break;
    }
    close(t->reply_fd);
    if (process_mode) {
        while (waitpid(t->worker_pid, NULL, 0) == -1 && errno == EINTR) {}
    } else pthread_join(t->worker_thread, NULL);
    for (int j = 0; j < MAX_JOBS; j++) {
        atomic_store(&job_registries[slot].roots[j], 0);
        for (int g = 0; g < JOB_GROUPS; g++) atomic_store(&job_registries[slot].groups[j].groups[g], 0);
    }
    completion_clear(t);
    free(t->browse_history);
    memset(t, 0, sizeof(*t));
    if (active_tab == slot) {
        for (int i = 0; i < MAX_TABS; i++)
            if (tabs[i].used) { active_tab = i; break; }
    }
}
static int tab_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_TABS; i++) count += tabs[i].used;
    return count;
}
/* Route worker replies to their own tab, including hidden tabs. */
static int collect_replies(int slot) {
    Tab *t = &tabs[slot];
    int changed = 0;
    if (flush_requests(t) == -1) { close_tab(slot); return 1; }

    for (int batch = 0; batch < 16; batch++) {
        ssize_t n = read(t->reply_fd, (char *)&t->incoming + t->incoming_used, sizeof(t->incoming) - t->incoming_used);
        if (n == 0) { close_tab(slot); return 1; }
        if (n == -1) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) { close_tab(slot); return 1; }
            break;
        }
        t->incoming_used += (size_t)n;
        if (t->incoming_used < sizeof(t->incoming)) continue;
        Reply *r = &t->incoming;
        r->text[PAYLOAD_SIZE - 1] = '\0';
        if (r->type == REPLY_OUTPUT) append_output(t, r->text);
        else if (r->type == REPLY_CWD) memcpy(t->cwd, r->text, sizeof(t->cwd));
        else if (r->type == REPLY_STARTED) t->command_group = (pid_t)r->value;
        else if (r->type == REPLY_DONE) {
            t->busy = 0; t->status = r->value;
            t->partial = 0; t->command_group = -1;
        }
        t->incoming_used = 0; changed = 1;
    }
    return changed;
}
#ifndef TERMINATOR_HEADLESS_TEST
static int bindings_command(Tab *t, const char *command);
#endif
/* Keep incomplete commands until quoting and syntax are complete. */
static void submit(Tab *t) {
    if (t->busy || (!t->input_length && !t->continued_used)) return;
    char command[COMMAND_SIZE];
    size_t length = t->continued_used + (size_t)t->input_length;
    if (length >= sizeof(command) - 1) {
        remember_line(t, "Command limit: 8190 characters. Ctrl+C cancels input.");
        return;
    }
    memcpy(command, t->continued, t->continued_used);
    memcpy(command + t->continued_used, t->input, (size_t)t->input_length + 1);
#ifndef TERMINATOR_HEADLESS_TEST
    if (!t->continued_used && bindings_command(t, command)) { editor_reset(t); return; }
#endif
    int completeness = command_completeness(command);
    char line[COMMAND_SIZE + 64];
    snprintf(line, sizeof(line), "%s%s", t->continued_used ? "> " : "user@terminator> ", t->input);
    t->partial = 0;
    append_output(t, line);
    append_output(t, "\n");
    t->scroll = 0;
    if (completeness == 1) {
        memcpy(t->continued, command, length);
        t->continued[length] = '\n'; t->continued[length + 1] = '\0';
        t->continued_used = length + 1;
        editor_reset(t);
        return;
    }
    if (history_add(command) < 0) append_output(t, history_warning);
    history_snapshot((int)(t - tabs));
    if (request_send(t, REQUEST_COMMAND, command) == 0) t->busy = 1;
    else {
        snprintf(line, sizeof(line), "Could not send command: %s", strerror(errno));
        remember_line(t, line);
        return;
    }
    editor_reset(t);
    t->continued_used = 0; t->continued[0] = '\0';
}

#ifndef TERMINATOR_HEADLESS_TEST

/* ------------------------------------------------------------------ */
/* Task 7 - hotkeys: defaults, config and `bindings` builtin          */
/* ------------------------------------------------------------------ */
enum { HK_LEFT, HK_RIGHT, HK_START, HK_END, HK_PREVIOUS, HK_NEXT, HK_KILL_START, HK_KILL_END,
       HK_WORD, HK_CLEAR, HK_EOF, HK_COPY, HK_PASTE, HK_NEW_TAB, HK_CLOSE_TAB, HK_NEXT_TAB,
       HK_PREV_TAB, HK_INTERRUPT, HK_SUSPEND, HK_PAGE_UP, HK_PAGE_DOWN, HK_BACKSPACE,
       HK_DELETE, HK_ENTER, HK_SEARCH, HK_COMPLETE, HK_COUNT };
typedef struct { const char *name, *default_text; KeySym key; unsigned modifiers; } Binding;
static Binding bindings[HK_COUNT] = {
    {"left",                "Left",             0, 0},
    {"right",               "Right",            0, 0},
    {"line_start",          "Ctrl+A",           0, 0},
    {"line_end",            "Ctrl+E",           0, 0},
    {"history_previous",    "Up",               0, 0},
    {"history_next",        "Down",             0, 0},
    {"delete_to_start",     "Ctrl+U",           0, 0},
    {"delete_to_end",       "Ctrl+K",           0, 0},
    {"delete_word",         "Ctrl+W",           0, 0},
    {"clear_screen",        "Ctrl+L",           0, 0},
    {"eof",                 "Ctrl+D",           0, 0},
    {"copy",                "Ctrl+Shift+C",     0, 0},
    {"paste",               "Ctrl+Shift+V",     0, 0},
    {"new_tab",             "Ctrl+T",           0, 0},
    {"close_tab",           "Ctrl+Shift+W",     0, 0},
    {"next_tab",            "Ctrl+Tab",         0, 0},
    {"previous_tab",        "Ctrl+Shift+Tab",   0, 0},
    {"interrupt",           "Ctrl+C",           0, 0},
    {"suspend",             "Ctrl+Z",           0, 0},
    {"page_up",             "Page_Up",          0, 0},
    {"page_down",           "Page_Down",        0, 0},
    {"backspace",           "BackSpace",        0, 0},
    {"delete",              "Delete",           0, 0},
    {"submit",              "Return",           0, 0},
    {"history_search",      "Ctrl+R",           0, 0},
    {"completion",          "Tab",              0, 0},
};
static char binding_messages[8192];
static int binding_errors;
static void binding_error(const char *text) {
    size_t used = strlen(binding_messages);
    snprintf(binding_messages + used, sizeof(binding_messages) - used, "bindings: %s\n", text);
    binding_errors++;
}
static char *trim_space(char *text) {
    text += strspn(text, " \t\r\n");
    size_t n = strlen(text);
    while (n && strchr(" \t\r\n", text[n - 1])) text[--n] = '\0';
    return text;
}
static KeySym normalized_key(KeySym key) {
    if (key == XK_ISO_Left_Tab) return XK_Tab;
    if (key == XK_KP_Enter) return XK_Return;
    KeySym lower, upper;
    XConvertCase(key, &lower, &upper);
    return lower;
}
static int binding_parse(const char *spec, KeySym *key, unsigned *modifiers) {
    if (!*spec || strlen(spec) >= 128 || strstr(spec, "++") || spec[strlen(spec) - 1] == '+') return -1;
    char text[128];
    snprintf(text, sizeof(text), "%s", spec);
    char *save = NULL;
    *modifiers = 0;
    *key = NoSymbol;
    for (char *part = strtok_r(text, "+", &save); part; part = strtok_r(NULL, "+", &save)) {
        part = trim_space(part);
        unsigned mask = 0;
        if (!strcmp(part, "Ctrl") || !strcmp(part, "Control")) mask = ControlMask;
        else if (!strcmp(part, "Shift")) mask = ShiftMask;
        else if (!strcmp(part, "Alt")) mask = Mod1Mask;
        else if (!strcmp(part, "Super")) mask = Mod4Mask;
        if (mask) {
            if (*key != NoSymbol || (*modifiers & mask)) return -1;
            *modifiers |= mask;
        } else {
            if (*key != NoSymbol) return -1;
            *key = normalized_key(XStringToKeysym(part));
            if (*key == NoSymbol || IsModifierKey(*key)) return -1;
        }
    }
    if (*key == NoSymbol) return -1;

    if (*key >= 32 && *key <= 126 && !(*modifiers & (ControlMask | Mod1Mask | Mod4Mask))) return -1;
    return 0;
}
/* Load saved shortcuts; invalid or conflicting entries use defaults. */
static void bindings_load(void) {
    binding_messages[0] = '\0'; binding_errors = 0;
    int custom[HK_COUNT] = {0}, seen[HK_COUNT] = {0};
    for (int i = 0; i < HK_COUNT; i++) binding_parse(bindings[i].default_text, &bindings[i].key, &bindings[i].modifiers);
    const char *home = getenv("HOME"), *override = getenv("TERMINATOR_CONFIG");
    char path[PATH_MAX];
    int path_length = override ? snprintf(path, sizeof(path), "%s", override)
                      : home   ? snprintf(path, sizeof(path), "%s/.terminatorrc", home)
                               : -1;
    if (path_length < 0 || path_length >= (int)sizeof(path)) {
        binding_error("Cannot locate ~/.terminatorrc; using defaults.");
        return;
    }
    int config_fd = open(path, O_RDONLY | O_CLOEXEC);
    FILE *file = config_fd < 0 ? NULL : fdopen(config_fd, "r");
    if (!file) {
        int saved = errno;
        if (config_fd >= 0) close(config_fd);
        errno = saved;
        if (errno != ENOENT) binding_error("Cannot read ~/.terminatorrc; using defaults.");
        return;
    }
    char *line = NULL;
    size_t capacity = 0;
    int line_number = 0;
    while (getline(&line, &capacity, file) >= 0) {
        line_number++;
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *name = trim_space(line);
        if (!*name) continue;
        char *equal = strchr(name, '=');
        char error[256];
        if (!equal) {
            snprintf(error, sizeof(error), "line %d: expected action=key.", line_number);
            binding_error(error);
            continue;
        }
        *equal = '\0';
        name = trim_space(name);
        char *value = trim_space(equal + 1);
        int action;
        for (action = 0; action < HK_COUNT; action++)
            if (!strcmp(name, bindings[action].name)) break;
        if (action == HK_COUNT) {
            snprintf(error, sizeof(error), "line %d: unknown action %.100s.", line_number, name);
            binding_error(error);
            continue;
        }
        KeySym key;
        unsigned modifiers;
        if (seen[action] || binding_parse(value, &key, &modifiers) == -1) {
            snprintf(error, sizeof(error), "line %d: invalid/duplicate %.80s; using %s.", line_number, name, bindings[action].default_text);
            binding_error(error);
            custom[action] = 0;
            binding_parse(bindings[action].default_text, &bindings[action].key, &bindings[action].modifiers);
        } else {
            bindings[action].key = key; bindings[action].modifiers = modifiers;
            custom[action] = 1;
        }
        seen[action] = 1;
    }
    if (ferror(file)) binding_error("Error reading config file; some entries may not have been loaded.");
    free(line); fclose(file);

    for (int pass = 0; pass < HK_COUNT; pass++) {
        int bad[HK_COUNT] = {0}, changed = 0;
        for (int i = 0; i < HK_COUNT; i++)
            for (int j = i + 1; j < HK_COUNT; j++)
                if (bindings[i].key == bindings[j].key && bindings[i].modifiers == bindings[j].modifiers) {
                    if (custom[i]) bad[i] = 1;
                    if (custom[j]) bad[j] = 1;
                }
        for (int i = 0; i < HK_COUNT; i++)
            if (bad[i]) {
                char error[200];
                snprintf(error, sizeof(error), "conflict for %s; using %s.", bindings[i].name, bindings[i].default_text);
                binding_error(error);
                custom[i] = 0; changed = 1;
                binding_parse(bindings[i].default_text, &bindings[i].key, &bindings[i].modifiers);
            }
        if (!changed) break;
    }
}
static int binding_match(KeySym key, unsigned state) {
    if (key == XK_ISO_Left_Tab) state |= ShiftMask;
    key = normalized_key(key);
    state &= ControlMask | ShiftMask | Mod1Mask | Mod4Mask;
    for (int i = 0; i < HK_COUNT; i++)
        if (bindings[i].key == key && bindings[i].modifiers == state) return i;
    return -1;
}
static int bindings_command(Tab *t, const char *command) {
    char text[COMMAND_SIZE];
    snprintf(text, sizeof(text), "%s", command);
    char *value = trim_space(text);
    if (strcmp(value, "bindings") && strcmp(value, "bindings reload")) return 0;
    remember_line(t, value);
    t->partial = 0;
    if (!strcmp(value, "bindings reload")) {
        bindings_load();
        append_output(t, binding_messages);
        remember_line(t, binding_errors ? "Bindings reloaded with fallback warnings." : "Bindings reloaded.");
    } else
        for (int i = 0; i < HK_COUNT; i++) {
            char line[200];
            unsigned mods = bindings[i].modifiers;
            snprintf(line, sizeof(line), "%s=%s%s%s%s%s", bindings[i].name, mods & ControlMask ? "Ctrl+" : "",
                     mods & ShiftMask ? "Shift+" : "", mods & Mod1Mask ? "Alt+" : "", mods & Mod4Mask ? "Super+" : "",
                     XKeysymToString(bindings[i].key));
            remember_line(t, line);
        }
    if (history_add(command) < 0) append_output(t, history_warning);
    t->status = binding_errors ? 2 : 0;
    request_send(t, REQUEST_SET_STATUS, NULL);
    return 1;
}

/* ---------- X11 clipboard transfers ---------- */
typedef struct {
    Window window;
    Atom property, target;
    char *text;
    size_t length, offset;
    long long deadline;
} ClipboardSend;
static ClipboardSend clipboard_sends[8];
static Atom atom_clipboard, atom_utf8, atom_targets, atom_text, atom_incr, atom_paste;
static char *clipboard_text;
static int paste_pending, paste_tab, paste_incr, paste_overflow, paste_fallback;
static unsigned long paste_generation;
static long long paste_deadline;
static char paste_buffer[COMMAND_SIZE];
static size_t paste_used;
static int selection_dragging;
static void selection_bounds(const Tab *t, int *a, int *ac, int *b, int *bc) {
    *a = t->anchor_line;
    *ac = t->anchor_col;
    *b = t->end_line;
    *bc = t->end_col;
    if (*a > *b || (*a == *b && *ac > *bc)) {
        int v = *a;
        *a = *b;
        *b = v;
        v = *ac;
        *ac = *bc;
        *bc = v;
    }
}
static char *selection_text(Tab *t) {
    if (!t->selection_active && t->input_length) return strdup(t->input);
    int a = 0, ac = 0, b = t->line_count - 1, bc = b >= 0 ? (int)strlen(screen_line(t, b)) : 0;
    if (t->selection_active) selection_bounds(t, &a, &ac, &b, &bc);
    if (a < 0 || b >= t->line_count || b < 0) return strdup("");
    size_t capacity = (size_t)(b - a + 1) * (LINE_SIZE + 1) + 1;
    char *text = malloc(capacity);
    if (!text) return NULL;
    size_t used = 0;
    for (int row = a; row <= b; row++) {
        int length = (int)strlen(screen_line(t, row));
        int start = row == a ? ac : 0, end = row == b ? bc : length;
        if (start > length) start = length;
        if (end > length) end = length;
        if (start < 0) start = 0;
        if (end < start) end = start;
        memcpy(text + used, screen_line(t, row) + start, (size_t)(end - start));
        used += (size_t)(end - start);
        if (row < b) text[used++] = '\n';
    }
    text[used] = '\0';
    return text;
}
static void clipboard_copy(Display *d, Window window, Tab *t, Time time, int primary_only) {
    char *text = selection_text(t);
    if (!text) { remember_line(t, "Copy failed: out of memory."); return; }
    free(clipboard_text);
    clipboard_text = text;
    XSetSelectionOwner(d, XA_PRIMARY, window, time);
    if (!primary_only) XSetSelectionOwner(d, atom_clipboard, window, time);
}
static void clipboard_request(Display *d, Window window, Tab *t, Time time) {
    if (paste_pending) { remember_line(t, "A clipboard request is already pending."); return; }
    if (XGetSelectionOwner(d, atom_clipboard) == None) { remember_line(t, "Clipboard is empty."); return; }
    paste_pending = 1; paste_tab = (int)(t - tabs);
    paste_generation = t->generation; paste_used = 0;
    paste_incr = 0; paste_overflow = 0;
    paste_fallback = 0; paste_deadline = watch_now() + 10000;
    XDeleteProperty(d, window, atom_paste);
    XConvertSelection(d, atom_clipboard, atom_utf8, atom_paste, window, time);
}
static void paste_complete(void) {
    if (paste_tab >= 0 && paste_tab < MAX_TABS && tabs[paste_tab].used && tabs[paste_tab].generation == paste_generation) {
        Tab *t = &tabs[paste_tab];
        if (paste_overflow) remember_line(t, "Paste rejected: unsupported text or input limit exceeded.");
        else {
            char clean[COMMAND_SIZE];
            size_t used = 0;
            int invalid = 0;
            for (size_t i = 0; i < paste_used; i++) {
                unsigned char c = (unsigned char)paste_buffer[i];
                if (c == '\r') { if (i + 1 < paste_used && paste_buffer[i + 1] == '\n') i++; c = '\n'; }
                if ((c >= 32 && c <= 126) || c == '\t' || c == '\n') clean[used++] = (char)c;
                else invalid = 1;
            }
            if (invalid || editor_insert(t, clean, used) == -1)
                remember_line(t, "Paste rejected: use ASCII text within the 8190-byte command limit.");
        }
    }
    paste_pending = 0; paste_incr = 0;
    paste_used = 0;
}
static int clipboard_read(Display *d, Window window) {
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;
    int result = XGetWindowProperty(d, window, atom_paste, 0, COMMAND_SIZE, False, AnyPropertyType, &type, &format, &count, &after, &data);
    if (result != Success) { if (data) XFree(data); paste_overflow = 1; return -1; }
    if (type == atom_incr) {
        paste_incr = 1;
        XFree(data);
        XDeleteProperty(d, window, atom_paste);
        return 1;
    }
    if (format != 8 || (type != atom_utf8 && type != XA_STRING && type != atom_text)) paste_overflow = 1;
    if (after || count > sizeof(paste_buffer) - 1 - paste_used) paste_overflow = 1;
    if (!paste_overflow && count) { memcpy(paste_buffer + paste_used, data, count); paste_used += count; }
    if (data) XFree(data);
    XDeleteProperty(d, window, atom_paste);
    return count ? 1 : 0;
}
static void clipboard_reply(Display *d, XSelectionRequestEvent *request) {
    XEvent reply = {0};
    reply.xselection.type = SelectionNotify; reply.xselection.display = d;
    reply.xselection.requestor = request->requestor; reply.xselection.selection = request->selection;
    reply.xselection.target = request->target; reply.xselection.time = request->time;
    reply.xselection.property = None;
    Atom property = request->property == None ? request->target : request->property;
    if (clipboard_text) {
        if (request->target == atom_targets) {
            Atom targets[] = {atom_targets, atom_utf8, XA_STRING, atom_text};
            XChangeProperty(d, request->requestor, property, XA_ATOM, 32, PropModeReplace, (unsigned char *)targets, 4);
            reply.xselection.property = property;
        } else if (request->target == atom_utf8 || request->target == XA_STRING || request->target == atom_text) {
            size_t length = strlen(clipboard_text);
            if (length <= 32768) {
                XChangeProperty(d, request->requestor, property, request->target, 8, PropModeReplace, (unsigned char *)clipboard_text,
                                (int)length);
                reply.xselection.property = property;
            } else
                for (int i = 0; i < 8; i++)
                    if (!clipboard_sends[i].text) {
                        ClipboardSend *s = &clipboard_sends[i];
                        s->text = strdup(clipboard_text);
                        if (!s->text) break;
                        s->window = request->requestor; s->property = property;
                        s->target = request->target; s->length = length;
                        s->offset = 0; s->deadline = watch_now() + 10000;
                        XWindowAttributes attributes;
                        if (!XGetWindowAttributes(d, s->window, &attributes)) {
                            free(s->text);
                            memset(s, 0, sizeof(*s));
                            break;
                        }
                        XSelectInput(d, s->window, attributes.your_event_mask | PropertyChangeMask);
                        unsigned long amount = (unsigned long)length;
                        XChangeProperty(d, s->window, property, atom_incr, 32, PropModeReplace, (unsigned char *)&amount, 1);
                        reply.xselection.property = property;
                        break;
                    }
        }
    }
    XSendEvent(d, request->requestor, False, 0, &reply);
    XFlush(d);
}
static int clipboard_event(Display *d, Window window, XEvent *event) {
    if (event->type == SelectionRequest) { clipboard_reply(d, &event->xselectionrequest); return 1; }
    if (event->type == SelectionClear) return 1;
    if (event->type == SelectionNotify && paste_pending && event->xselection.requestor == window &&
        event->xselection.selection == atom_clipboard) {
        if (event->xselection.property == None) {
            if (!paste_fallback) {
                paste_fallback = 1;
                XConvertSelection(d, atom_clipboard, XA_STRING, atom_paste, window, event->xselection.time);
            } else {
                paste_overflow = 1;
                paste_complete();
            }
        } else {
            clipboard_read(d, window);
            if (!paste_incr) paste_complete();
        }
        return 1;
    }
    if (event->type == PropertyNotify) {
        XPropertyEvent *p = &event->xproperty;
        if (paste_pending && paste_incr && p->window == window && p->atom == atom_paste && p->state == PropertyNewValue) {
            paste_deadline = watch_now() + 10000;
            int more = clipboard_read(d, window);
            if (more <= 0) paste_complete();
            return 1;
        }
        if (p->state == PropertyDelete)
            for (int i = 0; i < 8; i++) {
                ClipboardSend *s = &clipboard_sends[i];
                if (s->text && s->window == p->window && s->property == p->atom) {
                    size_t size = s->length - s->offset;
                    if (size > 32768) size = 32768;
                    XChangeProperty(d, s->window, s->property, s->target, 8, PropModeReplace, (unsigned char *)s->text + s->offset,
                                    (int)size);
                    s->offset += size; s->deadline = watch_now() + 10000;
                    if (!size) { free(s->text); memset(s, 0, sizeof(*s)); }
                    return 1;
                }
            }
    }
    return 0;
}
static int clipboard_timeout(void) {
    long long now = watch_now();
    int active = 0;
    if (paste_pending) {
        if (now >= paste_deadline) {
            paste_overflow = 1;
            paste_complete();
        } else active = 1;
    }
    for (int i = 0; i < 8; i++)
        if (clipboard_sends[i].text) {
            if (now >= clipboard_sends[i].deadline) {
                free(clipboard_sends[i].text);
                memset(&clipboard_sends[i], 0, sizeof(clipboard_sends[i]));
            } else active = 1;
        }
    return active ? 100 : -1;
}

static int gui_xerror(Display *d, XErrorEvent *event) {
    if (event->error_code == BadWindow || event->error_code == BadMatch || event->error_code == BadAtom) return 0;
    char message[128];
    XGetErrorText(d, event->error_code, message, sizeof(message));
    if (tabs[active_tab].used) remember_line(&tabs[active_tab], message);
    return 0;
}
static void selection_point(Tab *t, int x, int y, int width, int height, XFontStruct *font, int anchor) {
    (void)width;
    int visible = visible_rows(height);
    int end = t->line_count - t->scroll;
    int first = first_visible_line(t, visible);
    int row = first + (y - OUTPUT_TOP) / ROW_HEIGHT;
    if (row < first) row = first;
    if (row >= end) row = end - 1;
    if (row < 0 || row >= t->line_count) return;
    int column = (x - TEXT_LEFT) / (font->max_bounds.width ? font->max_bounds.width : 6);
    if (column < 0) column = 0;
    int length = (int)strlen(screen_line(t, row));
    if (column > length) column = length;
    if (anchor) { t->anchor_line = row; t->anchor_col = column; }
    t->end_line = row; t->end_col = column;
    t->selection_active = t->anchor_line != row || t->anchor_col != column;
}

/* Rebuild the window from stored tab output and editor state. */
static void redraw(Display *display, Window window, GC gc, XFontStruct *font, int width, int height) {
    Tab *t = &tabs[active_tab];
    XClearWindow(display, window);
    int x = 10;
    for (int i = 0; i < MAX_TABS; i++)
        if (tabs[i].used) {
            char label[40];
            snprintf(label, sizeof(label), "%s%d%s%s", i == active_tab ? "[" : " ", i + 1, tabs[i].busy ? "*" : "",
                     i == active_tab ? "]" : " ");
            XDrawString(display, window, gc, x, 20, label, (int)strlen(label));
            x += 65;
        }
    XDrawString(display, window, gc, x, 20, "[+]", 3);
    XDrawLine(display, window, gc, 0, 30, width, 30);
    char header[PATH_MAX + 40];
    snprintf(header, sizeof(header), "%s | %s", process_mode ? "Process mode" : "Thread mode", t->cwd);
    if (t->completion_count) {
        size_t used = strlen(header);
        snprintf(header + used, sizeof(header) - used, " | choice 1-%d: %s", t->completion_count, t->completion_digits);
    }
    XDrawString(display, window, gc, 10, 48, header, (int)strlen(header));
    int visible = visible_rows(height);
    int max_scroll = t->line_count > visible ? t->line_count - visible : 0;
    if (t->scroll > max_scroll) t->scroll = max_scroll;
    int end = t->line_count - t->scroll;
    int first = first_visible_line(t, visible);
    int y = OUTPUT_BASELINE;
    for (int i = first; i < end; i++, y += ROW_HEIGHT) {
        XDrawString(display, window, gc, 10, y, screen_line(t, i), (int)strlen(screen_line(t, i)));
        if (t->selection_active) {
            int a, ac, b, bc;
            selection_bounds(t, &a, &ac, &b, &bc);
            if (i >= a && i <= b) {
                int start = i == a ? ac : 0, stop = i == b ? bc : (int)strlen(screen_line(t, i));
                int length = (int)strlen(screen_line(t, i));
                if (start > length) start = length;
                if (stop > length) stop = length;
                if (stop > start) {
                    int left = 10 + XTextWidth(font, screen_line(t, i), start),
                        span = XTextWidth(font, screen_line(t, i) + start, stop - start);
                    XFillRectangle(display, window, gc, left, y - font->ascent, (unsigned)span, (unsigned)(font->ascent + font->descent));
                    XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
                    XDrawString(display, window, gc, left, y, screen_line(t, i) + start, stop - start);
                    XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
                }
            }
        }
    }
    const char *prefix = t->search_mode ? "Enter search term: " : t->busy ? "[running] " : t->continued_used ? "> " : "user@terminator> ";
    char prompt[COMMAND_SIZE + 64];
    snprintf(prompt, sizeof(prompt), "%s%s", prefix, t->input);

    for (size_t i = 0; prompt[i]; i++) {
        if (prompt[i] == '\n') prompt[i] = '~';
        else if (prompt[i] == '\t') prompt[i] = ' ';
    }
    int available = (width - 20) / (font->max_bounds.width ? font->max_bounds.width : 6);
    if (available < 2) available = 2;
    int length = (int)strlen(prompt), start = 0, position = (int)strlen(prefix) + t->cursor;
    if (position >= available) start = position - available + 1;
    int shown = length - start;
    if (shown > available) shown = available;
    int baseline = height - 15;
    XDrawString(display, window, gc, 10, baseline, prompt + start, shown);
    int cx = 10 + XTextWidth(font, prompt + start, position - start);
    XDrawLine(display, window, gc, cx, baseline - font->ascent, cx, baseline + font->descent);
    XFlush(display);
}
/* Send typed input to the foreground command; EOF closes its input. */
static void send_stdin(Tab *t, int eof) {
    char text[COMMAND_SIZE + 1];
    size_t length = (size_t)t->input_length;
    memcpy(text, t->input, length);
    if (!eof) text[length++] = '\n';
    text[length] = '\0';
    int frames = (int)(length / (INPUT_SIZE - 1)) + 1;
    if (frames + eof > QUEUE_SIZE - t->queue_count) {
        remember_line(t, "Input queue is full; try again after output progresses.");
        return;
    }
    if (request_send(t, REQUEST_INPUT, text) == -1 || (eof && request_send(t, REQUEST_EOF, NULL) == -1)) {
        remember_line(t, "Could not send command input.");
        return;
    }
    if (t->input_length) remember_line(t, t->input);
    editor_reset(t);
}
int gui_run(void) {
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open display. Start XQuartz or your Linux desktop.\n");
        history_destroy();
        return 1;
    }
    XSetErrorHandler(gui_xerror);
    gui_fd = ConnectionNumber(display);
    if (set_flags(gui_fd, 0) == -1) {
        perror("X11 descriptor");
        XCloseDisplay(display);
        history_destroy();
        return 1;
    }
    int screen = DefaultScreen(display), width = 900, height = 550;
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 100, 100, (unsigned)width, (unsigned)height, 1,
    BlackPixel(display, screen), BlackPixel(display, screen));
    XStoreName(display, window, "Terminator - Tabs");
    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
                     PropertyChangeMask);
    atom_clipboard = XInternAtom(display, "CLIPBOARD", False); atom_utf8 = XInternAtom(display, "UTF8_STRING", False);
    atom_targets = XInternAtom(display, "TARGETS", False); atom_text = XInternAtom(display, "TEXT", False);
    atom_incr = XInternAtom(display, "INCR", False); atom_paste = XInternAtom(display, "TERMINATOR_PASTE", False);
    Atom protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &delete_window, 1);
    GC gc = XCreateGC(display, window, 0, NULL);
    XSetForeground(display, gc, WhitePixel(display, screen));
    XFontStruct *font = XLoadQueryFont(display, "9x15");
    if (!font) font = XLoadQueryFont(display, "fixed");
    if (!font || new_tab() == -1) {
        fprintf(stderr, "Could not create font or worker.\n");
        if (font) XFreeFont(display, font);
        XFreeGC(display, gc); XDestroyWindow(display, window);
        XCloseDisplay(display);
        history_destroy();
        return 1;
    }
    XSetFont(display, gc, font->fid);
    XMapWindow(display, window);
    bindings_load();
    append_output(&tabs[active_tab], binding_messages);
    if (*history_warning) append_output(&tabs[active_tab], history_warning);
    int running = 1, dirty = 1;
    /* Collect output and X11 events; poll below blocks when neither is ready. */
    while (running && tab_count()) {
        for (int i = 0; i < MAX_TABS; i++)
            if (tabs[i].used) dirty |= collect_replies(i);
        if (!tab_count()) break;
        int events = 0;
        while (running && XPending(display) && events++ < 64) {
            XEvent event;
            XNextEvent(display, &event);
            Tab *t = &tabs[active_tab];
            if (clipboard_event(display, window, &event)) { dirty = 1; continue; }
            /* Expose means the window must be repainted. */
            if (event.type == Expose) dirty = 1;
            else if (event.type == ConfigureNotify) {
                width = event.xconfigure.width; height = event.xconfigure.height;
                dirty = 1;
            } else if (event.type == ButtonPress) {
                if (event.xbutton.button == 4) t->scroll += 3;
                else if (event.xbutton.button == 5) {
                    t->scroll -= 3;
                    if (t->scroll < 0) t->scroll = 0;
                } else if (event.xbutton.button == 1 && event.xbutton.y < 30) {
                    selection_dragging = 0;
                    int x = 10, selected = -1;
                    for (int i = 0; i < MAX_TABS; i++)
                        if (tabs[i].used) {
                            if (event.xbutton.x >= x && event.xbutton.x < x + 65) selected = i;
                            x += 65;
                        }
                    if (selected >= 0) active_tab = selected;
                    else if (event.xbutton.x >= x && event.xbutton.x < x + 40) {
                        if (new_tab() == -1) remember_line(t, "Cannot open tab (maximum 8 or resource error).");
                    }
                } else if (event.xbutton.button == 1 && event.xbutton.y >= 55 && event.xbutton.y < height - 40) {
                    t->selection_active = 0; selection_dragging = 1;
                    selection_point(t, event.xbutton.x, event.xbutton.y, width, height, font, 1);
                }
                dirty = 1;
            } else if (event.type == MotionNotify && selection_dragging) {
                selection_point(t, event.xmotion.x, event.xmotion.y, width, height, font, 0);
                dirty = 1;
            } else if (event.type == ButtonRelease && event.xbutton.button == 1) {
                if (selection_dragging && t->selection_active) clipboard_copy(display, window, t, event.xbutton.time, 1);
                selection_dragging = 0; dirty = 1;
            } else if (event.type == KeyPress) {
                /* Translate this key event, then apply its configured action. */
                char typed[32];
                KeySym key;
                int n = XLookupString(&event.xkey, typed, sizeof(typed), &key, NULL);
                unsigned state = event.xkey.state;
                int action = binding_match(key, state);
                if (action == HK_NEW_TAB) {
                    if (new_tab() == -1) remember_line(t, "Cannot open tab (maximum 8 or resource error).");
                } else if (action == HK_CLOSE_TAB) close_tab(active_tab);
                else if (action == HK_NEXT_TAB || action == HK_PREV_TAB) {
                    int direction = action == HK_PREV_TAB ? -1 : 1;
                    selection_dragging = 0;
                    for (int k = 1; k <= MAX_TABS; k++) {
                        int i = (active_tab + direction * k + MAX_TABS) % MAX_TABS;
                        if (tabs[i].used) { active_tab = i; break; }
                    }
                } else if (t->completion_count && key == XK_Escape) completion_clear(t);
                else if (t->completion_count && (action == HK_INTERRUPT || action == HK_EOF || action == HK_SUSPEND)) completion_clear(t);
                else if (t->completion_count && action == HK_ENTER) completion_confirm(t);
                else if (t->completion_count && action == HK_BACKSPACE) {
                    size_t length = strlen(t->completion_digits);
                    if (length) t->completion_digits[length - 1] = '\0';
                    else completion_clear(t);
                } else if (t->completion_count && !(state & (ControlMask | Mod1Mask | Mod4Mask)) && n == 1 && typed[0] >= '0' &&
                           typed[0] <= '9')
                    completion_digit(t, typed[0]);
                else if (action == HK_COMPLETE) completion_begin(t);
                else if (action == HK_SEARCH) history_search_begin(t);
                else if (t->search_mode && (action == HK_INTERRUPT || action == HK_EOF || action == HK_SUSPEND || key == XK_Escape)) {
                    history_search_restore(t);
                } else if (action == HK_INTERRUPT) {
                    if (t->busy) request_send(t, REQUEST_INTERRUPT, NULL);
                    editor_reset(t);
                    t->continued_used = 0; t->continued[0] = '\0';
                } else if (action == HK_SUSPEND) {
                    if (t->busy) request_send(t, REQUEST_SUSPEND, NULL);
                    editor_reset(t);
                    t->continued_used = 0; t->continued[0] = '\0';
                } else if (action == HK_COPY) clipboard_copy(display, window, t, event.xkey.time, 0);
                else if (action == HK_PASTE) clipboard_request(display, window, t, event.xkey.time);
                else if (action == HK_PAGE_UP) t->scroll += 10;
                else if (action == HK_PAGE_DOWN) {
                    t->scroll -= 10;
                    if (t->scroll < 0) t->scroll = 0;
                } else if (action == HK_ENTER) {
                    if (t->search_mode) history_search_submit(t);
                    else if (t->busy) send_stdin(t, 0);
                    else submit(t);
                } else if (action == HK_LEFT) {
                    completion_clear(t);
                    if (t->cursor > 0) t->cursor--;
                } else if (action == HK_RIGHT) {
                    completion_clear(t);
                    if (t->cursor < t->input_length) t->cursor++;
                } else if (action == HK_START) {
                    completion_clear(t);
                    t->cursor = 0;
                } else if (action == HK_END) {
                    completion_clear(t);
                    t->cursor = t->input_length;
                } else if (action == HK_PREVIOUS) editor_history(t, -1);
                else if (action == HK_NEXT) editor_history(t, 1);
                else if (action == HK_KILL_START) editor_delete(t, 0, t->cursor);
                else if (action == HK_KILL_END) editor_delete(t, t->cursor, t->input_length);
                else if (action == HK_WORD) editor_previous_word(t);
                else if (action == HK_BACKSPACE) {
                    if (t->cursor > 0) editor_delete(t, t->cursor - 1, t->cursor);
                } else if (action == HK_DELETE) editor_delete(t, t->cursor, t->cursor + 1);
                else if (action == HK_CLEAR) {
                    completion_clear(t);
                    screen_clear(t);
                } else if (action == HK_EOF) {
                    if (t->busy) send_stdin(t, 1);
                    else if (!t->input_length) close_tab(active_tab);
                    else editor_delete(t, t->cursor, t->cursor + 1);
                } else if (!(state & (ControlMask | Mod1Mask | Mod4Mask))) {
                    for (int j = 0; j < n; j++)
                        if ((unsigned char)typed[j] >= 32 && (unsigned char)typed[j] <= 126) {
                            if (editor_insert(t, typed + j, 1) == -1) XBell(display, 0);
                            t->selection_active = 0;
                        }
                }
                dirty = 1;
            } else if (event.type == ClientMessage && event.xclient.message_type == protocols && event.xclient.format == 32 &&
                       (Atom)event.xclient.data.l[0] == delete_window)
                running = 0;
            if (!tab_count()) break;
        }
        if (!running || !tab_count()) break;
        int paste_was_pending = paste_pending;
        int timeout = clipboard_timeout();
        if (paste_was_pending && !paste_pending) dirty = 1;
        if (dirty) { redraw(display, window, gc, font, width, height); dirty = 0; }
        struct pollfd fds[2 * MAX_TABS + 1];
        int count = 1;
        fds[0] = (struct pollfd){gui_fd, POLLIN, 0};
        for (int i = 0; i < MAX_TABS; i++)
            if (tabs[i].used) {
                fds[count++] = (struct pollfd){tabs[i].reply_fd, POLLIN, 0};
                if (tabs[i].queue_count) fds[count++] = (struct pollfd){tabs[i].request_fd, POLLOUT, 0};
            }
        if (poll(fds, (nfds_t)count, XPending(display) ? 0 : timeout) == -1 && errno != EINTR) running = 0;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) running = 0;
    }
    for (int i = 0; i < MAX_TABS; i++)
        if (tabs[i].used) close_tab(i);
    free(clipboard_text);
    for (int i = 0; i < 8; i++) free(clipboard_sends[i].text);
    XFreeGC(display, gc);
    XFreeFont(display, font);
    XDestroyWindow(display, window); XCloseDisplay(display);
    history_destroy();
    return 0;
}
#endif
