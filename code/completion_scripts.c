/*
 * Implements file-name completion and internal script execution.
 * Task 10 finds filename matches and updates the input line.
 * Task 11 validates and executes the supported script syntax.
 */
#include "terminator.h"

void completion_clear(Tab *t) {
    for (int i = 0; i < t->completion_count; i++) free(t->completion_matches[i]);
    free(t->completion_matches);
    t->completion_matches = NULL; t->completion_count = 0;
    t->completion_digits[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Task 10 - file-name auto-completion                                */
/* ------------------------------------------------------------------ */
/* Locate and decode only the word before the cursor. */
static int completion_word(Tab *t, int *start, int *quote, char *decoded) {
    int q = 0, begin = -1;
    size_t used = 0;
    size_t previous = t->continued_used, total = previous + (size_t)t->cursor;
    for (size_t i = 0; i < total; i++) {
        char c = i < previous ? t->continued[i] : t->input[i - previous];
        if (!q && strchr(" \t\n|<>&;()", c)) { begin = -1; used = 0; continue; }
        if (begin < 0) begin = (int)i;
        if (c == '\\' && q != '\'') {
            if (i + 1 >= total) return 0;
            char next = i + 1 < previous ? t->continued[i + 1] : t->input[i + 1 - previous];
            if (!q || strchr("$`\"\\\n", next)) { i++; if (next == '\n') continue; c = next; }
        } else if (c == '\'' || c == '"') {
            if (!q) { q = c; continue; }
            if (q == c) { q = 0; continue; }
        }
        if (used >= COMMAND_SIZE - 2) return 0;
        decoded[used++] = c;
    }
    if (begin < 0) begin = (int)total;
    if (begin < (int)previous) return 0;
    decoded[used] = '\0';
    *start = begin - (int)previous;
    *quote = q;
    return 1;
}

static int completion_encode(const char *name, int quote, int close_quote, char *out, size_t capacity) {
    size_t used = 0;
    if (quote) out[used++] = (char)quote;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        char part[4];
        size_t n = 1;
        part[0] = (char)*p;
        if (quote == '\'' && *p == '\'') {
            memcpy(part, "'\\''", 4);
            n = 4;
        } else if (quote == '"' && strchr("$`\"\\", *p)) {
            part[0] = '\\'; part[1] = (char)*p;
            n = 2;
        } else if (!quote && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("_./-", *p))) {
            part[0] = '\\'; part[1] = (char)*p;
            n = 2;
        }
        if (used + n + 2 >= capacity) return -1;
        memcpy(out + used, part, n);
        used += n;
    }
    if (quote && close_quote) out[used++] = (char)quote;
    out[used] = '\0';
    return (int)used;
}
static int completion_replace(Tab *t, const char *name, int finish) {
    char encoded[2 * COMMAND_SIZE];
    int length = completion_encode(name, t->completion_quote, finish && t->completion_end == t->input_length, encoded, sizeof(encoded));
    int removed = t->completion_end - t->completion_start;
    if (length < 0 || (t->search_mode ? 0 : t->continued_used) + (size_t)(t->input_length - removed + length) > COMMAND_SIZE - 2) {
        remember_line(t, "Completion exceeds the input limit; input unchanged.");
        return -1;
    }
    memmove(t->input + t->completion_start + length, t->input + t->completion_end, (size_t)(t->input_length - t->completion_end) + 1);
    memcpy(t->input + t->completion_start, encoded, (size_t)length);
    t->input_length += length - removed; t->cursor = t->completion_start + length;
    t->completion_end = t->cursor; t->selection_active = 0;
    return 0;
}
static int completion_compare(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void completion_show(Tab *t) {
    t->partial = 0; t->scroll = 0;
    for (int i = 0; i < t->completion_count; i++) {
        char number[48];
        snprintf(number, sizeof(number), "%d. ", i + 1);
        append_output(t, number);
        append_output(t, t->completion_matches[i]);
        append_output(t, "\n");
    }
    if (t->completion_count < 10) remember_line(t, "Press a listed number to choose; Escape cancels. Enter does not run the command.");
    else remember_line(t, "Type a choice number, then Enter; Escape cancels. Choosing never runs the command.");
}
/* One match completes; multiple matches extend to their common prefix. */
void completion_begin(Tab *t) {
    if (t->busy || t->search_mode) return;
    completion_clear(t);
    char word[COMMAND_SIZE];
    int start, quote;
    if (!completion_word(t, &start, &quote, word)) return;
    char *slash = strrchr(word, '/');
    size_t prefix = slash ? (size_t)(slash - word) + 1 : 0;
    const char *base = word + prefix;
    size_t base_length = strlen(base);
    char path[PATH_MAX];
    int n;
    if (prefix) {
        if (word[0] == '/') n = snprintf(path, sizeof(path), "%.*s", (int)prefix, word);
        else n = snprintf(path, sizeof(path), "%s/%.*s", t->cwd, (int)prefix, word);
    } else n = snprintf(path, sizeof(path), "%s", t->cwd);
    if (n < 0 || n >= (int)sizeof(path)) return;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return; }
    char **matches = NULL;
    int count = 0, capacity = 0, error = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(directory);
        if (!entry) { if (errno) error = 2; break; }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || (name[0] == '.' && base[0] != '.') || strncmp(name, base, base_length)) continue;

        int printable = 1;
        for (const unsigned char *p = (const unsigned char *)name; *p; p++)
            if (*p < 32 || *p > 126) { printable = 0; break; }
        if (!printable) continue;
        if (count == capacity) {
            if (capacity > INT_MAX / 2) { error = 1; break; }
            int next = capacity ? capacity * 2 : 16;
            char **grown = realloc(matches, (size_t)next * sizeof(*grown));
            if (!grown) { error = 1; break; }
            matches = grown; capacity = next;
        }
        struct stat st;
        int is_directory = fstatat(dirfd(directory), name, &st, 0) == 0 && S_ISDIR(st.st_mode);
        size_t length = prefix + strlen(name) + (is_directory ? 1 : 0);
        char *candidate = malloc(length + 1);
        if (!candidate) { error = 1; break; }
        memcpy(candidate, word, prefix);
        strcpy(candidate + prefix, name);
        if (is_directory) strcat(candidate, "/");
        matches[count++] = candidate; errno = 0;
    }

    closedir(directory);
    if (error || !count) {
        for (int i = 0; i < count; i++) free(matches[i]);
        free(matches);
        if (error) remember_line(t, error == 2 ? "Unable to read completion directory." : "Unable to allocate file-name completions.");
        return;
    }
    qsort(matches, (size_t)count, sizeof(*matches), completion_compare);
    t->completion_matches = matches; t->completion_count = count;
    t->completion_start = start; t->completion_end = t->cursor;
    t->completion_quote = quote;
    if (count == 1) { completion_replace(t, matches[0], 1); completion_clear(t); return; }
    size_t common = strlen(matches[0]);
    for (int i = 1; i < count; i++) {
        size_t j = 0;
        while (j < common && matches[0][j] == matches[i][j]) j++;
        common = j;
    }
    if (common > strlen(word)) {
        char saved = matches[0][common];
        matches[0][common] = '\0';
        int result = completion_replace(t, matches[0], 0);
        matches[0][common] = saved;
        if (result < 0) { completion_clear(t); return; }
    }
    completion_show(t);
}
static void completion_choose(Tab *t, unsigned long number) {
    if (!number || number > (unsigned long)t->completion_count) {
        remember_line(t, "Invalid completion number; choose a listed number or press Escape.");
        t->completion_digits[0] = '\0';
        return;
    }
    completion_replace(t, t->completion_matches[number - 1], 1);
    completion_clear(t);
}

void completion_digit(Tab *t, char digit) {
    if (t->completion_count < 10) { completion_choose(t, (unsigned long)(digit - '0')); return; }
    size_t used = strlen(t->completion_digits);
    if (used + 1 < sizeof(t->completion_digits)) {
        t->completion_digits[used] = digit; t->completion_digits[used + 1] = '\0';
    }
}
void completion_confirm(Tab *t) {
    if (!*t->completion_digits) { completion_clear(t); return; }
    errno = 0;
    char *end;
    unsigned long number = strtoul(t->completion_digits, &end, 10);
    if (errno || *end) { remember_line(t, "Invalid completion number."); t->completion_digits[0] = '\0'; return; }
    completion_choose(t, number);
}


/* ------------------------------------------------------------------ */
/* Task 11 - script lexer and internal interpreter                   */
/* ------------------------------------------------------------------ */
enum { ST_COMMAND, ST_IF, ST_THEN, ST_ELIF, ST_ELSE, ST_FI };
typedef struct {
    int kind, line;
    char *text;
} ScriptToken;
typedef struct ScriptNode ScriptNode;
typedef struct ScriptBranch {
    ScriptNode *condition, *body;
    struct ScriptBranch *next;
} ScriptBranch;
struct ScriptNode {
    char *command;
    ScriptBranch *branches;
    ScriptNode *next;
};
typedef struct {
    ScriptToken *tokens;
    size_t count, capacity, position;
    int error, line;
    char message[160];
} ScriptParser;
static void script_fail(ScriptParser *p, int incomplete, const char *message) {
    if (!p->error) { p->error = incomplete ? 1 : -1; snprintf(p->message, sizeof(p->message), "%s", message); }
}
static void script_free(ScriptNode *node) {
    while (node) {
        ScriptNode *next = node->next;
        ScriptBranch *b = node->branches;
        while (b) {
            ScriptBranch *n = b->next;
            script_free(b->condition);
            script_free(b->body);
            free(b);
            b = n;
        }
        free(node->command); free(node);
        node = next;
    }
}
static int script_token_add(ScriptParser *p, int kind, const char *text, size_t length, int line) {
    if (p->count == p->capacity) {
        size_t capacity = p->capacity ? p->capacity * 2 : 32;
        ScriptToken *tokens = realloc(p->tokens, capacity * sizeof(*tokens));
        if (!tokens) { script_fail(p, 0, "out of memory"); return -1; }
        p->tokens = tokens; p->capacity = capacity;
    }
    ScriptToken *t = &p->tokens[p->count];
    t->kind = kind; t->line = line;
    t->text = strndup(text, length);
    if (!t->text) { script_fail(p, 0, "out of memory"); return -1; }
    p->count++;
    return 0;
}
static int script_keyword(const char *text, const char *word) {
    size_t n = strlen(word);
    return !strncmp(text, word, n) && (!text[n] || strchr(" \t\r\n", text[n]));
}
static int script_emit_depth(ScriptParser *p, const char *buffer, size_t length, int line, int depth) {
    if (depth > NESTING_LIMIT) { script_fail(p, 0, "keyword nesting limit exceeded"); return -1; }
    while (length && strchr(" \t\r\n", *buffer)) { buffer++; length--; }
    while (length && strchr(" \t\r\n", buffer[length - 1])) {
        size_t slashes = 0, j = length - 1;
        while (j && buffer[j - 1] == '\\') { slashes++; j--; }
        if (slashes % 2) break;
        length--;
    }
    if (!length) return 0;
    char *text = strndup(buffer, length);
    if (!text) { script_fail(p, 0, "out of memory"); return -1; }
    static const char *keywords[] = {"if", "then", "elif", "else", "fi"};
    for (int k = 0; k < 5; k++)
        if (script_keyword(text, keywords[k])) {
            size_t n = strlen(keywords[k]);
            if (script_token_add(p, ST_IF + k, keywords[k], n, line) < 0) { free(text); return -1; }
            const char *rest = text + n;
            rest += strspn(rest, " \t\r\n");
            if (*rest) {
                if (k == 4) { script_fail(p, 0, "expected separator after fi"); free(text); return -1; }
                int result = script_emit_depth(p, rest, strlen(rest), line, depth + 1);
                free(text);
                return result;
            }
            free(text);
            return 0;
        }
    const char *unsupported[] = {"for", "while", "until", "case", "select", "function", "do", "done", "esac"};
    for (size_t k = 0; k < sizeof(unsupported) / sizeof(*unsupported); k++)
        if (script_keyword(text, unsupported[k])) {
            script_fail(p, 0, "loops, case statements and functions are not supported");
            free(text);
            return -1;
        }
    if (length >= COMMAND_SIZE - 1) { script_fail(p, 0, "script command exceeds 8190 bytes"); free(text); return -1; }
    int result = script_token_add(p, ST_COMMAND, text, length, line);
    free(text);
    return result;
}
static int script_emit(ScriptParser *p, const char *buffer, size_t length, int line) {
    return script_emit_depth(p, buffer, length, line, 0);
}
/* Split script commands and keywords without breaking quoted text. */
static int script_lex(ScriptParser *p, const char *text) {
    size_t length = strlen(text), used = 0;
    char *buffer = malloc(length + 1);
    if (!buffer) { script_fail(p, 0, "out of memory"); return -1; }
    int quote = 0, line = 1, start_line = 1;
    for (size_t i = 0; i < length && !p->error;) {
        char c = text[i];
        if (!quote && c == '#' && (!used || strchr(" \t\n|;", buffer[used - 1]))) {
            while (i < length && text[i] != '\n') i++;
            continue;
        }
        if (quote != '\'' && c == '\\') {
            if (i + 1 == length) { script_fail(p, 1, "unfinished line continuation"); break; }
            if (text[i + 1] == '\n') { i += 2; line++; continue; }
            buffer[used++] = text[i++]; buffer[used++] = text[i++];
            continue;
        }
        if (quote != '\'' && (c == '`' || (c == '$' && text[i + 1] == '('))) {
            size_t end = i;
            char *body = NULL;
            if (extract_substitution(text, &end, c == '`', &body) < 0) {
                script_fail(p, 1, "unfinished command substitution");
                break;
            }
            free(body);
            for (size_t j = i; j < end; j++)
                if (text[j] == '\n') line++;
            memcpy(buffer + used, text + i, end - i);
            used += end - i; i = end;
            continue;
        }
        if (c == '\'' || c == '"') { if (!quote) quote = c; else if (quote == c) quote = 0; }
        if (!quote && (c == ';' || c == '\n')) {
            size_t end = used;
            while (end && strchr(" \t\r\n", buffer[end - 1])) end--;
            if (c == '\n' && end && buffer[end - 1] == '|') {
                buffer[used++] = '\n';
                i++;
                line++;
                continue;
            }
            if (c == ';' && (!end || (i + 1 < length && text[i + 1] == ';'))) {
                script_fail(p, 0, "unexpected semicolon");
                break;
            }
            if (script_emit(p, buffer, used, start_line) < 0) break;
            used = 0;
            i++;
            if (c == '\n') line++;
            start_line = line;
            continue;
        }
        if (!used) start_line = line;
        buffer[used++] = c;
        i++;
        if (c == '\n') line++;
    }
    if (!p->error && quote) script_fail(p, 1, "unterminated quote");
    if (!p->error) script_emit(p, buffer, used, start_line);
    free(buffer);
    return p->error ? -1 : 0;
}
int script_assignment_prefix(const char *text) {
    if (!((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z') || *text == '_')) return 0;
    text++;
    while ((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z') || (*text >= '0' && *text <= '9') || *text == '_') text++;
    return *text == '=';
}
static int script_check_text(ScriptParser *p, const char *text, int depth);

static int script_validate_command(ScriptParser *p, const char *text, int depth) {
    int lexical = lexical_completeness(text);
    if (lexical) { script_fail(p, lexical == 1, "incomplete or invalid command syntax"); return -1; }
    int quote = 0, word = 0, pending = 0, assignment = script_assignment_prefix(text);
    int stage_assignment = assignment, test_command = script_keyword(text, "["), test_closed = 0;
    for (size_t i = 0; text[i] && !p->error;) {
        char c = text[i];
        if (!quote && strchr(" \t\n", c)) { word = 0; i++; continue; }
        if (!quote && (c == '<' || c == '>')) {
            if (pending || text[i + 1] == c) { script_fail(p, 0, "invalid redirection"); break; }
            pending = 1; word = 0;
            i++;
            continue;
        }
        if (!quote && c == '&') { script_fail(p, 0, "background jobs inside script files are not supported"); break; }
        if (!quote && c == '|') {
            if (pending) { script_fail(p, 0, "missing redirection filename"); break; }
            word = 0;
            i++;
            size_t j = i + strspn(text + i, " \t\n");
            stage_assignment = script_assignment_prefix(text + j);
            continue;
        }
        if (!word) {
            if (!quote && !pending && c == ']' && (!text[i + 1] || strchr(" \t\n|<>", text[i + 1]))) test_closed = 1;
            if (stage_assignment && !pending && !script_assignment_prefix(text + i)) {
                script_fail(p, 0, "assignment statements must be separate from commands");
                break;
            }
            word = 1; pending = 0;
        }
        if (quote != '\'' && c == '\\') {
            if (text[i + 1]) i += 2;
            else { script_fail(p, 1, "unfinished escape"); break; }
            continue;
        }
        if (quote != '\'' && (c == '`' || (c == '$' && text[i + 1] == '('))) {
            char *body = NULL;
            size_t end = i;
            if (extract_substitution(text, &end, c == '`', &body) < 0) {
                script_fail(p, 1, "unfinished substitution");
                break;
            }
            script_check_text(p, body, depth + 1);
            free(body);
            i = end;
            continue;
        }
        if (quote != '\'' && c == '$' && text[i + 1] == '{') {
            size_t end = i + 2;
            while (text[end] && text[end] != '}') end++;
            if (!text[end]) { script_fail(p, 1, "unfinished parameter expansion"); break; }
            size_t n = end - (i + 2);
            int valid = n > 0;
            int numeric = n && text[i + 2] >= '0' && text[i + 2] <= '9';
            for (size_t j = i + 2; j < end; j++) {
                char parameter = text[j];
                if (numeric) {
                    if (parameter < '0' || parameter > '9') valid = 0;
                } else if (!((parameter >= 'a' && parameter <= 'z') || (parameter >= 'A' && parameter <= 'Z') ||
                             (j > i + 2 && parameter >= '0' && parameter <= '9') || parameter == '_' ||
                             ((parameter == '?' || parameter == '#') && n == 1)))
                    valid = 0;
            }
            if (!valid) { script_fail(p, 0, "unsupported parameter expansion"); break; }
            i = end + 1;
            continue;
        }
        if (c == '\'' || c == '"') { if (!quote) quote = c; else if (quote == c) quote = 0; }
        i++;
    }
    if (!p->error && pending) script_fail(p, 0, "missing redirection filename");
    if (!p->error && test_command && !test_closed) script_fail(p, 0, "missing closing ] in test");
    return p->error ? -1 : 0;
}
static ScriptNode *script_list(ScriptParser *p, int depth);
static ScriptNode *script_if(ScriptParser *p, int depth) {
    if (depth > NESTING_LIMIT) { script_fail(p, 0, "conditional nesting limit exceeded"); return NULL; }
    ScriptNode *node = calloc(1, sizeof(*node));
    if (!node) { script_fail(p, 0, "out of memory"); return NULL; }
    ScriptBranch **tail = &node->branches;
    do {
        p->position++;
        ScriptBranch *b = calloc(1, sizeof(*b));
        if (!b) { script_fail(p, 0, "out of memory"); break; }
        *tail = b;
        tail = &b->next; b->condition = script_list(p, depth + 1);
        if (p->error) break;
        if (!b->condition) { script_fail(p, p->position == p->count, "missing if/elif condition"); break; }
        if (p->position == p->count || p->tokens[p->position].kind != ST_THEN) {
            script_fail(p, p->position == p->count, "missing then");
            break;
        }
        p->position++;
        b->body = script_list(p, depth + 1);
        if (!p->error && !b->body) script_fail(p, p->position == p->count, "empty then branch");
    } while (!p->error && p->position < p->count && p->tokens[p->position].kind == ST_ELIF);
    if (!p->error && p->position < p->count && p->tokens[p->position].kind == ST_ELSE) {
        p->position++;
        ScriptBranch *b = calloc(1, sizeof(*b));
        if (!b) script_fail(p, 0, "out of memory");
        else {
            *tail = b;
            b->body = script_list(p, depth + 1);
            if (!p->error && !b->body) script_fail(p, p->position == p->count, "empty else branch");
        }
    }
    if (!p->error) {
        if (p->position == p->count || p->tokens[p->position].kind != ST_FI) script_fail(p, p->position == p->count, "missing fi");
        else p->position++;
    }
    if (p->error) { script_free(node); return NULL; }
    return node;
}
static ScriptNode *script_list(ScriptParser *p, int depth) {
    ScriptNode *head = NULL, **tail = &head;
    while (p->position < p->count && !p->error) {
        ScriptToken *token = &p->tokens[p->position];
        p->line = token->line;
        if (token->kind != ST_COMMAND && token->kind != ST_IF) break;
        ScriptNode *node;
        if (token->kind == ST_IF) node = script_if(p, depth);
        else {
            if (script_validate_command(p, token->text, depth) < 0) break;
            node = calloc(1, sizeof(*node));
            if (!node) { script_fail(p, 0, "out of memory"); break; }
            node->command = strdup(token->text);
            p->position++;
            if (!node->command) { free(node); script_fail(p, 0, "out of memory"); break; }
        }
        if (!node) break;
        *tail = node;
        tail = &node->next;
    }
    if (p->error) { script_free(head); return NULL; }
    return head;
}
static ScriptNode *script_parse(const char *text, int depth, ScriptParser *p) {
    if (depth > NESTING_LIMIT) { script_fail(p, 0, "substitution nesting limit exceeded"); return NULL; }
    ScriptNode *head = NULL;
    if (!script_lex(p, text)) head = script_list(p, depth);
    if (!p->error && p->position < p->count) script_fail(p, 0, "unexpected then/elif/else/fi");
    if (p->error) { script_free(head); head = NULL; }
    for (size_t i = 0; i < p->count; i++) free(p->tokens[i].text);
    free(p->tokens);
    p->tokens = NULL;
    return head;
}
static int script_check_text(ScriptParser *parent, const char *text, int depth) {
    ScriptParser p = {0};
    ScriptNode *tree = script_parse(text, depth, &p);
    script_free(tree);
    if (p.error) script_fail(parent, p.error == 1, p.message);
    return p.error;
}
int script_syntax_status(const char *text) {

    char *copy = strdup(text);
    if (!copy) return -1;
    remove_background(copy);
    ScriptParser p = {0};
    ScriptNode *tree = script_parse(copy, 0, &p);
    free(copy);
    script_free(tree);
    return p.error;
}
/* Execute only the selected branch and keep the latest exit status. */
static int script_execute(Shell *shell, ScriptNode *tree, int depth) {
    int status = 0;
    for (ScriptNode *node = tree; node && !shell->exit_requested && !shell_interrupted; node = node->next) {
        if (node->command) status = run_command(shell, node->command, depth);
        else {
            status = 0;
            for (ScriptBranch *b = node->branches; b; b = b->next) {
                int condition = b->condition ? script_execute(shell, b->condition, depth) : 0;
                if (shell->exit_requested) { status = shell->last_status; break; }
                if (!condition) { status = script_execute(shell, b->body, depth); break; }
            }
        }
        shell->last_status = status;
    }
    if (shell_interrupted) { shell->last_status = 130; return 130; }
    return shell->exit_requested ? shell->last_status : status;
}
int script_test(char **argv) {
    int n = 0;
    while (argv[n]) n++;
    if (!strcmp(argv[0], "[")) {
        if (n < 2 || strcmp(argv[n - 1], "]")) { dprintf(2, "terminator: [: missing ]\n"); return 2; }
        n--;
    }
    int first = 1, negate = 0;
    if (first < n && !strcmp(argv[first], "!")) { negate = 1; first++; }
    int count = n - first, truth = 0;
    if (count == 0) truth = 0;
    else if (count == 1) truth = *argv[first] != 0;
    else if (count == 2) {
        const char *op = argv[first], *value = argv[first + 1];
        struct stat st;
        if (!strcmp(op, "-n")) truth = *value != 0;
        else if (!strcmp(op, "-z")) truth = *value == 0;
        else if (!strcmp(op, "-f")) truth = stat(value, &st) == 0 && S_ISREG(st.st_mode);
        else if (!strcmp(op, "-d")) truth = stat(value, &st) == 0 && S_ISDIR(st.st_mode);
        else if (!strcmp(op, "-e")) truth = stat(value, &st) == 0;
        else if (!strcmp(op, "-s")) truth = stat(value, &st) == 0 && st.st_size > 0;
        else if (!strcmp(op, "-r")) truth = access(value, R_OK) == 0;
        else if (!strcmp(op, "-w")) truth = access(value, W_OK) == 0;
        else if (!strcmp(op, "-x")) truth = access(value, X_OK) == 0;
        else goto invalid;
    } else if (count == 3) {
        const char *left = argv[first], *op = argv[first + 1], *right = argv[first + 2];
        if (!strcmp(op, "=") || !strcmp(op, "==")) truth = !strcmp(left, right);
        else if (!strcmp(op, "!=")) truth = strcmp(left, right) != 0;
        else {
            char *end1, *end2;
            errno = 0;
            long long a = strtoll(left, &end1, 10);
            int bad = errno || end1 == left || *end1;
            errno = 0;
            long long b = strtoll(right, &end2, 10);
            if (bad || errno || end2 == right || *end2) goto invalid;
            if (!strcmp(op, "-eq")) truth = a == b;
            else if (!strcmp(op, "-ne")) truth = a != b;
            else if (!strcmp(op, "-gt")) truth = a > b;
            else if (!strcmp(op, "-ge")) truth = a >= b;
            else if (!strcmp(op, "-lt")) truth = a < b;
            else if (!strcmp(op, "-le")) truth = a <= b;
            else goto invalid;
        }
    } else goto invalid;
    return (truth != negate) ? 0 : 1;
invalid:
    dprintf(2, "terminator: test: invalid expression or integer\n");
    return 2;
}
/* ------------------------------------------------------------------ */
/* script execution                                                   */
/* ------------------------------------------------------------------ */
int execute_script_file(Shell *shell, const char *path, char **arguments, int count, int source) {
    static int source_depth;
    if (source_depth >= NESTING_LIMIT) { dprintf(2, "terminator: script nesting limit exceeded\n"); return 2; }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        int error = errno;
        dprintf(2, "terminator: %s: %s\n", path, strerror(error));
        return source ? 1 : error == ENOENT ? 127 : 126;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        dprintf(2, "terminator: script must be a regular file\n");
        return 1;
    }
    char *text = malloc(CAPTURE_SIZE + 1);
    if (!text) { close(fd); return 1; }
    size_t used = 0;
    int error = 0;
    while (used < CAPTURE_SIZE) {
        ssize_t n = read(fd, text + used, CAPTURE_SIZE - used);
        if (n > 0) used += (size_t)n;
        else if (!n) break;
        else if (errno != EINTR) { error = 1; break; }
    }
    char extra;
    ssize_t more = 0;
    if (used == CAPTURE_SIZE) do {
            more = read(fd, &extra, 1);
        } while (more < 0 && errno == EINTR);
    close(fd);
    if (error || more != 0 || memchr(text, 0, used)) {
        free(text);
        dprintf(2, "terminator: script read error, embedded NUL or size limit (1 MiB)\n");
        return 2;
    }
    text[used] = '\0';

    size_t normalized = 0;
    for (size_t i = 0; i < used; i++)
        if (text[i] != '\r' || text[i + 1] != '\n') text[normalized++] = text[i];
    text[normalized] = 0;
    ScriptParser parser = {0};
    /* Validate the entire file before allowing any command to run. */
    ScriptNode *tree = script_parse(text, 0, &parser);
    free(text);
    if (parser.error) {
        dprintf(2, "terminator: %s:%d: syntax error: %s\n", path, parser.line ? parser.line : 1, parser.message);
        return 2;
    }
    source_depth++;
    if (source) {
        char **old_arguments = shell->arguments;
        int old_count = shell->argument_count;
        if (count >= 0) { shell->arguments = arguments; shell->argument_count = count; }
        int status = script_execute(shell, tree, 0);
        shell->arguments = old_arguments; shell->argument_count = old_count;
        script_free(tree);
        source_depth--;
        return status;
    }
    pid_t pid = fork();
    if (pid == 0) {
        Shell inner = *shell;
        inner.exit_requested = 0; inner.arguments = arguments;
        inner.argument_count = count; inner.argument_zero = path;
        inner.last_status = 0;
        inherited_environment(shell);
        memset(&inner.variables, 0, sizeof(inner.variables));
        int status = script_execute(&inner, tree, 0);
        script_free(tree);
        _exit(status);
    }
    int status = 1;
    if (pid < 0) dprintf(2, "terminator: script fork: %s\n", strerror(errno));
    else {
        int result;
        pid_t waited = shell_wait(pid, &result);
        if (waited > 0) status = WIFEXITED(result) ? WEXITSTATUS(result) : WIFSIGNALED(result) ? 128 + WTERMSIG(result) : 1;
    }
    script_free(tree);
    source_depth--;
    return status;
}
int script_path(const char *name) {
    if (!strchr(name, '/')) return 0;
    size_t length = strlen(name);
    if (length >= 3 && !strcmp(name + length - 3, ".sh")) return 1;
    int fd = open(name, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) { close(fd); return 0; }
    char line[128];
    ssize_t n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) return 0;
    line[n] = 0;
    char *end = strchr(line, '\n');
    if (end) *end = 0;
    return !strncmp(line, "#!", 2) && (strstr(line, "bash") || strstr(line, "/sh"));
}

int run_shell(Shell *shell, const char *command, int depth) {
    ScriptParser parser = {0};
    ScriptNode *tree = script_parse(command, depth, &parser);
    if (parser.error) {
        dprintf(2, "terminator: syntax error at line %d: %s\n", parser.line ? parser.line : 1, parser.message);
        shell->last_status = 2;
        return 2;
    }
    int status = script_execute(shell, tree, depth);
    script_free(tree);
    shell->last_status = status;
    return status;
}
