/* Milestone 6: pipelines, file patterns, redirection and substitutions.
 * Build on Linux: cc -std=c11 -Wall -Wextra terminator.c -o terminator -lX11
 * Build on macOS: add -I/opt/X11/include -L/opt/X11/lib
 *                    -Wl,-rpath,/opt/X11/lib
 * Process workers only. Implements the staged scope through pipes.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#ifndef TERMINATOR_HEADLESS_TEST
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
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

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_TABS 8
#define INPUT_SIZE 508
#define COMMAND_SIZE 8192
#define QUEUE_SIZE 64
#define MAX_LINES 1000
#define LINE_SIZE 600
#define HISTORY_SIZE 1000
#define PAYLOAD_SIZE PATH_MAX

enum { REQUEST_COMMAND=1, REQUEST_INTERRUPT, REQUEST_STOP, REQUEST_SET_STATUS, REQUEST_FRAGMENT };
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
static pid_t spawn_shell(Worker *w,const char *command,int last_status,int *output_fd,int *state_fd)
{
    int out[2],state[2];
    if(make_pipe(out,1)==-1)return -1;
    if(make_pipe(state,1)==-1){close(out[0]);close(out[1]);return -1;}
    pid_t pid=fork();
    if(pid==0) {
        close(out[0]);close(state[0]);
        if(setpgid(0,0)==-1 || fchdir(w->directory_fd)==-1)_exit(126);
        struct sigaction action={0};action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);
        sigaction(SIGINT,&action,NULL);sigaction(SIGPIPE,&action,NULL);
        if(dup2(out[1],1)==-1 || dup2(out[1],2)==-1)_exit(126);
        if(out[1]>2)close(out[1]);
        close(w->request_fd);close(w->reply_fd);close(w->directory_fd);
        int fd=open("/dev/null",O_RDONLY);
        if(fd==-1 || dup2(fd,0)==-1)_exit(126);
        if(fd>2)close(fd);
        Shell shell={0};snprintf(shell.cwd,sizeof(shell.cwd),"%s",w->cwd);shell.last_status=last_status;
        shell.history_text=history_snapshots[w->slot].text;
        int status=run_shell(&shell,command,0);
        ShellResult result={0};result.exit_requested=shell.exit_requested;
        snprintf(result.cwd,sizeof(result.cwd),"%s",shell.cwd);
        write_all(state[1],&result,sizeof(result));close(state[1]);
        _exit(status);
    }
    int error=errno;close(out[1]);close(state[1]);
    if(pid==-1){close(out[0]);close(state[0]);errno=error;return -1;}
    setpgid(pid,pid);*output_fd=out[0];*state_fd=state[0];return pid;
}

static void worker_main(Worker *argument)
{
    Worker w=*argument;free(argument);
    pid_t pid=-1,group=-1;
    int output=-1,state=-1,last_status=0,alive=1,stopping=0;
    Request request;size_t request_used=0;
    char command[COMMAND_SIZE];size_t command_used=0;
    int command_overflow=0;
    ShellResult result={0};size_t state_used=0;
    while(alive && !stopping) {
        if(output>=0) {
            for(int batch=0;batch<8;batch++) {
                Reply r={0};r.type=REPLY_OUTPUT;
                ssize_t n=read(output,r.text,sizeof(r.text)-1);
                if(n>0) {
                    r.text[n]='\0';
                    if(write_all(w.reply_fd,&r,sizeof(r))==-1){alive=0;break;}
                } else if(n==0){close(output);output=-1;break;}
                else if(errno==EINTR)continue;
                else if(errno==EAGAIN || errno==EWOULDBLOCK)break;
                else {close(output);output=-1;break;}
            }
        }
        if(state>=0) {
            char discard[64];
            void *destination=state_used<sizeof(result)?(char *)&result+state_used:discard;
            size_t amount=state_used<sizeof(result)?sizeof(result)-state_used:sizeof(discard);
            ssize_t n=read(state,destination,amount);
            if(n>0){if(state_used<sizeof(result))state_used+=(size_t)n;}
            else if(n==0){close(state);state=-1;}
            else if(errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK){close(state);state=-1;}
        }
        if(pid>0) {
            int status=0;pid_t waited=waitpid(pid,&status,WNOHANG);
            if(waited==pid) {
                last_status=WIFEXITED(status)?WEXITSTATUS(status):
                            WIFSIGNALED(status)?128+WTERMSIG(status):1;
                pid=-1;
            } else if(waited==-1 && errno!=EINTR){pid=-1;last_status=1;}
        }
        if(group>0 && pid==-1 && output==-1 && state==-1) {
            group=-1;
            if(state_used==sizeof(result)) {
                result.cwd[PATH_MAX-1]='\0';
                if(strcmp(result.cwd,w.cwd)) {
                    if(change_directory(&w,result.cwd)==-1){worker_error(&w,"cd");last_status=1;}
                    else if(reply_send(&w,REPLY_CWD,0,w.cwd)==-1)alive=0;
                }
                if(result.exit_requested)break;
            }
            if(reply_send(&w,REPLY_DONE,last_status,NULL)==-1)alive=0;
        }
        if(!alive)break;
        struct pollfd fds[3]={{w.request_fd,POLLIN,0},{output,POLLIN,0},{state,POLLIN,0}};
        if(poll(fds,3,pid>0 || output>=0 || state>=0?100:-1)==-1) {
            if(errno==EINTR)continue;
            break;
        }
        if(fds[0].revents&(POLLIN|POLLHUP|POLLERR)) {
            ssize_t n=read(w.request_fd,(char *)&request+request_used,sizeof(request)-request_used);
            if(n==0)break;
            if(n<0){if(errno==EINTR || errno==EAGAIN)continue;break;}
            request_used+=(size_t)n;
            if(request_used<sizeof(request))continue;
            request_used=0;request.text[INPUT_SIZE-1]='\0';
            if(request.type==REQUEST_STOP){stopping=1;continue;}
            if(request.type==REQUEST_INTERRUPT){if(group>0)kill(-group,SIGINT);continue;}
            if(group>0)continue;
            if(request.type==REQUEST_SET_STATUS) {
                last_status=0;
                if(reply_send(&w,REPLY_DONE,0,NULL)==-1)break;
                continue;
            }
            if(request.type!=REQUEST_FRAGMENT && request.type!=REQUEST_COMMAND)continue;
            size_t length=strlen(request.text);
            if(command_used+length>=sizeof(command))command_overflow=1;
            else if(!command_overflow){memcpy(command+command_used,request.text,length);command_used+=length;}
            if(request.type==REQUEST_FRAGMENT)continue;
            command[command_used]='\0';
            if(command_overflow) {
                reply_send(&w,REPLY_OUTPUT,0,"terminator: command is too long\n");last_status=2;
            } else {
                memset(&result,0,sizeof(result));state_used=0;
                pid=spawn_shell(&w,command,last_status,&output,&state);
                if(pid>0) {
                    group=pid;
                    if(reply_send(&w,REPLY_STARTED,(int)group,NULL)==-1)break;
                } else {worker_error(&w,"fork/pipe");last_status=1;}
            }
            command_used=0;command_overflow=0;
            if(group<0 && reply_send(&w,REPLY_DONE,last_status,NULL)==-1)break;
        }
    }
    if(group>0)kill(-group,SIGKILL);
    if(output>=0)close(output);
    if(state>=0)close(state);
    if(pid>0)while(waitpid(pid,NULL,0)==-1 && errno==EINTR){}
    close(w.directory_fd);close(w.request_fd);close(w.reply_fd);
}

typedef struct {
    int used,busy,status,request_fd,reply_fd,partial,scroll,input_length;
    pid_t worker_pid, command_group;
    char input[INPUT_SIZE],cwd[PATH_MAX];
    char continued[COMMAND_SIZE];size_t continued_used;
    Request outgoing[QUEUE_SIZE];int queue_head,queue_count;
    char lines[MAX_LINES][LINE_SIZE];
    int line_count;
    Reply incoming; size_t incoming_used;
} Tab;
static Tab tabs[MAX_TABS];
static int active_tab;
static void remember_line(Tab *t,const char *text)
{
    if (t->line_count==MAX_LINES) {
        memmove(t->lines,t->lines+1,(MAX_LINES-1)*sizeof(t->lines[0]));
        t->line_count--;
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
    if(type!=REQUEST_COMMAND && frames!=1){errno=EMSGSIZE;return -1;}
    if(frames>QUEUE_SIZE-t->queue_count){errno=EAGAIN;return -1;}
    errno=0;long limit=fpathconf(t->request_fd,_PC_PIPE_BUF);
    if(limit==-1 && errno)return -1;
    if(limit!=-1 && sizeof(Request)>(size_t)limit){errno=EMSGSIZE;return -1;}
    size_t offset=0;
    for(int i=0;i<frames;i++) {
        int index=(t->queue_head+t->queue_count)%QUEUE_SIZE;
        Request *r=&t->outgoing[index];memset(r,0,sizeof(*r));
        r->type=i==frames-1?type:REQUEST_FRAGMENT;
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
    Tab *t=&tabs[slot]; memset(t,0,sizeof(*t));
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
    remember_line(t,"Ctrl+T: new tab. Ctrl+Tab: switch. Ctrl+W: close. Ctrl+C: interrupt.");
    remember_line(t,"Quotes, substitutions, multiline input, < / >, pipelines | and file patterns work.");
    return slot;
}
static void close_tab(int slot)
{
    Tab *t=&tabs[slot]; if (!t->used)return;
    /* EOF on the request pipe is also a shutdown request. */
    request_send(t,REQUEST_STOP,NULL);
    close(t->request_fd);close(t->reply_fd);
    if(t->command_group>0)kill(-t->command_group,SIGKILL);
    while (waitpid(t->worker_pid,NULL,0)==-1 && errno==EINTR) {}
    memset(t,0,sizeof(*t));
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
static void submit(Tab *t)
{
    if(t->busy || (!t->input_length && !t->continued_used))return;
    char command[COMMAND_SIZE];
    size_t length=t->continued_used+(size_t)t->input_length;
    if(length>=sizeof(command)-1){remember_line(t,"Command limit: 8190 characters. Ctrl+C cancels input.");return;}
    memcpy(command,t->continued,t->continued_used);
    memcpy(command+t->continued_used,t->input,(size_t)t->input_length+1);
    int completeness=command_completeness(command);
    char line[LINE_SIZE];
    snprintf(line,sizeof(line),"%s%s",t->continued_used?"> ":"user@terminator> ",t->input);
    t->partial=0;remember_line(t,line);t->scroll=0;
    if(completeness==1) {
        memcpy(t->continued,command,length);t->continued[length]='\n';
        t->continued[length+1]='\0';t->continued_used=length+1;
        t->input_length=0;t->input[0]='\0';return;
    }
    history_add(command);
    history_snapshot((int)(t-tabs));
    if(request_send(t,REQUEST_COMMAND,command)==0)t->busy=1;
    else {
        snprintf(line,sizeof(line),"Could not send command: %s",strerror(errno));remember_line(t,line);return;
    }
    t->input_length=0;t->input[0]='\0';t->continued_used=0;t->continued[0]='\0';
}

#ifndef TERMINATOR_HEADLESS_TEST
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
    for(int i=first;i<end;i++,y+=20)
        XDrawString(display,window,gc,10,y,t->lines[i],(int)strlen(t->lines[i]));
    char prompt[LINE_SIZE];snprintf(prompt,sizeof(prompt),"%s%s",
        t->busy?"[running] ":t->continued_used?"> ":"user@terminator> ",t->input);
    int available=(width-20)/(font->max_bounds.width?font->max_bounds.width:6);
    int length=(int)strlen(prompt),start=0;
    if(available>1 && length>=available)start=length-available+1;
    int baseline=height-15;
    XDrawString(display,window,gc,10,baseline,prompt+start,length-start);
    int cx=10+XTextWidth(font,prompt+start,length-start);
    XDrawLine(display,window,gc,cx,baseline-font->ascent,cx,baseline+font->descent);
    XFlush(display);
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
    gui_fd=ConnectionNumber(display);
    if(set_flags(gui_fd,0)==-1){perror("X11 descriptor");XCloseDisplay(display);history_destroy();return 1;}
    int screen=DefaultScreen(display),width=900,height=550;
    Window window=XCreateSimpleWindow(display,RootWindow(display,screen),100,100,
        (unsigned)width,(unsigned)height,1,BlackPixel(display,screen),WhitePixel(display,screen));
    XStoreName(display,window,"Terminator - Tabs");
    XSelectInput(display,window,ExposureMask|KeyPressMask|StructureNotifyMask|ButtonPressMask);
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
    int running=1,dirty=1;
    while(running && tab_count()) {
        for(int i=0;i<MAX_TABS;i++)if(tabs[i].used)dirty|=collect_replies(i);
        if(!tab_count())break;
        int events=0;
        while(running && XPending(display) && events++<64) {
            XEvent event;XNextEvent(display,&event);
            Tab *t=&tabs[active_tab];
            if(event.type==Expose)dirty=1;
            else if(event.type==ConfigureNotify){width=event.xconfigure.width;height=event.xconfigure.height;dirty=1;}
            else if(event.type==ButtonPress){
                if(event.xbutton.button==4)t->scroll+=3;
                else if(event.xbutton.button==5){t->scroll-=3;if(t->scroll<0)t->scroll=0;}
                else if(event.xbutton.button==1 && event.xbutton.y<30){
                    int x=10,selected=-1;
                    for(int i=0;i<MAX_TABS;i++)if(tabs[i].used){
                        if(event.xbutton.x>=x && event.xbutton.x<x+65)selected=i;
                        x+=65;
                    }
                    if(selected>=0)active_tab=selected;
                    else if(event.xbutton.x>=x && event.xbutton.x<x+40){
                        if(new_tab()==-1)remember_line(t,"Cannot open tab (maximum 8 or resource error).");
                    }
                }
                dirty=1;
            } else if(event.type==KeyPress){
                char typed[32];KeySym key;
                int n=XLookupString(&event.xkey,typed,sizeof(typed),&key,NULL);
                unsigned state=event.xkey.state;
                if((state&ControlMask) && (key==XK_t || key==XK_T)){
                    if(new_tab()==-1)remember_line(t,"Cannot open tab (maximum 8 or resource error).");
                } else if((state&ControlMask) && (key==XK_w || key==XK_W))close_tab(active_tab);
                else if((state&ControlMask) && (key==XK_Tab || key==XK_ISO_Left_Tab)){
                    int direction=(state&ShiftMask)?-1:1;
                    for(int k=1;k<=MAX_TABS;k++){
                        int i=(active_tab+direction*k+MAX_TABS)%MAX_TABS;
                        if(tabs[i].used){active_tab=i;break;}
                    }
                } else if((state&ControlMask) && (key==XK_c || key==XK_C)){
                    if(t->busy)request_send(t,REQUEST_INTERRUPT,NULL);
                    else {t->input_length=0;t->input[0]='\0';t->continued_used=0;t->continued[0]='\0';}
                } else if(key==XK_Page_Up)t->scroll+=10;
                else if(key==XK_Page_Down){t->scroll-=10;if(t->scroll<0)t->scroll=0;}
                else if(key==XK_Return || key==XK_KP_Enter)submit(t);
                else if(key==XK_BackSpace){if(t->input_length)t->input[--t->input_length]='\0';}
                else if(!(state&(ControlMask|Mod1Mask|Mod4Mask))){
                    for(int j=0;j<n;j++)if((unsigned char)typed[j]>=32 &&
                       (unsigned char)typed[j]<=126 && t->input_length<INPUT_SIZE-1){
                        t->input[t->input_length++]=typed[j];t->input[t->input_length]='\0';
                    }
                }
                dirty=1;
            } else if(event.type==ClientMessage && event.xclient.message_type==protocols &&
                event.xclient.format==32 && (Atom)event.xclient.data.l[0]==delete_window)running=0;
            if(!tab_count())break;
        }
        if(!running || !tab_count())break;
        if(dirty){redraw(display,window,gc,font,width,height);dirty=0;}
        struct pollfd fds[2*MAX_TABS+1];int count=1;
        fds[0]=(struct pollfd){gui_fd,POLLIN,0};
        for(int i=0;i<MAX_TABS;i++)if(tabs[i].used) {
            fds[count++]=(struct pollfd){tabs[i].reply_fd,POLLIN,0};
            if(tabs[i].queue_count)fds[count++]=(struct pollfd){tabs[i].request_fd,POLLOUT,0};
        }
        if(poll(fds,(nfds_t)count,XPending(display)?0:-1)==-1 && errno!=EINTR)running=0;
        if(fds[0].revents&(POLLERR|POLLHUP|POLLNVAL))running=0;
    }
    for(int i=0;i<MAX_TABS;i++)if(tabs[i].used)close_tab(i);
    XFreeGC(display,gc);XFreeFont(display,font);XDestroyWindow(display,window);XCloseDisplay(display);
    history_destroy();return 0;
}
#endif
