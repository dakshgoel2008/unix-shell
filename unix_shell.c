#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#define MAX_INPUT_SIZE 1024     // max length of user input
#define MAX_ARG_SIZE 64         // max number of arguments
#define MAX_CMDS 16             // max commands in one pipeline
#define MAX_JOBS 32             // max background jobs tracked at once

typedef struct {
    char *inFile;       // <
    char *outFile;      // > or >>
    char *errFile;      // 2>
    int append;         // 1 if >> was used
} Redirect;

typedef struct {
    char *args[MAX_ARG_SIZE];
    Redirect redirect;
} Command;

typedef struct {
    int used;
    pid_t pids[MAX_CMDS];
    int count;
    int alive;          // processes of this job still running
    char cmdline[MAX_INPUT_SIZE];
} Job;

Job jobs[MAX_JOBS];

void initPrint(){
    printf("Welcome to the Unix Shell!\n");
    printf("Type 'help' to see built-in commands, 'exit' to quit.\n\n");
}

void printPrompt(){
    char cwd[1024];
    if(getcwd(cwd, sizeof(cwd)) == NULL){
        perror("getcwd");
        printf("$ ");
    } else {
        printf("%s$ ", cwd);
    }
    fflush(stdout);
}

// returns 0 on EOF (Ctrl+D), 1 otherwise
int takeInput(char *input){
    errno = 0;
    if(fgets(input, MAX_INPUT_SIZE, stdin) == NULL){
        if(errno == EINTR){     // Ctrl+C at the prompt, treat it as an empty line
            clearerr(stdin);
            input[0] = '\0';
            return 1;
        }
        printf("\n");
        return 0;
    }
    input[strcspn(input, "\n")] = '\0';
    return 1;
}

void handleSigint(int sig){
    (void)sig;
    if(write(STDOUT_FILENO, "\n", 1) < 0){}     // only async-safe calls allowed here
}

void setupSignals(){
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleSigint;
    sigemptyset(&sa.sa_mask);
    // no SA_RESTART, so Ctrl+C interrupts fgets and we get a fresh prompt
    sigaction(SIGINT, &sa, NULL);

    signal(SIGTSTP, SIG_IGN);   // no fg/bg yet, so Ctrl+Z is ignored
}

// removes a trailing '&' from the line, returns 1 if there was one
int checkBackground(char *input){
    int len = strlen(input);
    while(len > 0 && isspace((unsigned char)input[len - 1])) len--;
    if(len == 0 || input[len - 1] != '&') return 0;

    len--;
    while(len > 0 && isspace((unsigned char)input[len - 1])) len--;
    input[len] = '\0';
    return 1;
}

void addJob(pid_t *pids, int count, char *cmdline){
    for(int i = 0; i < MAX_JOBS; i++){
        if(jobs[i].used) continue;

        jobs[i].used = 1;
        jobs[i].count = count;
        jobs[i].alive = count;
        memcpy(jobs[i].pids, pids, count * sizeof(pid_t));
        strcpy(jobs[i].cmdline, cmdline);
        printf("[%d] %d\n", i + 1, pids[count - 1]);
        return;
    }
    // it still runs and gets reaped, it just won't show up in 'jobs'
    fprintf(stderr, "too many background jobs, not tracking this one\n");
}

Job *findJob(pid_t pid){
    for(int i = 0; i < MAX_JOBS; i++){
        if(!jobs[i].used) continue;
        for(int j = 0; j < jobs[i].count; j++){
            if(jobs[i].pids[j] == pid) return &jobs[i];
        }
    }
    return NULL;
}

// collects finished background processes so they don't stay as zombies
// foreground commands are always waited for directly, so this only ever sees background ones
void reapJobs(){
    pid_t pid;
    while((pid = waitpid(-1, NULL, WNOHANG)) > 0){
        Job *job = findJob(pid);
        if(job == NULL) continue;

        job->alive--;
        if(job->alive == 0){
            printf("[%d] Done    %s\n", (int)(job - jobs) + 1, job->cmdline);
            job->used = 0;
        }
    }
}

// splits input on spaces/tabs into args, returns number of args
int parseInput(char *input, char **args){
    int count = 0;
    char *token = strtok(input, " \t");

    while(token != NULL && count < MAX_ARG_SIZE - 1){
        args[count++] = token;
        token = strtok(NULL, " \t");
    }
    args[count] = NULL;     // execvp needs a NULL terminated array
    return count;
}

// removes <, >, >>, 2> and their file names from args and stores them in r
// returns 0 on success, -1 if an operator has no file after it
int parseRedirects(char **args, Redirect *r){
    r->inFile = r->outFile = r->errFile = NULL;
    r->append = 0;

    int j = 0;
    for(int i = 0; args[i] != NULL; i++){
        int isIn = strcmp(args[i], "<") == 0;
        int isOut = strcmp(args[i], ">") == 0;
        int isAppend = strcmp(args[i], ">>") == 0;
        int isErr = strcmp(args[i], "2>") == 0;

        if(!isIn && !isOut && !isAppend && !isErr){
            args[j++] = args[i];
            continue;
        }

        char *file = args[i + 1];
        if(file == NULL){
            fprintf(stderr, "syntax error: expected file after '%s'\n", args[i]);
            return -1;
        }

        if(isIn) r->inFile = file;
        if(isOut || isAppend){
            r->outFile = file;
            r->append = isAppend;
        }
        if(isErr) r->errFile = file;
        i++;    // skip the file name
    }
    args[j] = NULL;
    return 0;
}

// opens file and puts it in place of targetFd (0 = stdin, 1 = stdout, 2 = stderr)
// only called in the child, so on failure we just exit
void redirectFd(char *file, int flags, int targetFd){
    int fd = open(file, flags, 0644);
    if(fd < 0){
        perror(file);
        exit(1);
    }
    dup2(fd, targetFd);
    close(fd);
}

int countChar(char *s, char c){
    int count = 0;
    for(; *s; s++){
        if(*s == c) count++;
    }
    return count;
}

// splits the line on '|' and parses each part into cmds
// returns number of commands, 0 for an empty line, -1 on syntax error
int parseLine(char *input, Command *cmds){
    int pipes = countChar(input, '|');
    char *parts[MAX_CMDS];
    int n = 0;

    char *part = strtok(input, "|");
    while(part != NULL && n < MAX_CMDS){
        parts[n++] = part;
        part = strtok(NULL, "|");
    }
    if(part != NULL){
        fprintf(stderr, "too many commands in pipeline (max %d)\n", MAX_CMDS);
        return -1;
    }

    if(n == 0 && pipes == 0) return 0;
    if(n != pipes + 1){
        fprintf(stderr, "syntax error near '|'\n");
        return -1;
    }

    // parseInput also uses strtok, so it can only run after the loop above is done
    for(int i = 0; i < n; i++){
        if(parseInput(parts[i], cmds[i].args) == 0){
            if(pipes == 0) return 0;    // just an empty line
            fprintf(stderr, "syntax error near '|'\n");
            return -1;
        }
        if(parseRedirects(cmds[i].args, &cmds[i].redirect) != 0) return -1;
        if(cmds[i].args[0] == NULL){
            fprintf(stderr, "syntax error: missing command\n");
            return -1;
        }
    }
    return n;
}

void applyRedirects(Redirect *r){
    if(r->inFile){
        redirectFd(r->inFile, O_RDONLY, STDIN_FILENO);
    }
    if(r->outFile){
        int mode = r->append ? O_APPEND : O_TRUNC;
        redirectFd(r->outFile, O_WRONLY | O_CREAT | mode, STDOUT_FILENO);
    }
    if(r->errFile){
        redirectFd(r->errFile, O_WRONLY | O_CREAT | O_TRUNC, STDERR_FILENO);
    }
}

void shellHelp(){
    printf("Built-in commands:\n");
    printf("  cd [dir]   change directory (no dir = home)\n");
    printf("  pwd        print current directory\n");
    printf("  jobs       list background jobs\n");
    printf("  help       show this message\n");
    printf("  exit       quit the shell\n");
    printf("Anything else is run as a program from your PATH.\n");
    printf("End a command with '&' to run it in the background.\n");
}

void shellJobs(){
    reapJobs();
    for(int i = 0; i < MAX_JOBS; i++){
        if(jobs[i].used){
            printf("[%d] Running    %s &\n", i + 1, jobs[i].cmdline);
        }
    }
}

void shellCd(char **args){
    char *dir = args[1];
    if(dir == NULL){
        dir = getenv("HOME");
    }
    if(dir == NULL || chdir(dir) != 0){
        perror("cd");
    }
}

void shellPwd(){
    char cwd[1024];
    if(getcwd(cwd, sizeof(cwd)) == NULL){
        perror("pwd");
    } else {
        printf("%s\n", cwd);
    }
}

// returns 1 if the command was a built-in and has been handled
int runBuiltin(char **args){
    if(strcmp(args[0], "exit") == 0){
        exit(0);
    }
    if(strcmp(args[0], "cd") == 0){
        shellCd(args);
        return 1;
    }
    if(strcmp(args[0], "pwd") == 0){
        shellPwd();
        return 1;
    }
    if(strcmp(args[0], "help") == 0){
        shellHelp();
        return 1;
    }
    if(strcmp(args[0], "jobs") == 0){
        shellJobs();
        return 1;
    }
    return 0;
}

// runs n commands, connecting the stdout of each one to the stdin of the next
// in the background it returns right away and the job is tracked in the jobs table
void runPipeline(Command *cmds, int n, int background, char *cmdline){
    pid_t pids[MAX_CMDS];
    int started = 0;
    int prevRead = -1;      // read end of the pipe coming from the previous command
    pid_t pgid = 0;         // process group of a background job, 0 until the first child exists

    for(int i = 0; i < n; i++){
        int fds[2];
        int isLast = (i == n - 1);

        if(!isLast && pipe(fds) < 0){
            perror("pipe");
            break;
        }

        pid_t pid = fork();
        if(pid < 0){
            perror("fork");
            if(!isLast){
                close(fds[0]);
                close(fds[1]);
            }
            break;
        }

        if(pid == 0){
            signal(SIGINT, SIG_DFL);

            if(background){
                // own process group, so Ctrl+C in the terminal doesn't reach it
                setpgid(0, pgid);
                // and it shouldn't fight the shell for keyboard input
                if(i == 0) redirectFd("/dev/null", O_RDONLY, STDIN_FILENO);
            }

            if(prevRead != -1){
                dup2(prevRead, STDIN_FILENO);
                close(prevRead);
            }
            if(!isLast){
                close(fds[0]);
                dup2(fds[1], STDOUT_FILENO);
                close(fds[1]);
            }
            // explicit redirects win over the pipe, same as bash
            applyRedirects(&cmds[i].redirect);

            char **args = cmds[i].args;
            if(runBuiltin(args)) exit(0);
            execvp(args[0], args);
            // execvp only returns if it failed
            fprintf(stderr, "%s: command not found\n", args[0]);
            exit(127);
        }

        pids[started++] = pid;

        // set it from the parent too, so it's in place no matter which process runs first
        if(background){
            if(pgid == 0) pgid = pid;
            setpgid(pid, pgid);
        }

        // the parent must close its copies, otherwise the next command never sees EOF
        if(prevRead != -1) close(prevRead);
        if(!isLast){
            close(fds[1]);
            prevRead = fds[0];
        } else {
            prevRead = -1;
        }
    }

    if(prevRead != -1) close(prevRead);

    if(background){
        if(started > 0) addJob(pids, started, cmdline);
        return;
    }

    for(int i = 0; i < started; i++){
        // Ctrl+C interrupts waitpid in the shell too, so keep waiting until the child is gone
        while(waitpid(pids[i], NULL, 0) < 0 && errno == EINTR);
    }
}

int main(){
    char input[MAX_INPUT_SIZE];
    char cmdline[MAX_INPUT_SIZE];
    Command cmds[MAX_CMDS];

    initPrint();
    setupSignals();

    while(1){
        reapJobs();
        printPrompt();
        if(!takeInput(input)) break;

        int background = checkBackground(input);
        strcpy(cmdline, input);     // parseLine cuts up input, keep a copy for the jobs list

        int n = parseLine(input, cmds);
        if(n <= 0) continue;

        // a lone built-in must run in the shell itself, or cd would be lost with the child
        if(!background && n == 1 && runBuiltin(cmds[0].args)) continue;

        runPipeline(cmds, n, background, cmdline);
    }

    return 0;
}
