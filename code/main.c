/*
 * Starts Terminator and reads the execution mode.
 * Initializes shared history and runtime resources.
 * Opens the GUI or starts an internal command runner.
 */
#include "terminator.h"

int process_mode;
char executable_path[4096];
int registry_fd = -1;

pthread_mutex_t descriptor_mutex = PTHREAD_MUTEX_INITIALIZER;
TabJobRegistry *job_registries;
int gui_fd = -1;
long descriptor_limit;
char initial_cwd[PATH_MAX];

#ifndef TERMINATOR_HEADLESS_TEST
/* ------------------------------------------------------------------ */
/* Execution modes - thread and process workers                       */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--terminator-internal-runner")) return internal_runner();

    if (argc != 1 && !(argc == 2 && !strcmp(argv[1], "useProcess=True"))) {
        fprintf(stderr, "Usage: %s [useProcess=True]\n", argv[0]);
        return 1;
    }
    /* Default: one thread per tab. useProcess=True selects process workers. */
    process_mode = argc == 2;
#ifdef __APPLE__
    char raw_path[4096];
    uint32_t raw_size = sizeof(raw_path);
    if (_NSGetExecutablePath(raw_path, &raw_size) != 0 || !realpath(raw_path, executable_path)) {
        perror("executable path");
        return 1;
    }
#else
    ssize_t executable_size = readlink("/proc/self/exe", executable_path, sizeof(executable_path) - 1);
    if (executable_size < 0 || (size_t)executable_size >= sizeof(executable_path) - 1) {
        fprintf(stderr, "Cannot resolve executable path.\n");
        return 1;
    }
    executable_path[executable_size] = '\0';
#endif
    signal(SIGPIPE, SIG_IGN);
    descriptor_limit = sysconf(_SC_OPEN_MAX);
    if (descriptor_limit < 0) descriptor_limit = 1024;
    if (!getcwd(initial_cwd, sizeof(initial_cwd)) || history_init() == -1) { perror("startup"); return 1; }
    return gui_run();
}
#endif
