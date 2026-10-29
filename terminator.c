/* Terminator: process-mode shell with Tasks 6, 7 and 8 extensions.
 * Build on Linux: cc -std=c11 -Wall -Wextra terminator.c -o terminator -lX11
 * Build on macOS: add -I/opt/X11/include -L/opt/X11/lib
 *                    -Wl,-rpath,/opt/X11/lib
 * Process workers only: default thread mode and source remain outside this
 * update. Basic command stdin is line-buffered through a pipe;
 * this is not a PTY/VT terminal implementation for programs such as vim.
 *
 * EDITING DEFAULTS:
 * Left/Right: move; Ctrl+A/E: start/end; Up/Down: shared history;
 * Ctrl+U/K: delete before/after cursor; Ctrl+W: delete previous word;
 * Ctrl+L: clear this tab's scrollback; Ctrl+D: close an empty idle tab,
 * delete under the cursor on a nonempty idle line, or close a running
 * command's stdin after delivering any pending input; Enter sends an input
 * line to a running command. Ctrl+C interrupts that tab's command group.
 * Ctrl+Shift+C/V: X11 clipboard copy/paste. Drag output to select it.
 * Without an output selection, copy uses the current input, or scrollback
 * when input is empty. Pasting inserts text and NEVER executes it by itself.
 * Pasted newlines are displayed as '~' in the one-line editor and are kept
 * as actual newlines when Enter submits the command. ASCII text only.
 * Ctrl+T: new tab; Ctrl+Shift+W: close; Ctrl+Tab/Ctrl+Shift+Tab: switch.
 *
 * CUSTOMIZATION (no recompilation): edit ~/.terminatorrc, for example:
 *     line_start=Ctrl+B
 *     close_tab=Ctrl+Shift+Q
 * Run "bindings reload" after saving; restarting also loads the file.
 * "bindings" lists effective actions/keys. Use action names shown there.
 * Keys use X11 names (Left, Right, Up, Down, Home, End, Return, etc.) with
 * optional Ctrl+, Shift+, Alt+, Super+. Blank lines and # comments allowed.
 * Invalid, duplicate and conflicting entries generate window warnings and
 * fall back to defaults. Config swaps are resolved as a complete mapping.
 * TERMINATOR_CONFIG optionally specifies an alternative config pathname.
 * Input limit: 8190 bytes per complete command, including continuation text.
 *
 * JOB CONTROL:
 * Ctrl+Z (config action "suspend") stops the foreground job and returns the
 * prompt. "bg %1" continues a stopped job; "fg %1" brings a job forward.
 * "jobs" lists this tab's jobs; without an argument fg/bg choose the newest.
 * A final unquoted '&' starts one command or pipeline in the background.
 * Ctrl+C signals only the foreground logical job; other jobs/tabs continue.
 * Uses SIGSTOP for X11-driven suspension (no controlling PTY required), and
 * SIGCONT to resume. multiWatch subsidiary groups are part of the same job.
 * jobs/fg/bg are standalone worker built-ins, not pipeline stages. Up to 64
 * active jobs per tab and 256 multiWatch subsidiary groups per job.
 * Newly started background jobs receive EOF on stdin unless '<' overrides
 * it; a suspended foreground job keeps its input pipe when moved to bg/fg.
 * Closing a tab kills that tab's jobs, including stopped/background jobs.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#ifndef TERMINATOR_HEADLESS_TEST
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#endif
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
#include <semaphore.h>
#include <limits.h>
#include <glob.h>
#include <time.h>
#include <stdatomic.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_TABS 8
#define MAX_JOBS 64
#define JOB_GROUPS 256
typedef struct { atomic_int groups[JOB_GROUPS]; } JobGroups;
typedef struct { atomic_int roots[MAX_JOBS];JobGroups groups[MAX_JOBS]; } TabJobRegistry;
static TabJobRegistry *job_registries;
#define INPUT_SIZE 508
#define COMMAND_SIZE 8192
#define QUEUE_SIZE 64
#define MAX_LINES 1000
#define LINE_SIZE 600
#define HISTORY_SIZE 1000
#define PAYLOAD_SIZE PATH_MAX

enum { REQUEST_COMMAND=1, REQUEST_INTERRUPT, REQUEST_STOP, REQUEST_SET_STATUS,
       REQUEST_FRAGMENT, REQUEST_INPUT, REQUEST_EOF, REQUEST_SUSPEND };
enum { REPLY_OUTPUT=1, REPLY_DONE, REPLY_CWD, REPLY_STARTED };
typedef struct { int type; char text[INPUT_SIZE]; } Request;
/* POSIX guarantees atomic pipe writes of at least 512 bytes. macOS uses
 * that minimum, so the whole request (including its type) must fit. */
_Static_assert(sizeof(Request) <= 512, "Request exceeds portable PIPE_BUF");
typedef struct { int type, value; char text[PAYLOAD_SIZE]; } Reply;
typedef struct { int count; char entries[HISTORY_SIZE][COMMAND_SIZE]; } History;

#define HISTORY_TEXT_SIZE (HISTORY_SIZE*(COMMAND_SIZE+16))
typedef struct { char text[HISTORY_TEXT_SIZE]; } HistorySnapshot;
static HistorySnapshot *history_snapshots;
static History *history_data;
static sem_t *history_sem = SEM_FAILED;
static int gui_fd = -1;
static long descriptor_limit;
static char initial_cwd[PATH_MAX];

/* GUI is the sole history writer and lock holder, so a worker crash cannot
 * strand the history lock. In process mode the table is shared memory and
 * uses a named POSIX semaphore (macOS does not support unnamed sem_init).
 */
static void history_lock(void)
{
    while (sem_wait(history_sem) == -1 && errno == EINTR) {}
}
static void history_unlock(void)
{
    sem_post(history_sem);
}
static int history_init(void)
{
    history_data = mmap(NULL, sizeof(*history_data), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (history_data == MAP_FAILED) { history_data=NULL; return -1; }
    history_snapshots=mmap(NULL,MAX_TABS*sizeof(*history_snapshots),PROT_READ|PROT_WRITE,
                           MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(history_snapshots==MAP_FAILED) {
        history_snapshots=NULL;munmap(history_data,sizeof(*history_data));history_data=NULL;return -1;
    }
    char name[64];
    snprintf(name, sizeof(name), "/terminator-history-%ld", (long)getpid());
    history_sem=sem_open(name, O_CREAT | O_EXCL, 0600, 1);
    if (history_sem == SEM_FAILED) {
        munmap(history_data,sizeof(*history_data)); history_data=NULL;
        munmap(history_snapshots,MAX_TABS*sizeof(*history_snapshots));history_snapshots=NULL;
        return -1;
    }
    sem_unlink(name);
    job_registries=mmap(NULL,MAX_TABS*sizeof(*job_registries),PROT_READ|PROT_WRITE,
                        MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(job_registries==MAP_FAILED) {
        job_registries=NULL;sem_close(history_sem);
        munmap(history_data,sizeof(*history_data));history_data=NULL;
        munmap(history_snapshots,MAX_TABS*sizeof(*history_snapshots));history_snapshots=NULL;
        return -1;
    }
    for(int t=0;t<MAX_TABS;t++)for(int j=0;j<MAX_JOBS;j++) {
        atomic_init(&job_registries[t].roots[j],0);
        for(int g=0;g<JOB_GROUPS;g++)atomic_init(&job_registries[t].groups[j].groups[g],0);
    }
    return 0;
}
static void history_add(const char *command)
{
    history_lock();
    if (history_data->count == HISTORY_SIZE) {
        memmove(history_data->entries, history_data->entries+1,
                (HISTORY_SIZE-1)*sizeof(history_data->entries[0]));
        history_data->count--;
    }
    snprintf(history_data->entries[history_data->count++], COMMAND_SIZE,
             "%s",command);
    history_unlock();
}
/* The GUI freezes one history view per command before sending its request.
 * That tab's runner/substitutions can read it without taking a semaphore;
 * the GUI rewrites it only after REPLY_DONE. Killing a runner cannot strand
 * the shared history lock. Only the actual history text is copied. */
static void history_snapshot(int slot)
{
    history_lock();size_t used=0;
    char *text=history_snapshots[slot].text;text[0]='\0';
    for(int i=0;i<history_data->count;i++) {
        int n=snprintf(text+used,HISTORY_TEXT_SIZE-used,"%d  %s\n",i+1,history_data->entries[i]);
        if(n<0 || (size_t)n>=HISTORY_TEXT_SIZE-used)break;
        used+=(size_t)n;
    }
    history_unlock();
}
static void history_destroy(void)
{
    sem_close(history_sem);
    munmap(history_data,sizeof(*history_data));
    munmap(history_snapshots,MAX_TABS*sizeof(*history_snapshots));
    munmap(job_registries,MAX_TABS*sizeof(*job_registries));
}

static int set_flags(int fd, int nonblock)
{
    int f=fcntl(fd,F_GETFD);
    if (f==-1 || fcntl(fd,F_SETFD,f|FD_CLOEXEC)==-1) return -1;
    if (nonblock) {
        f=fcntl(fd,F_GETFL);
        if (f==-1 || fcntl(fd,F_SETFL,f|O_NONBLOCK)==-1) return -1;
    }
    return 0;
}
static int make_pipe(int p[2], int nonblocking_read)
{
    if (pipe(p)==-1) return -1;
    if (set_flags(p[0],nonblocking_read)==-1 || set_flags(p[1],0)==-1) {
        int error=errno; close(p[0]); close(p[1]); errno=error; return -1;
    }
    return 0;
}
static int write_all(int fd, const void *data, size_t size)
{
    const char *p=data;
    while (size) {
        ssize_t n=write(fd,p,size);
        if (n>0) { p+=n; size-=(size_t)n; }
        else if (n==-1 && errno==EINTR) continue;
        else return -1;
    }
    return 0;
}

typedef struct {
    int request_fd, reply_fd, directory_fd, slot;
    char cwd[PATH_MAX];
} Worker;
static int reply_send(Worker *w, int type, int value, const char *text)
{
    Reply r={0}; r.type=type; r.value=value;
    if (text) snprintf(r.text,sizeof(r.text),"%s",text);
    return write_all(w->reply_fd,&r,sizeof(r));
}
static int worker_error(Worker *w, const char *operation)
{
    char message[PAYLOAD_SIZE];
    snprintf(message,sizeof(message),"terminator: %s: %s\n",operation,strerror(errno));
    return reply_send(w,REPLY_OUTPUT,0,message);
}
static int change_directory(Worker *w, const char *path)
{
    char candidate[PATH_MAX],resolved[PATH_MAX];
    if (!path) path=getenv("HOME");
    if (!path) { errno=ENOENT; return -1; }
    int n;
    if (path[0]=='/') n=snprintf(candidate,sizeof(candidate),"%s",path);
    else n=snprintf(candidate,sizeof(candidate),"%s/%s",w->cwd,path);
    if (n<0 || (size_t)n>=sizeof(candidate)) { errno=ENAMETOOLONG; return -1; }
    if (!realpath(candidate,resolved)) return -1;
    int fd=open(resolved,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd==-1) return -1;
    if (set_flags(fd,0)==-1) { int e=errno; close(fd); errno=e; return -1; }
    close(w->directory_fd); w->directory_fd=fd;
    snprintf(w->cwd,sizeof(w->cwd),"%s",resolved);
    return 0;
}
#define EXPANSION_SIZE 65536
#define CAPTURE_SIZE (1024*1024)
#define ARGUMENT_LIMIT 1024
#define NESTING_LIMIT 32

typedef struct { char cwd[PATH_MAX]; int last_status,exit_requested;const char *history_text; } Shell;
typedef struct { int exit_requested; char cwd[PATH_MAX]; } ShellResult;
static int run_shell(Shell *shell,const char *command,int depth);
static int command_completeness(const char *text);

/* multiWatch uses subsidiary process groups. Publish them in lock-free shared
 * memory so the tab worker can stop/resume/clean the complete logical job. */
static JobGroups *active_job_groups;
static int job_group_add(pid_t group)
{
    if(!active_job_groups)return 0;
    for(int i=0;i<JOB_GROUPS;i++) {
        int empty=0;
        if(atomic_compare_exchange_strong(&active_job_groups->groups[i],&empty,(int)group))return 0;
    }
    errno=E2BIG;return -1;
}
static void job_group_remove(pid_t group)
{
    if(active_job_groups)for(int i=0;i<JOB_GROUPS;i++) {
        int old=(int)group;
        atomic_compare_exchange_strong(&active_job_groups->groups[i],&old,0);
    }
}

/* multiWatch owns one independently cancellable process group per command.
 * Its signal handler only sets a flag; cleanup runs in ordinary code. */
static volatile sig_atomic_t watch_stop;
static void watch_signal(int number) { watch_stop=number; }
typedef struct {
    char *command;
    pid_t pid, group;
    int fd, finished;
    long long next_run;
} WatchCommand;
static long long watch_now(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)==-1)return -1;
    return (long long)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static void watch_free(WatchCommand *items,size_t count)
{
    for(size_t i=0;i<count;i++)free(items[i].command);
    free(items);
}
/* Parse the outer list without expanding or stripping the inner shell syntax.
 * Inside a list string, \" and \\ encode a literal quote/backslash.
 * Other backslashes are preserved for the inner shell (e.g. printf '\n'). */
static int watch_parse(const char *text,WatchCommand **out,size_t *count)
{
    *out=NULL;*count=0;
    while(*text==' ' || *text=='\t' || *text=='\n')text++;
    if(*text++!='[')return -1;
    for(;;) {
        while(*text==' ' || *text=='\t' || *text=='\n')text++;
        if(*text!='"')goto invalid;
        text++;
        char *command=malloc(strlen(text)+1);
        if(!command)goto invalid;
        size_t used=0;
        while(*text && *text!='"') {
            if(*text=='\\' && (text[1]=='"' || text[1]=='\\'))text++;
            command[used++]=*text++;
        }
        if(*text!='"'){free(command);goto invalid;}
        text++;command[used]='\0';
        size_t nonspace=strspn(command," \t\n");
        if(!command[nonspace]){free(command);goto invalid;}
        WatchCommand *grown=realloc(*out,(*count+1)*sizeof(**out));
        if(!grown){free(command);goto invalid;}
        *out=grown;
        (*out)[*count]=(WatchCommand){.command=command,.pid=-1,.group=-1,.fd=-1};
        (*count)++;
        while(*text==' ' || *text=='\t' || *text=='\n')text++;
        if(*text==','){text++;continue;}
        if(*text!=']')goto invalid;
        text++;
        while(*text==' ' || *text=='\t' || *text=='\n')text++;
        if(*text)goto invalid;
        return 0;
    }
invalid:
    watch_free(*out,*count);*out=NULL;*count=0;return -1;
}
static int watch_launch(WatchCommand *items,size_t count,size_t index,
                        Shell *shell,int depth)
{
    int p[2];if(make_pipe(p,1)==-1)return -1;
    pid_t pid=fork();
    if(pid==0) {
        close(p[0]);
        if(setpgid(0,0)==-1)_exit(126);
        struct sigaction action={0};action.sa_handler=SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT,&action,NULL);sigaction(SIGTERM,&action,NULL);
        sigaction(SIGHUP,&action,NULL);sigaction(SIGPIPE,&action,NULL);
        for(size_t i=0;i<count;i++)if(items[i].fd>=0)close(items[i].fd);
        /* Capture normal output and diagnostics through this command's pipe. */
        if(dup2(p[1],1)==-1 || dup2(p[1],2)==-1)_exit(126);
        if(p[1]>2)close(p[1]);
        Shell inner=*shell;inner.exit_requested=0;
        _exit(run_shell(&inner,items[index].command,depth+1));
    }
    int error=errno;close(p[1]);
    if(pid==-1){close(p[0]);errno=error;return -1;}
    /* Both sides establish the group, avoiding a parent/child scheduling race. */
    if(setpgid(pid,pid)==-1 && errno!=EACCES && errno!=ESRCH) {
        error=errno;kill(pid,SIGKILL);close(p[0]);
        while(waitpid(pid,NULL,0)==-1 && errno==EINTR){}
        errno=error;return -1;
    }
    if(job_group_add(pid)==-1) {
        kill(-pid,SIGKILL);kill(pid,SIGKILL);close(p[0]);
        while(waitpid(pid,NULL,0)==-1 && errno==EINTR){}
        return -1;
    }
    items[index].pid=pid;items[index].group=pid;
    items[index].fd=p[0];items[index].finished=0;return 0;
}
static int watch_print(const char *command,const char *data,size_t length)
{
    static const char rule[]="----------------------------------------------------\n";
    if(dprintf(1,"\"%s\", %lld:\n",command,(long long)time(NULL))<0 ||
       write_all(1,rule,sizeof(rule)-1)==-1 || write_all(1,data,length)==-1)return -1;
    if(length && data[length-1]!='\n' && write_all(1,"\n",1)==-1)return -1;
    return write_all(1,rule,sizeof(rule)-1);
}
static int execute_multiwatch(Shell *shell,const char *list,int depth)
{
    WatchCommand *items=NULL;size_t count=0;
    if(watch_parse(list,&items,&count)==-1) {
        dprintf(2,"Usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n");return 2;
    }
    /* Validate all entries before starting any of them. */
    for(size_t i=0;i<count;i++)if(command_completeness(items[i].command)) {
        dprintf(2,"multiWatch: incomplete or unsupported command: %s\n",items[i].command);
        watch_free(items,count);return 2;
    }
    struct pollfd *fds=calloc(count,sizeof(*fds));
    if(!fds){watch_free(items,count);return 1;}
    struct sigaction action={0},old_int,old_term,old_hup,old_pipe;
    action.sa_handler=watch_signal;sigemptyset(&action.sa_mask);
    watch_stop=0;
    sigaction(SIGINT,&action,&old_int);sigaction(SIGTERM,&action,&old_term);
    sigaction(SIGHUP,&action,&old_hup);
    struct sigaction ignore={0};ignore.sa_handler=SIG_IGN;sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE,&ignore,&old_pipe);
    int status=0;
    while(!watch_stop) {
        long long now=watch_now();if(now<0){status=1;break;}
        int timeout=-1;
        for(size_t i=0;i<count && !watch_stop;i++) {
            WatchCommand *w=&items[i];
            if(w->pid>0) {
                pid_t result=waitpid(w->pid,NULL,WNOHANG);
                if(result==w->pid){w->pid=-1;w->finished=1;w->next_run=now+2000;}
                else if(result==-1 && errno!=EINTR){status=1;goto done;}
            }
            if(w->pid<0 && w->fd<0 && now>=w->next_run) {
                /* Remove any leftover descendants from the preceding run. */
                if(w->group>0){kill(-w->group,SIGKILL);job_group_remove(w->group);}
                w->group=-1;
                if(watch_launch(items,count,i,shell,depth)==-1) {
                    dprintf(2,"multiWatch: launch: %s\n",strerror(errno));status=1;goto done;
                }
            }
            fds[i]=(struct pollfd){w->fd,POLLIN,0};
            if(w->pid<0 && w->fd<0) {
                long long delay=w->next_run-now;
                int ms=delay<=0?0:delay>INT_MAX?INT_MAX:(int)delay;
                if(timeout<0 || ms<timeout)timeout=ms;
            }
            /* A child may close stdout before it exits; periodically reap it.
             * This is a blocking timed poll, not a busy loop. */
            if(w->pid>0 && (timeout<0 || timeout>100))timeout=100;
        }
        if(watch_stop)break;
        int ready=poll(fds,(nfds_t)count,timeout);
        if(ready<0){if(errno==EINTR)continue;status=1;break;}
        for(size_t i=0;i<count && !watch_stop;i++) {
            if(!(fds[i].revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL)))continue;
            char buffer[4096];
            ssize_t n=read(items[i].fd,buffer,sizeof(buffer));
            if(n>0) {
                /* Each arriving chunk is a complete, non-interleaved block. */
                if(watch_print(items[i].command,buffer,(size_t)n)==-1){status=1;goto done;}
            } else if(n==0) {close(items[i].fd);items[i].fd=-1;}
            else if(errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK){status=1;goto done;}
        }
    }
done:
    /* Stop every command's complete process group, including pipeline stages.
     * SIGKILL also handles monitored programs that ignore Ctrl+C/SIGTERM. */
    for(size_t i=0;i<count;i++) {
        if(items[i].group>0){kill(-items[i].group,SIGKILL);job_group_remove(items[i].group);}
        if(items[i].fd>=0)close(items[i].fd);
    }
    for(size_t i=0;i<count;i++)if(items[i].pid>0)
        while(waitpid(items[i].pid,NULL,0)==-1 && errno==EINTR){}
    if(watch_stop==SIGINT){write_all(1,"^C\n",3);status=130;}
    else if(watch_stop)status=128+watch_stop;
    sigaction(SIGINT,&old_int,NULL);sigaction(SIGTERM,&old_term,NULL);
    sigaction(SIGHUP,&old_hup,NULL);
    sigaction(SIGPIPE,&old_pipe,NULL);
    free(fds);watch_free(items,count);return status;
}

/* Scan without executing expansions. Substitutions have their own quote
 * context. Return 1 for an incomplete line, -1 for invalid/unsupported syntax.
 */
static int scan_context(const char *text,size_t *position,int terminator,int depth)
{
    if(depth>NESTING_LIMIT)return -1;
    int quote=0,word=0,after_pipe=0;
    for(size_t i=*position;text[i];) {
        unsigned char c=(unsigned char)text[i];
        if(quote=='\'') {
            if(c=='\'')quote=0;
            i++;continue;
        }
        if(c=='\\') {
            if(!text[i+1])return 1;
            if(text[i+1]!='\n')word=1;
            if(quote=='"' && !strchr("$`\"\\\n",text[i+1]))i++;
            else i+=2;
            continue;
        }
        if(c=='$' && text[i+1]=='(') {
            word=1;
            size_t inner=i+2;
            int result=scan_context(text,&inner,')',depth+1);
            if(result)return result;
            i=inner;continue;
        }
        if(c=='`') {
            word=1;
            size_t end=i+1;
            for(;text[end] && text[end]!='`';end++) {
                if(text[end]=='\\') {
                    if(!text[end+1])return 1;
                    end++;
                }
            }
            if(!text[end])return 1;
            /* Backtick body is checked after its escape processing. */
            i=end+1;continue;
        }
        if(c=='$' && text[i+1]=='{') {
            word=1;
            size_t end=i+2;
            while(text[end] && text[end]!='}')end++;
            if(!text[end])return 1;
            i=end+1;continue;
        }
        if(c=='"'){word=1;quote=quote=='"'?0:'"';i++;continue;}
        if(!quote && c=='\''){word=1;quote='\'';i++;continue;}
        if(!quote && terminator && c==(unsigned char)terminator) {
            if(after_pipe && !word)return -1;
            *position=i+1;return 0;
        }
        if(!quote && c=='|') {
            if(!word)return -1;
            word=0;after_pipe=1;i++;continue;
        }
        if(!quote && !terminator && c=='&' && word &&
           !text[i+1+strspn(text+i+1," \t\n")])return 0;
        if(!quote && strchr("&;()",c))return -1;
        if(!quote && c=='\n' && !(after_pipe && !word)){word=0;after_pipe=0;}
        else if(c!=' ' && c!='\t' && c!='\n')word=1;
        i++;
    }
    return quote || terminator || (after_pipe && !word) ? 1 : 0;
}
static int command_completeness(const char *text)
{
    size_t position=0;return scan_context(text,&position,0,0);
}
static int extract_substitution(const char *text,size_t *position,int backtick,char **body)
{
    size_t start=*position+(backtick?1:2),end;
    if(backtick) {
        end=start;
        while(text[end] && text[end]!='`') {
            if(text[end]=='\\' && text[end+1])end++;
            end++;
        }
        if(!text[end])return -1;
        *body=malloc(end-start+1);if(!*body)return -1;
        size_t used=0;
        for(size_t i=start;i<end;i++) {
            if(text[i]=='\\' && i+1<end && strchr("$`\\",text[i+1]))i++;
            (*body)[used++]=text[i];
        }
        (*body)[used]='\0';*position=end+1;
    } else {
        end=start;
        if(scan_context(text,&end,')',0))return -1;
        *body=strndup(text+start,end-start-1);
        if(!*body)return -1;
        *position=end;
    }
    return 0;
}
static char *capture_substitution(Shell *shell,const char *body,int depth,int *sub_status)
{
    int p[2];if(make_pipe(p,0)==-1)return NULL;
    pid_t pid=fork();
    if(pid==0) {
        close(p[0]);
        if(dup2(p[1],STDOUT_FILENO)==-1)_exit(126);
        if(p[1]>2)close(p[1]);
        Shell inner=*shell;inner.exit_requested=0;
        int status=run_shell(&inner,body,depth+1);
        _exit(status);
    }
    close(p[1]);
    if(pid==-1){close(p[0]);return NULL;}
    char *captured=malloc(CAPTURE_SIZE+1);
    size_t used=0;int failed=!captured;
    char buffer[4096];
    for(;;) {
        ssize_t n=read(p[0],buffer,sizeof(buffer));
        if(n==0)break;
        if(n<0){if(errno==EINTR)continue;failed=1;break;}
        if(!failed) {
            if(used+(size_t)n>CAPTURE_SIZE)failed=1;
            else {memcpy(captured+used,buffer,(size_t)n);used+=(size_t)n;}
        }
    }
    close(p[0]);
    int status=0;pid_t waited;
    do{waited=waitpid(pid,&status,0);}while(waited==-1 && errno==EINTR);
    *sub_status=waited==-1?1:WIFEXITED(status)?WEXITSTATUS(status):
                WIFSIGNALED(status)?128+WTERMSIG(status):1;
    if(failed){free(captured);errno=E2BIG;return NULL;}
    while(used && captured[used-1]=='\n')used--;
    captured[used]='\0';return captured;
}

typedef struct {
    char text[EXPANSION_SIZE];
    unsigned char split[EXPANSION_SIZE],empty_quote[EXPANSION_SIZE],protected[EXPANSION_SIZE];
    size_t used;
} ExpandedWord;
static int word_add(ExpandedWord *word,const char *text,int split)
{
    size_t length=strlen(text);
    if(word->used+length>=EXPANSION_SIZE)return -1;
    memcpy(word->text+word->used,text,length);
    memset(word->split+word->used,split,(size_t)length);
    memset(word->protected+word->used,!split,(size_t)length);
    word->used+=length;word->text[word->used]='\0';return 0;
}
static int char_add(ExpandedWord *word,char c)
{
    char text[2]={c,0};return word_add(word,text,0);
}
static int argument_add(char **argv,int *argc,const char *text,size_t length)
{
    if(*argc>=ARGUMENT_LIMIT)return -1;
    argv[*argc]=strndup(text,length);if(!argv[*argc])return -1;
    (*argc)++;argv[*argc]=NULL;return 0;
}
static int word_argument_add(char **argv,int *argc,const ExpandedWord *word,size_t start,size_t length)
{
    int wildcard=0;
    for(size_t i=start;i<start+length;i++)
        if(!word->protected[i] && strchr("*?[",word->text[i]))wildcard=1;
    if(!wildcard)return argument_add(argv,argc,word->text+start,length);
    char *pattern=malloc(2*length+1);if(!pattern)return -1;
    size_t used=0;
    for(size_t i=start;i<start+length;i++) {
        char c=word->text[i];
        if(c=='\\' || (word->protected[i] && strchr("*?[]",c)))pattern[used++]='\\';
        pattern[used++]=c;
    }
    pattern[used]='\0';glob_t matches={0};
    int result=glob(pattern,0,NULL,&matches);free(pattern);
    if(result==GLOB_NOMATCH){globfree(&matches);return argument_add(argv,argc,word->text+start,length);}
    if(result){globfree(&matches);return -1;}
    for(size_t i=0;i<matches.gl_pathc;i++)
        if(argument_add(argv,argc,matches.gl_pathv[i],strlen(matches.gl_pathv[i]))==-1){globfree(&matches);return -1;}
    globfree(&matches);return 0;
}
static void arguments_free(char **argv)
{
    if(argv){for(int i=0;argv[i];i++)free(argv[i]);free(argv);}
}
static int expansion(Shell *shell,const char *text,size_t *position,
                     ExpandedWord *word,int quoted,int depth,int *sub_status)
{
    size_t i=*position;
    if(text[i]=='`' || (text[i]=='$' && text[i+1]=='(')) {
        char *body=NULL;
        if(extract_substitution(text,position,text[i]=='`',&body)==-1)return -1;
        char *value=capture_substitution(shell,body,depth,sub_status);free(body);
        if(!value)return -1;
        int result=word_add(word,value,!quoted);free(value);return result;
    }
    if(text[i]!='$')return -1;
    i++;char name[256];size_t used=0;int braced=text[i]=='{';
    if(braced)i++;
    if(text[i]=='?'){name[used++]='?';i++;}
    else {
        if(!((text[i]>='A' && text[i]<='Z') || (text[i]>='a' && text[i]<='z') || text[i]=='_')) {
            if(braced)return -1;
            *position=i;return char_add(word,'$');
        }
        while((text[i]>='A' && text[i]<='Z') || (text[i]>='a' && text[i]<='z') ||
              (text[i]>='0' && text[i]<='9') || text[i]=='_') {
            if(used>=sizeof(name)-1)return -1;
            name[used++]=text[i++];
        }
    }
    if(braced){if(text[i]!='}')return -1;i++;}
    name[used]='\0';*position=i;
    char status_text[32];const char *value;
    if(!strcmp(name,"?")){snprintf(status_text,sizeof(status_text),"%d",shell->last_status);value=status_text;}
    else if(!strcmp(name,"PWD"))value=shell->cwd;
    else value=getenv(name);
    return word_add(word,value?value:"",!quoted);
}
typedef struct { int target;char *path; } Redirection;
typedef struct { Redirection items[ARGUMENT_LIMIT];int count; } Redirections;
static void redirections_free(Redirections *redirections)
{
    for(int i=0;i<redirections->count;i++)free(redirections->items[i].path);
    redirections->count=0;
}
/* Each open creates a separate file description, including for two tabs
 * reading the same input. Apply redirections in their written order. */
static int redirections_apply(const Redirections *redirections,int saved[2])
{
    saved[0]=saved[1]=-1;
    if(!redirections->count)return 0;
    for(int i=0;i<2;i++) {
        saved[i]=dup(i);
        if(saved[i]<0 || set_flags(saved[i],0)==-1)goto failure;
    }
    for(int i=0;i<redirections->count;i++) {
        const Redirection *r=&redirections->items[i];
        int flags=r->target==0?O_RDONLY:O_WRONLY|O_CREAT|O_TRUNC;
        int fd=open(r->path,flags,0666);
        if(fd<0){dprintf(2,"terminator: %s: %s\n",r->path,strerror(errno));return -1;}
        if(dup2(fd,r->target)<0){close(fd);goto failure;}
        close(fd);
    }
    return 0;
failure:
    dprintf(2,"terminator: redirection: %s\n",strerror(errno));return -1;
}
static void redirections_restore(int saved[2])
{
    for(int i=0;i<2;i++)if(saved[i]>=0){dup2(saved[i],i);close(saved[i]);saved[i]=-1;}
}
static char **parse_arguments(Shell *shell,const char *text,int depth,int *sub_status,
                              Redirections *redirections)
{
    char **argv=calloc(ARGUMENT_LIMIT+1,sizeof(*argv));
    ExpandedWord *word=malloc(sizeof(*word));
    if(!argv || !word){free(argv);free(word);return NULL;}
    int argc=0,pending=-1;size_t i=0;
    while(text[i]) {
        while(text[i]==' ' || text[i]=='\t' || text[i]=='\n')i++;
        if(!text[i])break;
        if(text[i]=='<' || text[i]=='>') {
            if(pending>=0 || text[i+1]=='<' || text[i+1]=='>')goto syntax_error;
            pending=text[i]=='<'?0:1;i++;continue;
        }
        int before=argc;
        memset(word,0,sizeof(*word));int quote=0;
        if(text[i]=='~' && (text[i+1]=='/' || !text[i+1] || text[i+1]==' ' || text[i+1]=='\t')) {
            const char *home=getenv("HOME");
            if(home){if(word_add(word,home,0)==-1)goto error;i++;}
        }
        while(text[i]) {
            char c=text[i];
            if(quote=='\'') {
                if(c=='\''){quote=0;word->empty_quote[word->used]=1;}
                else if(char_add(word,c)==-1)goto error;
                i++;continue;
            }
            if(c=='\\') {
                char next=text[i+1];if(!next)goto error;
                if(quote=='"' && !strchr("$`\"\\\n",next)) {
                    if(char_add(word,c)==-1)goto error;
                    i++;continue;
                }
                if(next!='\n' && char_add(word,next)==-1)goto error;
                i+=2;continue;
            }
            if(c=='"'){quote=quote=='"'?0:'"';word->empty_quote[word->used]=1;i++;continue;}
            if(!quote && c=='\''){quote='\'';word->empty_quote[word->used]=1;i++;continue;}
            if(c=='$' || c=='`') {
                if(expansion(shell,text,&i,word,quote=='"',depth,sub_status)==-1)goto error;
                continue;
            }
            if(!quote && (c==' ' || c=='\t' || c=='\n'))break;
            if(!quote && (c=='<' || c=='>'))break;
            if(!quote && strchr("|&;()",c))goto syntax_error;
            if(char_add(word,c)==-1)goto error;
            if(!quote)word->protected[word->used-1]=0;
            i++;
        }
        if(quote)goto error;
        size_t start=0;int preserve_empty=0;
        for(size_t j=0;j<=word->used;j++) {
            if(word->empty_quote[j])preserve_empty=1;
            int delimiter=j==word->used || (word->split[j] && strchr(" \t\n",word->text[j]));
            if(delimiter) {
                if((j>start || preserve_empty) && word_argument_add(argv,&argc,word,start,j-start)==-1)goto error;
                start=j+1;preserve_empty=0;
            }
        }
        if(pending>=0) {
            if(argc-before!=1 || redirections->count>=ARGUMENT_LIMIT)goto syntax_error;
            Redirection *r=&redirections->items[redirections->count++];
            r->target=pending;r->path=argv[before];argv[before]=NULL;argc=before;pending=-1;
        }
    }
    if(pending>=0)goto syntax_error;
    free(word);return argv;
syntax_error:
    dprintf(2,"terminator: invalid redirection; use < filename or > filename (one filename)\n");
error:
    free(word);arguments_free(argv);return NULL;
}
/* Newlines outside quotes/substitutions separate commands. A backslash-
 * newline pair is removed by the argument parser instead. */
static size_t next_separator(const char *text,size_t start,int include_pipe)
{
    int quote=0,after_pipe=0;size_t i=start;
    while(text[i]) {
        char c=text[i];
        if(quote=='\''){if(c=='\'')quote=0;i++;continue;}
        if(c!=' ' && c!='\t' && c!='\n' && c!='|' && c!='\\')after_pipe=0;
        if(c=='\\') {
            if(text[i+1]!='\n')after_pipe=0;
            if(quote=='"' && !strchr("$`\"\\\n",text[i+1]))i++;
            else i+=2;
            continue;
        }
        if(c=='$' && text[i+1]=='(') {
            size_t end=i+2;if(scan_context(text,&end,')',0))return strlen(text);
            i=end;continue;
        }
        if(c=='`') {
            i++;
            while(text[i] && text[i]!='`'){if(text[i]=='\\' && text[i+1])i++;i++;}
            if(text[i])i++;
            continue;
        }
        if(c=='$' && text[i+1]=='{') {
            i+=2;while(text[i] && text[i]!='}')i++;
            if(text[i])i++;
            continue;
        }
        if(c=='"'){quote=quote=='"'?0:'"';i++;continue;}
        if(!quote && c=='\''){quote='\'';i++;continue;}
        if(!quote && c=='|'){if(include_pipe)return i;after_pipe=1;i++;continue;}
        if(!quote && c=='\n' && !after_pipe)return i;
        i++;
    }
    return i;
}
static int execute_simple(Shell *shell,char **argv,Redirections *redirections,
                          int substitution_status,int direct_exec)
{
    int saved[2];
    if(redirections_apply(redirections,saved)==-1) {
        redirections_restore(saved);
        shell->last_status=1;return 1;
    }
    int status=shell->last_status;
    int argc=0;while(argv[argc])argc++;
    if(!argc){if(substitution_status>=0)status=substitution_status;else if(redirections->count || direct_exec)status=0;}
    else if(!strcmp(argv[0],"cd")) {
        const char *path=argc==2?argv[1]:getenv("HOME");
        if(argc>2){dprintf(2,"terminator: cd: too many arguments\n");status=2;}
        else if(!path || chdir(path)==-1){dprintf(2,"terminator: cd: %s\n",path?strerror(errno):"HOME is not set");status=1;}
        else if(!getcwd(shell->cwd,sizeof(shell->cwd))){dprintf(2,"terminator: pwd: %s\n",strerror(errno));status=1;}
        else status=0;
    } else if(!strcmp(argv[0],"pwd")) {
        if(argc!=1){dprintf(2,"Usage at this stage: pwd\n");status=2;}
        else {dprintf(1,"%s\n",shell->cwd);status=0;}
    } else if(!strcmp(argv[0],"history")) {
        if(argc!=1){dprintf(2,"Usage at this stage: history\n");status=2;}
        else {
            const char *text=shell->history_text?shell->history_text:"";
            status=write_all(1,text,strlen(text))==-1?1:0;
        }
    } else if(!strcmp(argv[0],"exit")) {
        if(argc>2){dprintf(2,"terminator: exit: too many arguments\n");status=2;}
        else {
            char *end=NULL;long value=argc==2?strtol(argv[1],&end,10):shell->last_status;
            if(argc==2 && (!*argv[1] || *end)){dprintf(2,"terminator: exit: numeric argument required\n");status=2;}
            else {status=(unsigned char)value;shell->exit_requested=1;}
        }
    } else {
        if(direct_exec) {
            execvp(argv[0],argv);int error=errno;
            if(error==ENOENT)dprintf(2,"terminator: %s: command not found\n",argv[0]);
            else dprintf(2,"terminator: %s: %s\n",argv[0],strerror(error));
            return error==ENOENT?127:126;
        }
        pid_t pid=fork();
        if(pid==0){
            execvp(argv[0],argv);int error=errno;
            if(error==ENOENT)dprintf(2,"terminator: %s: command not found\n",argv[0]);
            else dprintf(2,"terminator: %s: %s\n",argv[0],strerror(error));
            _exit(error==ENOENT?127:126);
        }
        if(pid==-1){dprintf(2,"terminator: fork: %s\n",strerror(errno));status=1;}
        else {
            int result=0;pid_t waited;
            do{waited=waitpid(pid,&result,0);}while(waited==-1 && errno==EINTR);
            status=waited==-1?1:WIFEXITED(result)?WEXITSTATUS(result):
                   WIFSIGNALED(result)?128+WTERMSIG(result):1;
        }
    }
    redirections_restore(saved);shell->last_status=status;return status;
}
typedef struct {
    char **argv;Redirections redirections;int substitution_status;
} PipelineStage;
/* Launch every stage before waiting. Only the neighbouring pipe endpoints
 * are inherited, so consumers see EOF and early exits cause SIGPIPE. */
static size_t pipeline_stage_start(const char *command,size_t start)
{
    for(;;) {
        if(command[start] && strchr(" \t\n",command[start]))start++;
        else if(command[start]=='\\' && command[start+1]=='\n')start+=2;
        else return start;
    }
}
static int execute_pipeline(Shell *shell,const char *command,int depth)
{
    size_t count=1;
    for(size_t start=0;;) {
        size_t end=next_separator(command,start,1);
        if(command[end]!='|')break;
        count++;start=end+1;
        start=pipeline_stage_start(command,start);
    }
    PipelineStage *stages=calloc(count,sizeof(*stages));
    pid_t *children=calloc(count,sizeof(*children));
    if(!stages || !children){free(stages);free(children);return 1;}
    int status=2;size_t start=0,launched=0;int previous=-1;
    for(size_t i=0;i<count;i++) {
        size_t end=next_separator(command,start,1);
        char *part=strndup(command+start,end-start);if(!part)goto cleanup;
        stages[i].substitution_status=-1;
        stages[i].argv=parse_arguments(shell,part,depth,&stages[i].substitution_status,&stages[i].redirections);
        free(part);
        if(!stages[i].argv)goto cleanup;
        start=command[end]?end+1:end;
        start=pipeline_stage_start(command,start);
    }
    for(size_t i=0;i<count;i++) {
        int next[2]={-1,-1};
        if(i+1<count && make_pipe(next,0)==-1)goto launch_failed;
        pid_t pid=fork();
        if(pid==0) {
            if(previous>=0 && dup2(previous,0)==-1)_exit(126);
            if(next[1]>=0 && dup2(next[1],1)==-1)_exit(126);
            if(previous>=0)close(previous);
            if(next[0]>=0)close(next[0]);
            if(next[1]>=0)close(next[1]);
            Shell inner=*shell;inner.exit_requested=0;
            int result=execute_simple(&inner,stages[i].argv,&stages[i].redirections,
                                      stages[i].substitution_status,1);
            _exit(result);
        }
        if(previous>=0){close(previous);previous=-1;}
        if(next[1]>=0)close(next[1]);
        if(pid==-1){if(next[0]>=0)close(next[0]);goto launch_failed;}
        previous=next[0];children[launched++]=pid;
    }
    status=0;
    for(size_t i=0;i<launched;i++) {
        int result=0;pid_t waited;
        do{waited=waitpid(children[i],&result,0);}while(waited==-1 && errno==EINTR);
        if(i==launched-1)status=waited==-1?1:WIFEXITED(result)?WEXITSTATUS(result):
                                WIFSIGNALED(result)?128+WTERMSIG(result):1;
    }
    goto cleanup;
launch_failed:
    dprintf(2,"terminator: pipeline: %s\n",strerror(errno));status=1;
    if(previous>=0){close(previous);previous=-1;}
    for(size_t i=0;i<launched;i++)kill(children[i],SIGKILL);
    for(size_t i=0;i<launched;i++)while(waitpid(children[i],NULL,0)==-1 && errno==EINTR){}
cleanup:
    if(previous>=0)close(previous);
    for(size_t i=0;i<count;i++){arguments_free(stages[i].argv);redirections_free(&stages[i].redirections);}
    free(stages);free(children);shell->last_status=status;return status;
}
static int run_shell(Shell *shell,const char *command,int depth)
{
    if(depth>NESTING_LIMIT){dprintf(2,"terminator: substitution nesting limit exceeded\n");return 2;}
    /* Keep list quotes intact: the ordinary argv parser would remove them. */
    const char *start_command=command+strspn(command," \t\n");
    if(!strncmp(start_command,"multiWatch",10) &&
       (!start_command[10] || strchr(" \t\n[",start_command[10]))) {
        shell->last_status=execute_multiwatch(shell,start_command+10,depth);
        return shell->last_status;
    }
    int complete=command_completeness(command);
    if(complete){dprintf(2,"terminator: unsupported or incomplete shell syntax\n");return 2;}
    size_t end=next_separator(command,0,0);
    if(command[end]) {
        size_t start=0;
        while(command[start]) {
            end=next_separator(command,start,0);
            char *part=strndup(command+start,end-start);
            if(!part){dprintf(2,"terminator: out of memory\n");return 1;}
            int status=run_shell(shell,part,depth);free(part);
            shell->last_status=status;
            if(shell->exit_requested || !command[end])break;
            start=end+1;
        }
        return shell->last_status;
    }
    size_t pipeline_end=next_separator(command,0,1);
    if(command[pipeline_end]=='|')return execute_pipeline(shell,command,depth);
    int substitution_status=-1;
    Redirections redirections={0};
    char **argv=parse_arguments(shell,command,depth,&substitution_status,&redirections);
    if(!argv){redirections_free(&redirections);dprintf(2,"terminator: invalid syntax or expansion size limit exceeded\n");return 2;}
    int status=execute_simple(shell,argv,&redirections,substitution_status,0);
    redirections_free(&redirections);arguments_free(argv);return status;
}
static int peer_job_fds[192],peer_job_fd_count;
static pid_t spawn_shell(Worker *w,const char *command,int last_status,int *output_fd,int *state_fd,int *input_fd,JobGroups *groups)
{
    char *history_copy=strdup(history_snapshots[w->slot].text);
    if(!history_copy)return -1;
    int out[2],state[2],in[2];
    if(make_pipe(out,1)==-1){free(history_copy);return -1;}
    if(make_pipe(state,1)==-1){close(out[0]);close(out[1]);free(history_copy);return -1;}
    if(make_pipe(in,0)==-1) {
        close(out[0]);close(out[1]);close(state[0]);close(state[1]);free(history_copy);return -1;
    }
    if(set_flags(in[1],1)==-1) {
        close(out[0]);close(out[1]);close(state[0]);close(state[1]);close(in[0]);close(in[1]);free(history_copy);return -1;
    }
    pid_t pid=fork();
    if(pid==0) {
        active_job_groups=groups;
        ptrdiff_t index=groups-job_registries[w->slot].groups;
        atomic_store(&job_registries[w->slot].roots[index],(int)getpid());
        for(int i=0;i<peer_job_fd_count;i++)close(peer_job_fds[i]);
        close(out[0]);close(state[0]);close(in[1]);
        if(setpgid(0,0)==-1 || fchdir(w->directory_fd)==-1)_exit(126);
        struct sigaction action={0};action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);
        sigaction(SIGINT,&action,NULL);sigaction(SIGPIPE,&action,NULL);
        if(dup2(out[1],1)==-1 || dup2(out[1],2)==-1)_exit(126);
        if(out[1]>2)close(out[1]);
        close(w->request_fd);close(w->reply_fd);close(w->directory_fd);
        if(dup2(in[0],0)==-1)_exit(126);
        if(in[0]>2)close(in[0]);
        Shell shell={0};snprintf(shell.cwd,sizeof(shell.cwd),"%s",w->cwd);shell.last_status=last_status;
        shell.history_text=history_copy;
        int status=run_shell(&shell,command,0);
        ShellResult result={0};result.exit_requested=shell.exit_requested;
        snprintf(result.cwd,sizeof(result.cwd),"%s",shell.cwd);
        write_all(state[1],&result,sizeof(result));close(state[1]);
        _exit(status);
    }
    int error=errno;free(history_copy);close(out[1]);close(state[1]);close(in[0]);
    if(pid==-1){close(out[0]);close(state[0]);close(in[1]);errno=error;return -1;}
    ptrdiff_t index=groups-job_registries[w->slot].groups;
    atomic_store(&job_registries[w->slot].roots[index],(int)pid);
    setpgid(pid,pid);*output_fd=out[0];*state_fd=state[0];*input_fd=in[1];return pid;
}

/* Each job owns a runner process group, output/state/input pipes and status.
 * Only this tab's worker reads or modifies this table. */
typedef struct {
    int used,id,stopped,finished,status,output,state,input,input_eof,allow_state;
    pid_t pid,group;
    char command[COMMAND_SIZE],pending[COMMAND_SIZE*4];
    size_t input_used,state_used;
    ShellResult result;
    JobGroups *groups;
} Job;
static void signal_job(Job *job,int number)
{
    if(job->group>0)kill(-job->group,number);
    if(job->groups)for(int i=0;i<JOB_GROUPS;i++) {
        int group=atomic_load(&job->groups->groups[i]);
        if(group>0)kill(-group,number);
    }
}
static void job_close(Job *job)
{
    if(job->output>=0)close(job->output);
    if(job->state>=0)close(job->state);
    if(job->input>=0)close(job->input);
    if(job->groups)for(int g=0;g<JOB_GROUPS;g++)atomic_store(&job->groups->groups[g],0);
    memset(job,0,sizeof(*job));
}
static int job_message(Worker *w,Job *job,const char *state)
{
    char text[COMMAND_SIZE+128];
    snprintf(text,sizeof(text),"[%d]+ %s %s\n",job->id,state,job->command);
    return reply_send(w,REPLY_OUTPUT,0,text);
}
static Job *find_job(Job *jobs,const char *spec)
{
    if(!spec || !strcmp(spec,"%+") || !strcmp(spec,"%%")) {
        Job *best=NULL;
        for(int i=0;i<MAX_JOBS;i++)if(jobs[i].used && !jobs[i].finished && (!best || jobs[i].id>best->id))best=&jobs[i];
        return best;
    }
    if(*spec=='%')spec++;
    if(!*spec)return NULL;
    char *end;errno=0;long id=strtol(spec,&end,10);
    if(errno || *end || id<=0 || id>INT_MAX)return NULL;
    for(int i=0;i<MAX_JOBS;i++)if(jobs[i].used && !jobs[i].finished && jobs[i].id==id)return &jobs[i];
    return NULL;
}
/* Only an unquoted final '&' is background syntax. The completeness scanner
 * has already rejected '&' in the middle, '&&', and unfinished quotes. */
static int remove_background(char *text)
{
    size_t n=strlen(text);while(n && strchr(" \t\n",text[n-1]))n--;
    if(!n || text[n-1]!='&')return 0;
    size_t slashes=0,pos=n-1;
    while(pos && text[pos-1]=='\\'){slashes++;pos--;}
    if(slashes%2)return 0;
    /* A quoted '&' ends in a quote, not '&', once the command is complete. */
    text[n-1]='\0';return 1;
}
static void worker_main(Worker *argument)
{
    Worker w=*argument;free(argument);
    Job *jobs=calloc(MAX_JOBS,sizeof(*jobs));
    if(!jobs){worker_error(&w,"jobs allocation");close(w.directory_fd);close(w.request_fd);close(w.reply_fd);return;}
    Job *foreground=NULL;
    int next_id=1,last_status=0,alive=1,stopping=0;
    Request request;size_t request_used=0;
    char command[COMMAND_SIZE];size_t command_used=0;int command_overflow=0;
    while(alive && !stopping) {
        int active=0;
        for(int i=0;i<MAX_JOBS && alive;i++) {
            Job *j=&jobs[i];if(!j->used)continue;active=1;
            if(j->input>=0 && j->input_used) {
                ssize_t n=write(j->input,j->pending,j->input_used);
                if(n>0){j->input_used-=(size_t)n;memmove(j->pending,j->pending+n,j->input_used);}
                else if(n<0 && errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK) {
                    close(j->input);j->input=-1;j->input_used=0;
                }
            }
            if(j->input>=0 && j->input_eof && !j->input_used){close(j->input);j->input=-1;}
            if(j->output>=0)for(int batch=0;batch<8;batch++) {
                char text[PAYLOAD_SIZE];ssize_t n=read(j->output,text,sizeof(text)-1);
                if(n>0){text[n]='\0';if(reply_send(&w,REPLY_OUTPUT,0,text)==-1){alive=0;break;}}
                else if(n==0){close(j->output);j->output=-1;break;}
                else if(errno==EINTR)continue;
                else if(errno==EAGAIN || errno==EWOULDBLOCK)break;
                else {close(j->output);j->output=-1;break;}
            }
            if(j->state>=0) {
                char discard[64];
                void *destination=j->state_used<sizeof(j->result)?(char *)&j->result+j->state_used:discard;
                size_t amount=j->state_used<sizeof(j->result)?sizeof(j->result)-j->state_used:sizeof(discard);
                ssize_t n=read(j->state,destination,amount);
                if(n>0){if(j->state_used<sizeof(j->result))j->state_used+=(size_t)n;}
                else if(n==0){close(j->state);j->state=-1;}
                else if(errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK){close(j->state);j->state=-1;}
            }
            if(!j->finished) {
                int status;pid_t result=waitpid(j->pid,&status,WNOHANG|WUNTRACED|WCONTINUED);
                if(result==j->pid) {
                    if(WIFSTOPPED(status)) {
                        j->stopped=1;signal_job(j,SIGSTOP);
                        if(job_message(&w,j,"Stopped")==-1)alive=0;
                        if(foreground==j) {
                            foreground=NULL;last_status=128+SIGTSTP;
                            if(reply_send(&w,REPLY_DONE,last_status,NULL)==-1)alive=0;
                        }
                    } else if(WIFCONTINUED(status))j->stopped=0;
                    else {
                        j->finished=1;j->stopped=0;
                        j->status=WIFEXITED(status)?WEXITSTATUS(status):WIFSIGNALED(status)?128+WTERMSIG(status):1;
                        signal_job(j,SIGKILL);
                        /* If a runner died before its monitor could clean up,
                         * dispose of the separately grouped watched commands. */
                        if(j->groups)for(int g=0;g<JOB_GROUPS;g++) {
                            int group=atomic_load(&j->groups->groups[g]);if(group>0)kill(-group,SIGKILL);
                        }
                    }
                } else if(result==-1 && errno!=EINTR){j->finished=1;j->status=1;}
            }
            if(j->finished && j->output<0 && j->state<0) {
                if(foreground==j) {
                    last_status=j->status;foreground=NULL;
                    if(j->allow_state && j->state_used==sizeof(j->result)) {
                        j->result.cwd[PATH_MAX-1]='\0';
                        if(strcmp(j->result.cwd,w.cwd)) {
                            if(change_directory(&w,j->result.cwd)==-1){worker_error(&w,"cd");last_status=1;}
                            else if(reply_send(&w,REPLY_CWD,0,w.cwd)==-1)alive=0;
                        }
                        if(j->result.exit_requested)stopping=1;
                    }
                    if(reply_send(&w,REPLY_DONE,last_status,NULL)==-1)alive=0;
                } else {
                    char state[64];snprintf(state,sizeof(state),"Done (%d)",j->status);
                    if(job_message(&w,j,state)==-1)alive=0;
                }
                signal_job(j,SIGKILL);atomic_store(&job_registries[w.slot].roots[i],0);job_close(j);
            }
        }
        if(!alive || stopping)break;
        struct pollfd fds[1+3*MAX_JOBS];int count=1;
        fds[0]=(struct pollfd){w.request_fd,POLLIN,0};
        for(int i=0;i<MAX_JOBS;i++)if(jobs[i].used) {
            Job *j=&jobs[i];fds[count++]=(struct pollfd){j->output,POLLIN,0};
            fds[count++]=(struct pollfd){j->state,POLLIN,0};
            fds[count++]=(struct pollfd){j->input_used?j->input:-1,POLLOUT,0};
        }
        if(poll(fds,(nfds_t)count,active?100:-1)==-1){if(errno==EINTR)continue;break;}
        if(!(fds[0].revents&(POLLIN|POLLHUP|POLLERR)))continue;
        ssize_t n=read(w.request_fd,(char *)&request+request_used,sizeof(request)-request_used);
        if(n==0)break;
        if(n<0){if(errno==EINTR || errno==EAGAIN)continue;break;}
        request_used+=(size_t)n;if(request_used<sizeof(request))continue;
        request_used=0;request.text[INPUT_SIZE-1]='\0';
        if(request.type==REQUEST_STOP)break;
        if(request.type==REQUEST_INTERRUPT || request.type==REQUEST_SUSPEND) {
            if(foreground) {
                if(request.type==REQUEST_SUSPEND) {
                    reply_send(&w,REPLY_OUTPUT,0,"^Z\n");
                    /* SIGSTOP cannot be ignored by an orphan process group.
                     * No controlling PTY is required for X11-driven suspension. */
                    signal_job(foreground,SIGSTOP);
                } else {
                    const char *name=foreground->command+strspn(foreground->command," \t\n");
                    if(strncmp(name,"multiWatch",10) || (name[10] && !strchr(" \t\n[",name[10])))
                        reply_send(&w,REPLY_OUTPUT,0,"^C\n");
                    signal_job(foreground,SIGINT);
                }
            }
            continue;
        }
        if(request.type==REQUEST_INPUT || request.type==REQUEST_EOF) {
            Job *j=foreground;
            if(j && j->input>=0 && !j->input_eof) {
                size_t length=strlen(request.text);
                if(length<=sizeof(j->pending)-j->input_used) {
                    memcpy(j->pending+j->input_used,request.text,length);j->input_used+=length;
                } else reply_send(&w,REPLY_OUTPUT,0,"Input queue full; wait before sending more input.\n");
                if(request.type==REQUEST_EOF)j->input_eof=1;
            }
            continue;
        }
        if(foreground)continue;
        if(request.type==REQUEST_SET_STATUS) {last_status=0;reply_send(&w,REPLY_DONE,0,NULL);continue;}
        if(request.type!=REQUEST_FRAGMENT && request.type!=REQUEST_COMMAND)continue;
        size_t length=strlen(request.text);
        if(command_used+length>=sizeof(command))command_overflow=1;
        else if(!command_overflow){memcpy(command+command_used,request.text,length);command_used+=length;}
        if(request.type==REQUEST_FRAGMENT)continue;
        command[command_used]='\0';command_used=0;
        if(command_overflow) {
            command_overflow=0;reply_send(&w,REPLY_OUTPUT,0,"terminator: command is too long\n");
            last_status=2;reply_send(&w,REPLY_DONE,last_status,NULL);continue;
        }
        if(command_completeness(command)) {
            reply_send(&w,REPLY_OUTPUT,0,"terminator: unsupported or incomplete syntax\n");
            last_status=2;reply_send(&w,REPLY_DONE,last_status,NULL);continue;
        }
        int background=remove_background(command);
        char copy[COMMAND_SIZE];snprintf(copy,sizeof(copy),"%s",command);
        char *save=NULL,*name=strtok_r(copy," \t\n",&save);
        if(name && (!strcmp(name,"jobs") || !strcmp(name,"fg") || !strcmp(name,"bg"))) {
            char *spec=strtok_r(NULL," \t\n",&save),*extra=strtok_r(NULL," \t\n",&save);
            last_status=0;
            if(background || extra || (!strcmp(name,"jobs") && spec)) {
                reply_send(&w,REPLY_OUTPUT,0,"Usage: jobs | fg [%job] | bg [%job] (standalone commands)\n");last_status=2;
            } else if(!strcmp(name,"jobs")) {
                int any=0;
                for(int i=0;i<MAX_JOBS;i++)if(jobs[i].used) {
                    any=1;job_message(&w,&jobs[i],jobs[i].finished?"Done":jobs[i].stopped?"Stopped":"Running");
                }
                if(!any)reply_send(&w,REPLY_OUTPUT,0,"No jobs.\n");
            } else {
                Job *j=find_job(jobs,spec);
                if(!j){reply_send(&w,REPLY_OUTPUT,0,"terminator: no such job\n");last_status=1;}
                else if(!strcmp(name,"bg")) {
                    if(!j->stopped){reply_send(&w,REPLY_OUTPUT,0,"terminator: job is already running\n");last_status=1;}
                    else {
                        j->stopped=0;signal_job(j,SIGCONT);
                        char text[COMMAND_SIZE+64];snprintf(text,sizeof(text),"[%d]+ %s &\n",j->id,j->command);
                        reply_send(&w,REPLY_OUTPUT,0,text);
                    }
                } else {
                    foreground=j;j->stopped=0;
                    reply_send(&w,REPLY_OUTPUT,0,j->command);reply_send(&w,REPLY_OUTPUT,0,"\n");
                    reply_send(&w,REPLY_STARTED,(int)j->group,NULL);signal_job(j,SIGCONT);
                }
            }
            if(!foreground)reply_send(&w,REPLY_DONE,last_status,NULL);
            continue;
        }
        Job *j=NULL;for(int i=0;i<MAX_JOBS;i++)if(!jobs[i].used){j=&jobs[i];break;}
        if(!j || next_id==INT_MAX) {
            reply_send(&w,REPLY_OUTPUT,0,"terminator: job limit reached (64 active jobs)\n");last_status=1;
            reply_send(&w,REPLY_DONE,last_status,NULL);continue;
        }
        memset(j,0,sizeof(*j));j->output=j->state=j->input=-1;
        j->groups=&job_registries[w.slot].groups[j-jobs];
        for(int g=0;g<JOB_GROUPS;g++)atomic_store(&j->groups->groups[g],0);
        if(!atomic_is_lock_free(&j->groups->groups[0])) {
            reply_send(&w,REPLY_OUTPUT,0,"terminator: platform lacks lock-free shared job atomics\n");
            job_close(j);reply_send(&w,REPLY_DONE,1,NULL);continue;
        }
        peer_job_fd_count=0;
        for(int i=0;i<MAX_JOBS;i++)if(jobs[i].used) {
            int descriptors[]={jobs[i].output,jobs[i].state,jobs[i].input};
            for(int k=0;k<3;k++)if(descriptors[k]>=0)peer_job_fds[peer_job_fd_count++]=descriptors[k];
        }
        j->pid=spawn_shell(&w,command,last_status,&j->output,&j->state,&j->input,j->groups);
        if(j->pid<0){worker_error(&w,"fork/pipe");job_close(j);last_status=1;reply_send(&w,REPLY_DONE,1,NULL);continue;}
        j->used=1;j->id=next_id++;j->group=j->pid;j->allow_state=!background;
        snprintf(j->command,sizeof(j->command),"%s",command);
        if(background) {
            /* New background commands receive EOF on stdin unless '<' replaces
             * it. Every redirection is opened independently in its child. */
            close(j->input);j->input=-1;j->input_eof=1;
            char text[COMMAND_SIZE+80];snprintf(text,sizeof(text),"[%d] %ld %s &\n",j->id,(long)j->pid,j->command);
            reply_send(&w,REPLY_OUTPUT,0,text);last_status=0;reply_send(&w,REPLY_DONE,0,NULL);
        } else {foreground=j;reply_send(&w,REPLY_STARTED,(int)j->group,NULL);}
    }
    /* Stop/resume frozen groups before final termination; registry entries also
     * cover multiWatch's subsidiary groups if its runner crashed. */
    for(int i=0;i<MAX_JOBS;i++)if(jobs[i].used)signal_job(&jobs[i],SIGKILL);
    for(int i=0;i<MAX_JOBS;i++)if(jobs[i].used) {
        if(!jobs[i].finished)while(waitpid(jobs[i].pid,NULL,0)==-1 && errno==EINTR){}
        atomic_store(&job_registries[w.slot].roots[i],0);
        job_close(&jobs[i]);
    }
    free(jobs);close(w.directory_fd);close(w.request_fd);close(w.reply_fd);
}

typedef struct {
    int used,busy,status,request_fd,reply_fd,partial,scroll,input_length,cursor;
    int history_position,history_count,selection_active,anchor_line,anchor_col,end_line,end_col;
    unsigned long generation;
    pid_t worker_pid, command_group;
    char input[COMMAND_SIZE],cwd[PATH_MAX],draft[COMMAND_SIZE];
    char (*browse_history)[COMMAND_SIZE];
    char continued[COMMAND_SIZE];size_t continued_used;
    Request outgoing[QUEUE_SIZE];int queue_head,queue_count;
    char lines[MAX_LINES][LINE_SIZE];
    int line_count;
    Reply incoming; size_t incoming_used;
} Tab;
static Tab tabs[MAX_TABS];
static int active_tab;
static void editor_history_reset(Tab *t)
{
    free(t->browse_history);t->browse_history=NULL;
    t->history_count=0;t->history_position=0;t->draft[0]='\0';
}
static void editor_reset(Tab *t)
{
    t->input_length=0;t->cursor=0;t->input[0]='\0';
    editor_history_reset(t);
}
static int editor_insert(Tab *t,const char *text,size_t size)
{
    if(t->continued_used+(size_t)t->input_length+size>COMMAND_SIZE-2)return -1;
    if(t->cursor<0)t->cursor=0;
    if(t->cursor>t->input_length)t->cursor=t->input_length;
    memmove(t->input+t->cursor+size,t->input+t->cursor,
            (size_t)(t->input_length-t->cursor)+1);
    memcpy(t->input+t->cursor,text,size);
    t->cursor+=(int)size;t->input_length+=(int)size;
    return 0;
}
static void editor_delete(Tab *t,int start,int end)
{
    if(start<0)start=0;
    if(end>t->input_length)end=t->input_length;
    if(start>=end)return;
    memmove(t->input+start,t->input+end,(size_t)(t->input_length-end)+1);
    t->input_length-=end-start;
    if(t->cursor>=end)t->cursor-=end-start;
    else if(t->cursor>start)t->cursor=start;
}
static void editor_previous_word(Tab *t)
{
    int start=t->cursor;
    while(start>0 && strchr(" \t\n",t->input[start-1]))start--;
    while(start>0 && !strchr(" \t\n",t->input[start-1]))start--;
    editor_delete(t,start,t->cursor);
}
/* Freeze a per-tab browsing snapshot on the first Up. This keeps history
 * navigation stable even while another tab adds shared history entries. */
static void editor_history(Tab *t,int direction)
{
    if(t->busy)return;
    if(!t->browse_history) {
        if(direction>0)return;
        history_lock();int count=history_data->count;
        char (*copy)[COMMAND_SIZE]=count?malloc((size_t)count*sizeof(*copy)):NULL;
        if(copy)memcpy(copy,history_data->entries,(size_t)count*sizeof(*copy));
        history_unlock();
        if(!copy)return;
        t->browse_history=copy;t->history_count=count;t->history_position=count;
        snprintf(t->draft,sizeof(t->draft),"%s",t->input);
    }
    int next=t->history_position+direction;
    if(next<0 || next>t->history_count)return;
    t->history_position=next;
    const char *text=next==t->history_count?t->draft:t->browse_history[next];
    snprintf(t->input,sizeof(t->input),"%s",text);
    t->input_length=(int)strlen(t->input);t->cursor=t->input_length;
}
static void remember_line(Tab *t,const char *text)
{
    if (t->line_count==MAX_LINES) {
        memmove(t->lines,t->lines+1,(MAX_LINES-1)*sizeof(t->lines[0]));
        t->line_count--;
        /* Absolute selection rows change when the oldest output is dropped. */
        if(t->selection_active) {
            if(t->anchor_line==0 || t->end_line==0)t->selection_active=0;
            else {t->anchor_line--;t->end_line--;}
        }
    }
    snprintf(t->lines[t->line_count++],LINE_SIZE,"%s",text);
}
static void append_output(Tab *t,const char *text)
{
    for (const unsigned char *p=(const unsigned char *)text; *p;p++) {
        if (!t->partial) { remember_line(t,""); t->partial=1; }
        if (*p=='\n') { t->partial=0; continue; }
        if (*p<32 && *p!='\t') continue;
        if (*p==127) continue;
        size_t n=strlen(t->lines[t->line_count-1]);
        if (n==LINE_SIZE-1) { remember_line(t,""); n=0; }
        t->lines[t->line_count-1][n]=*p=='\t' ? ' ' : (char)*p;
        t->lines[t->line_count-1][n+1]='\0';
    }
}
static int flush_requests(Tab *t)
{
    /* Frames fit the POSIX minimum PIPE_BUF; never split a frame. */
    while(t->queue_count) {
        Request *r=&t->outgoing[t->queue_head];
        ssize_t n=write(t->request_fd,r,sizeof(*r));
        if(n<0 && errno==EINTR)continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return 0;
        if(n!=(ssize_t)sizeof(*r))return -1;
        t->queue_head=(t->queue_head+1)%QUEUE_SIZE;t->queue_count--;
    }
    return 0;
}
static int request_send(Tab *t,int type,const char *text)
{
    size_t length=text?strlen(text):0;
    int frames=(int)(length/(INPUT_SIZE-1))+1;
    if(type!=REQUEST_COMMAND && type!=REQUEST_INPUT && frames!=1){errno=EMSGSIZE;return -1;}
    if(frames>QUEUE_SIZE-t->queue_count){errno=EAGAIN;return -1;}
    errno=0;long limit=fpathconf(t->request_fd,_PC_PIPE_BUF);
    if(limit==-1 && errno)return -1;
    if(limit!=-1 && sizeof(Request)>(size_t)limit){errno=EMSGSIZE;return -1;}
    size_t offset=0;
    for(int i=0;i<frames;i++) {
        int index=(t->queue_head+t->queue_count)%QUEUE_SIZE;
        Request *r=&t->outgoing[index];memset(r,0,sizeof(*r));
        r->type=type==REQUEST_COMMAND && i!=frames-1?REQUEST_FRAGMENT:type;
        size_t chunk=length-offset;if(chunk>INPUT_SIZE-1)chunk=INPUT_SIZE-1;
        if(chunk)memcpy(r->text,text+offset,chunk);
        offset+=chunk;t->queue_count++;
    }
    return flush_requests(t);
}
static int new_tab(void)
{
    int slot;
    for (slot=0;slot<MAX_TABS && tabs[slot].used;slot++) {}
    if (slot==MAX_TABS) { errno=ENOSPC; return -1; }
    int commands[2],replies[2];
    if (make_pipe(commands,1)==-1) return -1;
    if (make_pipe(replies,1)==-1) { close(commands[0]); close(commands[1]); return -1; }
    if (set_flags(commands[1],1)==-1) {
        close(commands[0]);close(commands[1]);close(replies[0]);close(replies[1]);return -1;
    }
    Worker *w=calloc(1,sizeof(*w));
    if (!w) { close(commands[0]);close(commands[1]);close(replies[0]);close(replies[1]);return -1; }
    w->request_fd=commands[0]; w->reply_fd=replies[1];w->slot=slot;
    snprintf(w->cwd,sizeof(w->cwd),"%s",initial_cwd);
    w->directory_fd=open(initial_cwd,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (w->directory_fd==-1 || set_flags(w->directory_fd,0)==-1) {
        if (w->directory_fd>=0)close(w->directory_fd);
        free(w);close(commands[0]);close(commands[1]);close(replies[0]);close(replies[1]);return -1;
    }
    static unsigned long generation;
    Tab *t=&tabs[slot]; memset(t,0,sizeof(*t));t->generation=++generation;
    t->request_fd=commands[1]; t->reply_fd=replies[0];
    snprintf(t->cwd,sizeof(t->cwd),"%s",initial_cwd);
    pid_t pid=fork();
    if (pid==0) {
        /* Close inherited endpoints of every other tab, including X11.
         * The GUI parent has no worker threads. */
        for (long fd=3;fd<descriptor_limit;fd++)
            if (fd!=w->request_fd && fd!=w->reply_fd && fd!=w->directory_fd)
                close((int)fd);
        gui_fd=-1;
        worker_main(w); _exit(0);
    }
    if (pid==-1) {
        close(w->directory_fd);free(w);
        close(commands[0]);close(commands[1]);close(replies[0]);close(replies[1]);return -1;
    }
    t->worker_pid=pid;
    close(w->directory_fd);free(w);
    close(commands[0]);close(replies[1]);
    t->used=1; active_tab=slot;
    remember_line(t,"Process-only tab.");
    remember_line(t,"Ctrl+T: new tab. Ctrl+Tab: switch. Ctrl+Shift+W: close. Ctrl+C: interrupt.");
    remember_line(t,"Ctrl+W: delete word. Ctrl+Shift+C/V: copy/paste. bindings reload: reload ~/.terminatorrc.");
    remember_line(t,"Ctrl+Z: suspend. cmd &: background. jobs / fg %N / bg %N manage this tab's jobs.");
    remember_line(t,"Quotes, substitutions, multiline input, < / >, pipelines | and file patterns work.");
    return slot;
}
static void close_tab(int slot)
{
    Tab *t=&tabs[slot]; if (!t->used)return;
    /* EOF on the request pipe is also a shutdown request. */
    request_send(t,REQUEST_STOP,NULL);
    close(t->request_fd);close(t->reply_fd);
    /* The GUI can also recover jobs if this tab's worker has crashed. */
    for(int j=0;j<MAX_JOBS;j++) {
        int root=atomic_load(&job_registries[slot].roots[j]);if(root>0)kill(-root,SIGKILL);
        for(int g=0;g<JOB_GROUPS;g++) {
            int group=atomic_load(&job_registries[slot].groups[j].groups[g]);
            if(group>0)kill(-group,SIGKILL);
        }
    }
    /* The worker shuts its runner down gracefully so multiWatch can clean up. */
    while (waitpid(t->worker_pid,NULL,0)==-1 && errno==EINTR) {}
    for(int j=0;j<MAX_JOBS;j++) {
        atomic_store(&job_registries[slot].roots[j],0);
        for(int g=0;g<JOB_GROUPS;g++)atomic_store(&job_registries[slot].groups[j].groups[g],0);
    }
    free(t->browse_history);memset(t,0,sizeof(*t));
    if (active_tab==slot) {
        for (int i=0;i<MAX_TABS;i++)if(tabs[i].used){active_tab=i;break;}
    }
}
static int tab_count(void)
{
    int count=0; for(int i=0;i<MAX_TABS;i++) count+=tabs[i].used; return count;
}
static int collect_replies(int slot)
{
    Tab *t=&tabs[slot]; int changed=0;
    if(flush_requests(t)==-1){close_tab(slot);return 1;}
    /* Bound work per tab so noisy output cannot starve other tabs or X11. */
    for (int batch=0;batch<16;batch++) {
        ssize_t n=read(t->reply_fd,(char *)&t->incoming+t->incoming_used,
                       sizeof(t->incoming)-t->incoming_used);
        if (n==0) { close_tab(slot);return 1; }
        if (n==-1) {
            if(errno==EINTR)continue;
            if(errno!=EAGAIN && errno!=EWOULDBLOCK){close_tab(slot);return 1;}
            break;
        }
        t->incoming_used+=(size_t)n;
        if(t->incoming_used<sizeof(t->incoming))continue;
        Reply *r=&t->incoming;r->text[PAYLOAD_SIZE-1]='\0';
        if(r->type==REPLY_OUTPUT) append_output(t,r->text);
        else if(r->type==REPLY_CWD) memcpy(t->cwd,r->text,sizeof(t->cwd));
        else if(r->type==REPLY_STARTED)t->command_group=(pid_t)r->value;
        else if(r->type==REPLY_DONE){t->busy=0;t->status=r->value;t->partial=0;t->command_group=-1;}
        t->incoming_used=0;changed=1;
    }
    return changed;
}
#ifndef TERMINATOR_HEADLESS_TEST
static int bindings_command(Tab *t,const char *command);
#endif
static void submit(Tab *t)
{
    if(t->busy || (!t->input_length && !t->continued_used))return;
    char command[COMMAND_SIZE];
    size_t length=t->continued_used+(size_t)t->input_length;
    if(length>=sizeof(command)-1){remember_line(t,"Command limit: 8190 characters. Ctrl+C cancels input.");return;}
    memcpy(command,t->continued,t->continued_used);
    memcpy(command+t->continued_used,t->input,(size_t)t->input_length+1);
#ifndef TERMINATOR_HEADLESS_TEST
    if(!t->continued_used && bindings_command(t,command)){editor_reset(t);return;}
#endif
    int completeness=command_completeness(command);
    char line[COMMAND_SIZE+64];
    snprintf(line,sizeof(line),"%s%s",t->continued_used?"> ":"user@terminator> ",t->input);
    t->partial=0;append_output(t,line);append_output(t,"\n");t->scroll=0;
    if(completeness==1) {
        memcpy(t->continued,command,length);t->continued[length]='\n';
        t->continued[length+1]='\0';t->continued_used=length+1;
        editor_reset(t);return;
    }
    history_add(command);
    history_snapshot((int)(t-tabs));
    if(request_send(t,REQUEST_COMMAND,command)==0)t->busy=1;
    else {
        snprintf(line,sizeof(line),"Could not send command: %s",strerror(errno));remember_line(t,line);return;
    }
    editor_reset(t);t->continued_used=0;t->continued[0]='\0';
}

#ifndef TERMINATOR_HEADLESS_TEST
/* Key configuration: ~/.terminatorrc, one action=key per line.
 * Example: line_start=Ctrl+B
 * Run "bindings reload" in a tab after editing the file. "bindings" lists
 * effective bindings. Invalid/conflicting entries revert to the default.
 * Lock/CapsLock and NumLock do not affect matching. */
enum { HK_LEFT,HK_RIGHT,HK_START,HK_END,HK_PREVIOUS,HK_NEXT,HK_KILL_START,
       HK_KILL_END,HK_WORD,HK_CLEAR,HK_EOF,HK_COPY,HK_PASTE,HK_NEW_TAB,
       HK_CLOSE_TAB,HK_NEXT_TAB,HK_PREV_TAB,HK_INTERRUPT,HK_SUSPEND,HK_PAGE_UP,
       HK_PAGE_DOWN,HK_BACKSPACE,HK_DELETE,HK_ENTER,HK_COUNT };
typedef struct { const char *name,*default_text;KeySym key;unsigned modifiers; } Binding;
static Binding bindings[HK_COUNT]={
    {"left","Left",0,0},{"right","Right",0,0},
    {"line_start","Ctrl+A",0,0},{"line_end","Ctrl+E",0,0},
    {"history_previous","Up",0,0},{"history_next","Down",0,0},
    {"delete_to_start","Ctrl+U",0,0},{"delete_to_end","Ctrl+K",0,0},
    {"delete_word","Ctrl+W",0,0},{"clear_screen","Ctrl+L",0,0},
    {"eof","Ctrl+D",0,0},{"copy","Ctrl+Shift+C",0,0},
    {"paste","Ctrl+Shift+V",0,0},{"new_tab","Ctrl+T",0,0},
    {"close_tab","Ctrl+Shift+W",0,0},{"next_tab","Ctrl+Tab",0,0},
    {"previous_tab","Ctrl+Shift+Tab",0,0},{"interrupt","Ctrl+C",0,0},{"suspend","Ctrl+Z",0,0},
    {"page_up","Page_Up",0,0},{"page_down","Page_Down",0,0},
    {"backspace","BackSpace",0,0},{"delete","Delete",0,0},{"submit","Return",0,0}
};
static char binding_messages[8192];
static int binding_errors;
static void binding_error(const char *text)
{
    size_t used=strlen(binding_messages);
    snprintf(binding_messages+used,sizeof(binding_messages)-used,"bindings: %s\n",text);
    binding_errors++;
}
static char *trim_space(char *text)
{
    text+=strspn(text," \t\r\n");size_t n=strlen(text);
    while(n && strchr(" \t\r\n",text[n-1]))text[--n]='\0';
    return text;
}
static KeySym normalized_key(KeySym key)
{
    if(key==XK_ISO_Left_Tab)return XK_Tab;
    if(key==XK_KP_Enter)return XK_Return;
    KeySym lower,upper;XConvertCase(key,&lower,&upper);return lower;
}
static int binding_parse(const char *spec,KeySym *key,unsigned *modifiers)
{
    if(!*spec || strlen(spec)>=128 || strstr(spec,"++") || spec[strlen(spec)-1]=='+')return -1;
    char text[128];snprintf(text,sizeof(text),"%s",spec);
    char *save=NULL;*modifiers=0;*key=NoSymbol;
    for(char *part=strtok_r(text,"+",&save);part;part=strtok_r(NULL,"+",&save)) {
        part=trim_space(part);unsigned mask=0;
        if(!strcmp(part,"Ctrl") || !strcmp(part,"Control"))mask=ControlMask;
        else if(!strcmp(part,"Shift"))mask=ShiftMask;
        else if(!strcmp(part,"Alt"))mask=Mod1Mask;
        else if(!strcmp(part,"Super"))mask=Mod4Mask;
        if(mask) {
            if(*key!=NoSymbol || (*modifiers&mask))return -1;
            *modifiers|=mask;
        } else {
            if(*key!=NoSymbol)return -1;
            *key=normalized_key(XStringToKeysym(part));
            if(*key==NoSymbol || IsModifierKey(*key))return -1;
        }
    }
    if(*key==NoSymbol)return -1;
    /* Avoid stealing unmodified printable characters from ordinary typing. */
    if(*key>=32 && *key<=126 && !(*modifiers&(ControlMask|Mod1Mask|Mod4Mask)))return -1;
    return 0;
}
static void bindings_load(void)
{
    binding_messages[0]='\0';binding_errors=0;
    int custom[HK_COUNT]={0},seen[HK_COUNT]={0};
    for(int i=0;i<HK_COUNT;i++)binding_parse(bindings[i].default_text,&bindings[i].key,&bindings[i].modifiers);
    const char *home=getenv("HOME"),*override=getenv("TERMINATOR_CONFIG");char path[PATH_MAX];
    int path_length=override?snprintf(path,sizeof(path),"%s",override):
                    home?snprintf(path,sizeof(path),"%s/.terminatorrc",home):-1;
    if(path_length<0 || path_length>=(int)sizeof(path)) {
        binding_error("Cannot locate ~/.terminatorrc; using defaults.");return;
    }
    FILE *file=fopen(path,"r");
    if(!file){if(errno!=ENOENT)binding_error("Cannot read ~/.terminatorrc; using defaults.");return;}
    char *line=NULL;size_t capacity=0;int line_number=0;
    while(getline(&line,&capacity,file)>=0) {
        line_number++;char *comment=strchr(line,'#');if(comment)*comment='\0';
        char *name=trim_space(line);if(!*name)continue;
        char *equal=strchr(name,'=');char error[256];
        if(!equal){snprintf(error,sizeof(error),"line %d: expected action=key.",line_number);binding_error(error);continue;}
        *equal='\0';name=trim_space(name);char *value=trim_space(equal+1);
        int action;for(action=0;action<HK_COUNT;action++)if(!strcmp(name,bindings[action].name))break;
        if(action==HK_COUNT){snprintf(error,sizeof(error),"line %d: unknown action %.100s.",line_number,name);binding_error(error);continue;}
        KeySym key;unsigned modifiers;
        if(seen[action] || binding_parse(value,&key,&modifiers)==-1) {
            snprintf(error,sizeof(error),"line %d: invalid/duplicate %.80s; using %s.",line_number,name,bindings[action].default_text);
            binding_error(error);custom[action]=0;
            binding_parse(bindings[action].default_text,&bindings[action].key,&bindings[action].modifiers);
        } else {bindings[action].key=key;bindings[action].modifiers=modifiers;custom[action]=1;}
        seen[action]=1;
    }
    if(ferror(file))binding_error("Error reading config file; some entries may not have been loaded.");
    free(line);fclose(file);
    /* Reversion may expose another collision with a restored default. */
    for(int pass=0;pass<HK_COUNT;pass++) {
        int bad[HK_COUNT]={0},changed=0;
        for(int i=0;i<HK_COUNT;i++)for(int j=i+1;j<HK_COUNT;j++)
            if(bindings[i].key==bindings[j].key && bindings[i].modifiers==bindings[j].modifiers) {
                if(custom[i])bad[i]=1;
                if(custom[j])bad[j]=1;
            }
        for(int i=0;i<HK_COUNT;i++)if(bad[i]) {
            char error[200];snprintf(error,sizeof(error),"conflict for %s; using %s.",bindings[i].name,bindings[i].default_text);
            binding_error(error);custom[i]=0;changed=1;
            binding_parse(bindings[i].default_text,&bindings[i].key,&bindings[i].modifiers);
        }
        if(!changed)break;
    }
}
static int binding_match(KeySym key,unsigned state)
{
    if(key==XK_ISO_Left_Tab)state|=ShiftMask;
    key=normalized_key(key);state&=ControlMask|ShiftMask|Mod1Mask|Mod4Mask;
    for(int i=0;i<HK_COUNT;i++)if(bindings[i].key==key && bindings[i].modifiers==state)return i;
    return -1;
}
static int bindings_command(Tab *t,const char *command)
{
    char text[COMMAND_SIZE];snprintf(text,sizeof(text),"%s",command);char *value=trim_space(text);
    if(strcmp(value,"bindings") && strcmp(value,"bindings reload"))return 0;
    remember_line(t,value);t->partial=0;
    if(!strcmp(value,"bindings reload")) {
        bindings_load();append_output(t,binding_messages);
        remember_line(t,binding_errors?"Bindings reloaded with fallback warnings.":"Bindings reloaded.");
    } else for(int i=0;i<HK_COUNT;i++) {
        char line[200];unsigned mods=bindings[i].modifiers;
        snprintf(line,sizeof(line),"%s=%s%s%s%s%s",bindings[i].name,
                 mods&ControlMask?"Ctrl+":"",mods&ShiftMask?"Shift+":"",
                 mods&Mod1Mask?"Alt+":"",mods&Mod4Mask?"Super+":"",XKeysymToString(bindings[i].key));
        remember_line(t,line);
    }
    history_add(command);t->status=binding_errors?2:0;
    request_send(t,REQUEST_SET_STATUS,NULL);return 1;
}

/* X11 clipboard implementation. Copy uses dragged output, or the current
 * input when there is no output selection. With empty input, copy scrollback.
 * Large selections use the X11 INCR protocol in both directions. */
typedef struct { Window window;Atom property,target;char *text;size_t length,offset;long long deadline; } ClipboardSend;
static ClipboardSend clipboard_sends[8];
static Atom atom_clipboard,atom_utf8,atom_targets,atom_text,atom_incr,atom_paste;
static char *clipboard_text;
static int paste_pending,paste_tab,paste_incr,paste_overflow,paste_fallback;
static unsigned long paste_generation;
static long long paste_deadline;
static char paste_buffer[COMMAND_SIZE];static size_t paste_used;
static int selection_dragging;
static void selection_bounds(const Tab *t,int *a,int *ac,int *b,int *bc)
{
    *a=t->anchor_line;*ac=t->anchor_col;*b=t->end_line;*bc=t->end_col;
    if(*a>*b || (*a==*b && *ac>*bc)){int v=*a;*a=*b;*b=v;v=*ac;*ac=*bc;*bc=v;}
}
static char *selection_text(Tab *t)
{
    if(!t->selection_active && t->input_length)return strdup(t->input);
    int a=0,ac=0,b=t->line_count-1,bc=b>=0?(int)strlen(t->lines[b]):0;
    if(t->selection_active)selection_bounds(t,&a,&ac,&b,&bc);
    if(a<0 || b>=t->line_count || b<0)return strdup("");
    size_t capacity=(size_t)(b-a+1)*(LINE_SIZE+1)+1;
    char *text=malloc(capacity);if(!text)return NULL;size_t used=0;
    for(int row=a;row<=b;row++) {
        int length=(int)strlen(t->lines[row]);int start=row==a?ac:0,end=row==b?bc:length;
        if(start>length)start=length;
        if(end>length)end=length;
        if(start<0)start=0;
        if(end<start)end=start;
        memcpy(text+used,t->lines[row]+start,(size_t)(end-start));used+=(size_t)(end-start);
        if(row<b)text[used++]='\n';
    }
    text[used]='\0';return text;
}
static void clipboard_copy(Display *d,Window window,Tab *t,Time time,int primary_only)
{
    char *text=selection_text(t);
    if(!text){remember_line(t,"Copy failed: out of memory.");return;}
    free(clipboard_text);clipboard_text=text;
    XSetSelectionOwner(d,XA_PRIMARY,window,time);
    if(!primary_only)XSetSelectionOwner(d,atom_clipboard,window,time);
}
static void clipboard_request(Display *d,Window window,Tab *t,Time time)
{
    if(paste_pending){remember_line(t,"A clipboard request is already pending.");return;}
    if(XGetSelectionOwner(d,atom_clipboard)==None){remember_line(t,"Clipboard is empty.");return;}
    paste_pending=1;paste_tab=(int)(t-tabs);paste_generation=t->generation;
    paste_used=0;paste_incr=0;paste_overflow=0;paste_fallback=0;paste_deadline=watch_now()+10000;
    XDeleteProperty(d,window,atom_paste);
    XConvertSelection(d,atom_clipboard,atom_utf8,atom_paste,window,time);
}
static void paste_complete(void)
{
    if(paste_tab>=0 && paste_tab<MAX_TABS && tabs[paste_tab].used && tabs[paste_tab].generation==paste_generation) {
        Tab *t=&tabs[paste_tab];
        if(paste_overflow)remember_line(t,"Paste rejected: unsupported text or input limit exceeded.");
        else {
            char clean[COMMAND_SIZE];size_t used=0;int invalid=0;
            for(size_t i=0;i<paste_used;i++) {
                unsigned char c=(unsigned char)paste_buffer[i];
                if(c=='\r'){if(i+1<paste_used && paste_buffer[i+1]=='\n')i++;c='\n';}
                if((c>=32 && c<=126) || c=='\t' || c=='\n')clean[used++]=(char)c;
                else invalid=1;
            }
            if(invalid || editor_insert(t,clean,used)==-1)
                remember_line(t,"Paste rejected: use ASCII text within the 8190-byte command limit.");
            /* Pasting never executes commands. Enter is always required. */
        }
    }
    paste_pending=0;paste_incr=0;paste_used=0;
}
static int clipboard_read(Display *d,Window window)
{
    Atom type;int format;unsigned long count,after;unsigned char *data=NULL;
    int result=XGetWindowProperty(d,window,atom_paste,0,COMMAND_SIZE,False,AnyPropertyType,
                                  &type,&format,&count,&after,&data);
    if(result!=Success){if(data)XFree(data);paste_overflow=1;return -1;}
    if(type==atom_incr) {
        paste_incr=1;XFree(data);XDeleteProperty(d,window,atom_paste);return 1;
    }
    if(format!=8 || (type!=atom_utf8 && type!=XA_STRING && type!=atom_text))paste_overflow=1;
    if(after || count>sizeof(paste_buffer)-1-paste_used)paste_overflow=1;
    if(!paste_overflow && count){memcpy(paste_buffer+paste_used,data,count);paste_used+=count;}
    if(data)XFree(data);
    XDeleteProperty(d,window,atom_paste);
    return count?1:0;
}
static void clipboard_reply(Display *d,XSelectionRequestEvent *request)
{
    XEvent reply={0};reply.xselection.type=SelectionNotify;
    reply.xselection.display=d;reply.xselection.requestor=request->requestor;
    reply.xselection.selection=request->selection;reply.xselection.target=request->target;
    reply.xselection.time=request->time;reply.xselection.property=None;
    Atom property=request->property==None?request->target:request->property;
    if(clipboard_text) {
        if(request->target==atom_targets) {
            Atom targets[]={atom_targets,atom_utf8,XA_STRING,atom_text};
            XChangeProperty(d,request->requestor,property,XA_ATOM,32,PropModeReplace,(unsigned char *)targets,4);
            reply.xselection.property=property;
        } else if(request->target==atom_utf8 || request->target==XA_STRING || request->target==atom_text) {
            size_t length=strlen(clipboard_text);
            if(length<=32768) {
                XChangeProperty(d,request->requestor,property,request->target,8,PropModeReplace,
                                (unsigned char *)clipboard_text,(int)length);
                reply.xselection.property=property;
            } else for(int i=0;i<8;i++)if(!clipboard_sends[i].text) {
                ClipboardSend *s=&clipboard_sends[i];s->text=strdup(clipboard_text);if(!s->text)break;
                s->window=request->requestor;s->property=property;s->target=request->target;
                s->length=length;s->offset=0;s->deadline=watch_now()+10000;
                XWindowAttributes attributes;
                if(!XGetWindowAttributes(d,s->window,&attributes)) {
                    free(s->text);memset(s,0,sizeof(*s));break;
                }
                XSelectInput(d,s->window,attributes.your_event_mask|PropertyChangeMask);
                unsigned long amount=(unsigned long)length;
                XChangeProperty(d,s->window,property,atom_incr,32,PropModeReplace,(unsigned char *)&amount,1);
                reply.xselection.property=property;break;
            }
        }
    }
    XSendEvent(d,request->requestor,False,0,&reply);XFlush(d);
}
static int clipboard_event(Display *d,Window window,XEvent *event)
{
    if(event->type==SelectionRequest){clipboard_reply(d,&event->xselectionrequest);return 1;}
    if(event->type==SelectionClear)return 1;
    if(event->type==SelectionNotify && paste_pending && event->xselection.requestor==window &&
       event->xselection.selection==atom_clipboard) {
        if(event->xselection.property==None) {
            if(!paste_fallback){paste_fallback=1;XConvertSelection(d,atom_clipboard,XA_STRING,atom_paste,window,event->xselection.time);}
            else {paste_overflow=1;paste_complete();}
        } else {clipboard_read(d,window);if(!paste_incr)paste_complete();}
        return 1;
    }
    if(event->type==PropertyNotify) {
        XPropertyEvent *p=&event->xproperty;
        if(paste_pending && paste_incr && p->window==window && p->atom==atom_paste && p->state==PropertyNewValue) {
            paste_deadline=watch_now()+10000;
            int more=clipboard_read(d,window);if(more<=0)paste_complete();return 1;
        }
        if(p->state==PropertyDelete)for(int i=0;i<8;i++) {
            ClipboardSend *s=&clipboard_sends[i];
            if(s->text && s->window==p->window && s->property==p->atom) {
                size_t size=s->length-s->offset;if(size>32768)size=32768;
                XChangeProperty(d,s->window,s->property,s->target,8,PropModeReplace,
                                (unsigned char *)s->text+s->offset,(int)size);
                s->offset+=size;s->deadline=watch_now()+10000;
                if(!size){free(s->text);memset(s,0,sizeof(*s));}return 1;
            }
        }
    }
    return 0;
}
static int clipboard_timeout(void)
{
    long long now=watch_now();int active=0;
    if(paste_pending) {
        if(now>=paste_deadline){paste_overflow=1;paste_complete();}
        else active=1;
    }
    for(int i=0;i<8;i++)if(clipboard_sends[i].text) {
        if(now>=clipboard_sends[i].deadline){free(clipboard_sends[i].text);memset(&clipboard_sends[i],0,sizeof(clipboard_sends[i]));}
        else active=1;
    }
    return active?100:-1;
}
/* Clipboard clients may disappear during an asynchronous transfer. A stale
 * requestor is not a reason to terminate the entire terminal application. */
static int gui_xerror(Display *d,XErrorEvent *event)
{
    if(event->error_code==BadWindow || event->error_code==BadMatch || event->error_code==BadAtom)return 0;
    char message[128];XGetErrorText(d,event->error_code,message,sizeof(message));
    if(tabs[active_tab].used)remember_line(&tabs[active_tab],message);
    return 0;
}
static void selection_point(Tab *t,int x,int y,int width,int height,XFontStruct *font,int anchor)
{
    (void)width;int visible=(height-100)/20;if(visible<0)visible=0;
    int end=t->line_count-t->scroll,first=end-visible;if(first<0)first=0;
    int row=first+(y-55)/20;
    if(row<first)row=first;
    if(row>=end)row=end-1;
    if(row<0 || row>=t->line_count)return;
    int column=(x-10)/(font->max_bounds.width?font->max_bounds.width:6);
    if(column<0)column=0;
    int length=(int)strlen(t->lines[row]);if(column>length)column=length;
    if(anchor){t->anchor_line=row;t->anchor_col=column;}
    t->end_line=row;t->end_col=column;
    t->selection_active=t->anchor_line!=row || t->anchor_col!=column;
}

static void redraw(Display *display,Window window,GC gc,XFontStruct *font,
                   int width,int height)
{
    Tab *t=&tabs[active_tab];
    XClearWindow(display,window);
    int x=10;
    for(int i=0;i<MAX_TABS;i++)if(tabs[i].used) {
        char label[40];snprintf(label,sizeof(label),"%s%d%s%s",i==active_tab?"[":" ",
            i+1,tabs[i].busy?"*":"",i==active_tab?"]":" ");
        XDrawString(display,window,gc,x,20,label,(int)strlen(label));x+=65;
    }
    XDrawString(display,window,gc,x,20,"[+]",3);
    XDrawLine(display,window,gc,0,30,width,30);
    char header[PATH_MAX+40];snprintf(header,sizeof(header),"%s | %s",
        "Process mode",t->cwd);
    XDrawString(display,window,gc,10,48,header,(int)strlen(header));
    int visible=(height-100)/20;if(visible<0)visible=0;
    int max_scroll=t->line_count>visible?t->line_count-visible:0;
    if(t->scroll>max_scroll)t->scroll=max_scroll;
    int end=t->line_count-t->scroll,first=end-visible;if(first<0)first=0;
    int y=70;
    for(int i=first;i<end;i++,y+=20) {
        XDrawString(display,window,gc,10,y,t->lines[i],(int)strlen(t->lines[i]));
        if(t->selection_active) {
            int a,ac,b,bc;selection_bounds(t,&a,&ac,&b,&bc);
            if(i>=a && i<=b) {
                int start=i==a?ac:0,stop=i==b?bc:(int)strlen(t->lines[i]);
                int length=(int)strlen(t->lines[i]);if(start>length)start=length;if(stop>length)stop=length;
                if(stop>start) {
                    int left=10+XTextWidth(font,t->lines[i],start),span=XTextWidth(font,t->lines[i]+start,stop-start);
                    XFillRectangle(display,window,gc,left,y-font->ascent,(unsigned)span,(unsigned)(font->ascent+font->descent));
                    XSetForeground(display,gc,WhitePixel(display,DefaultScreen(display)));
                    XDrawString(display,window,gc,left,y,t->lines[i]+start,stop-start);
                    XSetForeground(display,gc,BlackPixel(display,DefaultScreen(display)));
                }
            }
        }
    }
    const char *prefix=t->busy?"[running] ":t->continued_used?"> ":"user@terminator> ";
    char prompt[COMMAND_SIZE+64];snprintf(prompt,sizeof(prompt),"%s%s",prefix,t->input);
    /* Pasted newlines remain real input but are shown as '~' in this one-line
     * editor; tabs are shown as spaces. Paste never submits automatically. */
    for(size_t i=0;prompt[i];i++){if(prompt[i]=='\n')prompt[i]='~';else if(prompt[i]=='\t')prompt[i]=' ';}
    int available=(width-20)/(font->max_bounds.width?font->max_bounds.width:6);
    if(available<2)available=2;
    int length=(int)strlen(prompt),start=0,position=(int)strlen(prefix)+t->cursor;
    if(position>=available)start=position-available+1;
    int shown=length-start;if(shown>available)shown=available;
    int baseline=height-15;
    XDrawString(display,window,gc,10,baseline,prompt+start,shown);
    int cx=10+XTextWidth(font,prompt+start,position-start);
    XDrawLine(display,window,gc,cx,baseline-font->ascent,cx,baseline+font->descent);
    XFlush(display);
}
static void send_stdin(Tab *t,int eof)
{
    char text[COMMAND_SIZE+1];size_t length=(size_t)t->input_length;
    memcpy(text,t->input,length);
    if(!eof)text[length++]='\n';
    text[length]='\0';
    int frames=(int)(length/(INPUT_SIZE-1))+1;
    if(frames+eof>QUEUE_SIZE-t->queue_count) {
        remember_line(t,"Input queue is full; try again after output progresses.");return;
    }
    if(request_send(t,REQUEST_INPUT,text)==-1 || (eof && request_send(t,REQUEST_EOF,NULL)==-1)) {
        remember_line(t,"Could not send command input.");return;
    }
    if(t->input_length)remember_line(t,t->input);
    editor_reset(t);
}
int main(int argc,char **argv)
{
    /* The old explicit process flag is accepted for compatibility. */
    if(argc!=1 && !(argc==2 && !strcmp(argv[1],"useProcess=True"))) {
        fprintf(stderr,"Usage: %s [useProcess=True]\n",argv[0]);return 1;
    }
    signal(SIGPIPE,SIG_IGN);
    descriptor_limit=sysconf(_SC_OPEN_MAX);
    if(descriptor_limit<0)descriptor_limit=1024;
    if(!getcwd(initial_cwd,sizeof(initial_cwd)) || history_init()==-1){perror("startup");return 1;}
    Display *display=XOpenDisplay(NULL);
    if(!display){fprintf(stderr,"Cannot open display. Start XQuartz or your Linux desktop.\n");history_destroy();return 1;}
    XSetErrorHandler(gui_xerror);
    gui_fd=ConnectionNumber(display);
    if(set_flags(gui_fd,0)==-1){perror("X11 descriptor");XCloseDisplay(display);history_destroy();return 1;}
    int screen=DefaultScreen(display),width=900,height=550;
    Window window=XCreateSimpleWindow(display,RootWindow(display,screen),100,100,
        (unsigned)width,(unsigned)height,1,BlackPixel(display,screen),WhitePixel(display,screen));
    XStoreName(display,window,"Terminator - Tabs");
    XSelectInput(display,window,ExposureMask|KeyPressMask|StructureNotifyMask|ButtonPressMask|
                              ButtonReleaseMask|ButtonMotionMask|PropertyChangeMask);
    atom_clipboard=XInternAtom(display,"CLIPBOARD",False);
    atom_utf8=XInternAtom(display,"UTF8_STRING",False);
    atom_targets=XInternAtom(display,"TARGETS",False);
    atom_text=XInternAtom(display,"TEXT",False);
    atom_incr=XInternAtom(display,"INCR",False);
    atom_paste=XInternAtom(display,"TERMINATOR_PASTE",False);
    Atom protocols=XInternAtom(display,"WM_PROTOCOLS",False);
    Atom delete_window=XInternAtom(display,"WM_DELETE_WINDOW",False);
    XSetWMProtocols(display,window,&delete_window,1);
    GC gc=XCreateGC(display,window,0,NULL);
    XSetForeground(display,gc,BlackPixel(display,screen));
    XFontStruct *font=XLoadQueryFont(display,"fixed");
    if(!font || new_tab()==-1){
        fprintf(stderr,"Could not create font or worker.\n");
        if(font)XFreeFont(display,font);
        XFreeGC(display,gc);XDestroyWindow(display,window);XCloseDisplay(display);history_destroy();return 1;
    }
    XSetFont(display,gc,font->fid);XMapWindow(display,window);
    bindings_load();append_output(&tabs[active_tab],binding_messages);
    int running=1,dirty=1;
    while(running && tab_count()) {
        for(int i=0;i<MAX_TABS;i++)if(tabs[i].used)dirty|=collect_replies(i);
        if(!tab_count())break;
        int events=0;
        while(running && XPending(display) && events++<64) {
            XEvent event;XNextEvent(display,&event);
            Tab *t=&tabs[active_tab];
            if(clipboard_event(display,window,&event)){dirty=1;continue;}
            if(event.type==Expose)dirty=1;
            else if(event.type==ConfigureNotify){width=event.xconfigure.width;height=event.xconfigure.height;dirty=1;}
            else if(event.type==ButtonPress){
                if(event.xbutton.button==4)t->scroll+=3;
                else if(event.xbutton.button==5){t->scroll-=3;if(t->scroll<0)t->scroll=0;}
                else if(event.xbutton.button==1 && event.xbutton.y<30){
                    selection_dragging=0;
                    int x=10,selected=-1;
                    for(int i=0;i<MAX_TABS;i++)if(tabs[i].used){
                        if(event.xbutton.x>=x && event.xbutton.x<x+65)selected=i;
                        x+=65;
                    }
                    if(selected>=0)active_tab=selected;
                    else if(event.xbutton.x>=x && event.xbutton.x<x+40){
                        if(new_tab()==-1)remember_line(t,"Cannot open tab (maximum 8 or resource error).");
                    }
                } else if(event.xbutton.button==1 && event.xbutton.y>=55 && event.xbutton.y<height-40) {
                    t->selection_active=0;selection_dragging=1;
                    selection_point(t,event.xbutton.x,event.xbutton.y,width,height,font,1);
                }
                dirty=1;
            } else if(event.type==MotionNotify && selection_dragging) {
                selection_point(t,event.xmotion.x,event.xmotion.y,width,height,font,0);dirty=1;
            } else if(event.type==ButtonRelease && event.xbutton.button==1) {
                if(selection_dragging && t->selection_active)
                    clipboard_copy(display,window,t,event.xbutton.time,1);
                selection_dragging=0;dirty=1;
            } else if(event.type==KeyPress){
                char typed[32];KeySym key;
                int n=XLookupString(&event.xkey,typed,sizeof(typed),&key,NULL);
                unsigned state=event.xkey.state;
                int action=binding_match(key,state);
                if(action==HK_NEW_TAB){
                    if(new_tab()==-1)remember_line(t,"Cannot open tab (maximum 8 or resource error).");
                } else if(action==HK_CLOSE_TAB)close_tab(active_tab);
                else if(action==HK_NEXT_TAB || action==HK_PREV_TAB){
                    int direction=action==HK_PREV_TAB?-1:1;selection_dragging=0;
                    for(int k=1;k<=MAX_TABS;k++){
                        int i=(active_tab+direction*k+MAX_TABS)%MAX_TABS;
                        if(tabs[i].used){active_tab=i;break;}
                    }
                } else if(action==HK_INTERRUPT){
                    if(t->busy)request_send(t,REQUEST_INTERRUPT,NULL);
                    editor_reset(t);t->continued_used=0;t->continued[0]='\0';
                } else if(action==HK_SUSPEND) {
                    if(t->busy)request_send(t,REQUEST_SUSPEND,NULL);
                    editor_reset(t);t->continued_used=0;t->continued[0]='\0';
                } else if(action==HK_COPY)clipboard_copy(display,window,t,event.xkey.time,0);
                else if(action==HK_PASTE)clipboard_request(display,window,t,event.xkey.time);
                else if(action==HK_PAGE_UP)t->scroll+=10;
                else if(action==HK_PAGE_DOWN){t->scroll-=10;if(t->scroll<0)t->scroll=0;}
                else if(action==HK_ENTER){if(t->busy)send_stdin(t,0);else submit(t);}
                else if(action==HK_LEFT){if(t->cursor>0)t->cursor--;}
                else if(action==HK_RIGHT){if(t->cursor<t->input_length)t->cursor++;}
                else if(action==HK_START)t->cursor=0;
                else if(action==HK_END)t->cursor=t->input_length;
                else if(action==HK_PREVIOUS)editor_history(t,-1);
                else if(action==HK_NEXT)editor_history(t,1);
                else if(action==HK_KILL_START)editor_delete(t,0,t->cursor);
                else if(action==HK_KILL_END)editor_delete(t,t->cursor,t->input_length);
                else if(action==HK_WORD)editor_previous_word(t);
                else if(action==HK_BACKSPACE){if(t->cursor>0)editor_delete(t,t->cursor-1,t->cursor);}
                else if(action==HK_DELETE)editor_delete(t,t->cursor,t->cursor+1);
                else if(action==HK_CLEAR){t->line_count=0;t->partial=0;t->scroll=0;t->selection_active=0;}
                else if(action==HK_EOF) {
                    if(t->busy)send_stdin(t,1);
                    else if(!t->input_length)close_tab(active_tab);
                    else editor_delete(t,t->cursor,t->cursor+1);
                }
                else if(!(state&(ControlMask|Mod1Mask|Mod4Mask))){
                    for(int j=0;j<n;j++)if((unsigned char)typed[j]>=32 && (unsigned char)typed[j]<=126){
                        if(editor_insert(t,typed+j,1)==-1)XBell(display,0);
                        t->selection_active=0;
                    }
                }
                dirty=1;
            } else if(event.type==ClientMessage && event.xclient.message_type==protocols &&
                event.xclient.format==32 && (Atom)event.xclient.data.l[0]==delete_window)running=0;
            if(!tab_count())break;
        }
        if(!running || !tab_count())break;
        int paste_was_pending=paste_pending;
        int timeout=clipboard_timeout();
        if(paste_was_pending && !paste_pending)dirty=1;
        if(dirty){redraw(display,window,gc,font,width,height);dirty=0;}
        struct pollfd fds[2*MAX_TABS+1];int count=1;
        fds[0]=(struct pollfd){gui_fd,POLLIN,0};
        for(int i=0;i<MAX_TABS;i++)if(tabs[i].used) {
            fds[count++]=(struct pollfd){tabs[i].reply_fd,POLLIN,0};
            if(tabs[i].queue_count)fds[count++]=(struct pollfd){tabs[i].request_fd,POLLOUT,0};
        }
        if(poll(fds,(nfds_t)count,XPending(display)?0:timeout)==-1 && errno!=EINTR)running=0;
        if(fds[0].revents&(POLLERR|POLLHUP|POLLNVAL))running=0;
    }
    for(int i=0;i<MAX_TABS;i++)if(tabs[i].used)close_tab(i);
    free(clipboard_text);
    for(int i=0;i<8;i++)free(clipboard_sends[i].text);
    XFreeGC(display,gc);XFreeFont(display,font);XDestroyWindow(display,window);XCloseDisplay(display);
    history_destroy();return 0;
}
#endif
