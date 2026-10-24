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
#define MAX_PARTS 32            // max commands chained with ; && || &
#define MAX_TOKENS 128          // max tokens in one part
#define MAX_WORD_SIZE 4096      // max length of one word after $VAR expansion
#define HISTORY_SIZE 100

extern char **environ;

typedef struct {
    char *inFile;       // <
    char *outFile;      // > or >>
    char *errFile;      // 2>
    int append;         // 1 if >> was used
    int errToOut;       // 1 if 2>&1 was used
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

typedef enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_ERR, T_ERR_TO_OUT } TokenType;

typedef struct {
    TokenType type;
    char *text;         // only set for T_WORD
} Token;

typedef struct {
    char buf[MAX_WORD_SIZE];
    int len;
} WordBuf;

// how a part is connected to the part before it
typedef enum { OP_SEMI, OP_AND, OP_OR } ListOp;

typedef struct {
    char *text;
    ListOp op;
    int background;     // 1 if the part ended with &
} Part;

Job jobs[MAX_JOBS];
int lastStatus = 0;     // exit status of the last command, used by $? && ||
char history[HISTORY_SIZE][MAX_INPUT_SIZE];
int historyCount = 0;

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

int isBlank(const char *s){
    while(isspace((unsigned char)*s)) s++;
    return *s == '\0';
}

char *trim(char *s){
    while(isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while(end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

/* ---------- history ---------- */

void addHistory(const char *line){
    if(historyCount == HISTORY_SIZE){
        // drop the oldest entry
        memmove(history[0], history[1], (HISTORY_SIZE - 1) * MAX_INPUT_SIZE);
        historyCount--;
    }
    strcpy(history[historyCount++], line);
}

// replaces "!!" with the last command, and a line "!n" with command number n
// returns 0 if the referenced command doesn't exist
int expandHistory(char *line){
    if(line[0] == '!' && isdigit((unsigned char)line[1])){
        char *end;
        long num = strtol(line + 1, &end, 10);
        if(*end == '\0'){
            if(num < 1 || num > historyCount){
                fprintf(stderr, "!%ld: event not found\n", num);
                return 0;
            }
            strcpy(line, history[num - 1]);
            printf("%s\n", line);   // show what is being run, like bash does
            return 1;
        }
    }

    if(strstr(line, "!!") == NULL) return 1;
    if(historyCount == 0){
        fprintf(stderr, "!!: event not found\n");
        return 0;
    }

    const char *last = history[historyCount - 1];
    char result[MAX_INPUT_SIZE];
    int len = 0;

    for(char *p = line; *p && len < MAX_INPUT_SIZE - 1; ){
        if(p[0] == '!' && p[1] == '!'){
            for(const char *q = last; *q && len < MAX_INPUT_SIZE - 1; q++){
                result[len++] = *q;
            }
            p += 2;
        } else {
            result[len++] = *p++;
        }
    }
    result[len] = '\0';

    strcpy(line, result);
    printf("%s\n", line);
    return 1;
}

/* ---------- splitting the line into parts ---------- */

// splits the line at ;, &&, || and & (when not inside quotes)
// returns the number of parts, or -1 on a syntax error
int splitList(char *line, Part *parts){
    int n = 1;
    char quote = 0;     // the quote char we are inside, or 0

    parts[0].text = line;
    parts[0].op = OP_SEMI;
    parts[0].background = 0;

    for(char *p = line; *p; p++){
        if(quote){
            if(*p == quote) quote = 0;
            else if(quote == '"' && *p == '\\' && p[1]) p++;
            continue;
        }
        if(*p == '\'' || *p == '"'){
            quote = *p;
            continue;
        }
        if(*p == '\\' && p[1]){
            p++;
            continue;
        }

        ListOp next = OP_SEMI;
        int len = 1;

        if(p[0] == '&' && p[1] == '&'){
            next = OP_AND;
            len = 2;
        } else if(p[0] == '|' && p[1] == '|'){
            next = OP_OR;
            len = 2;
        } else if(p[0] == '&' && !(p > line && p[-1] == '>')){     // the & in 2>&1 is not a separator
            parts[n - 1].background = 1;
        } else if(p[0] != ';'){
            continue;
        }

        if(n == MAX_PARTS){
            fprintf(stderr, "too many commands on one line (max %d)\n", MAX_PARTS);
            return -1;
        }
        *p = '\0';
        p += len - 1;
        parts[n].text = p + 1;
        parts[n].op = next;
        parts[n].background = 0;
        n++;
    }

    if(quote){
        fprintf(stderr, "syntax error: unterminated quote\n");
        return -1;
    }

    for(int i = 0; i < n; i++){
        parts[i].text = trim(parts[i].text);
    }

    // a trailing ; or & leaves an empty last part, which is fine
    if(n > 1 && parts[n - 1].text[0] == '\0' && parts[n - 1].op == OP_SEMI) n--;

    for(int i = 0; i < n; i++){
        if(parts[i].text[0] == '\0'){
            fprintf(stderr, "syntax error: missing command\n");
            return -1;
        }
    }
    return n;
}

/* ---------- tokenizer ---------- */

// words longer than MAX_WORD_SIZE get cut off
void addChar(WordBuf *w, char c){
    if(w->len < MAX_WORD_SIZE - 1) w->buf[w->len++] = c;
}

void addStr(WordBuf *w, const char *s){
    while(*s) addChar(w, *s++);
}

int isOperatorChar(char c){
    return c == '|' || c == '<' || c == '>';
}

// p points just after a '$'. appends the variable's value to w
// and returns where the variable name ends
const char *expandVar(const char *p, WordBuf *w){
    if(*p == '?'){
        char num[16];
        snprintf(num, sizeof(num), "%d", lastStatus);
        addStr(w, num);
        return p + 1;
    }

    if(!isalpha((unsigned char)*p) && *p != '_'){
        addChar(w, '$');    // a lone $ stays as it is
        return p;
    }

    char name[128];
    int len = 0;
    while(isalnum((unsigned char)*p) || *p == '_'){
        if(len < (int)sizeof(name) - 1) name[len++] = *p;
        p++;
    }
    name[len] = '\0';

    char *value = getenv(name);
    if(value) addStr(w, value);
    return p;
}

// reads one word starting at p into w, handling quotes, \ escapes, $VAR and ~
// quoted is set if the word had quotes, so "" still counts as an (empty) argument
// returns where the word ends
const char *readWord(const char *p, WordBuf *w, int *quoted){
    w->len = 0;
    *quoted = 0;

    if(p[0] == '~' && (p[1] == '\0' || p[1] == '/' || isspace((unsigned char)p[1]) || isOperatorChar(p[1]))){
        char *home = getenv("HOME");
        if(home){
            addStr(w, home);
            p++;
        }
    }

    while(*p && !isspace((unsigned char)*p) && !isOperatorChar(*p)){
        if(*p == '\''){
            // single quotes: everything inside is literal
            *quoted = 1;
            p++;
            while(*p && *p != '\'') addChar(w, *p++);
            if(*p) p++;
        } else if(*p == '"'){
            // double quotes: $VAR still expands, \ only escapes $ " and \ itself
            *quoted = 1;
            p++;
            while(*p && *p != '"'){
                if(p[0] == '\\' && p[1] && strchr("$\"\\", p[1])){
                    addChar(w, p[1]);
                    p += 2;
                } else if(*p == '$'){
                    p = expandVar(p + 1, w);
                } else {
                    addChar(w, *p++);
                }
            }
            if(*p) p++;
        } else if(p[0] == '\\' && p[1]){
            addChar(w, p[1]);
            p += 2;
        } else if(*p == '$'){
            p = expandVar(p + 1, w);
        } else {
            addChar(w, *p++);
        }
    }

    w->buf[w->len] = '\0';
    return p;
}

void freeTokens(Token *tokens, int count){
    for(int i = 0; i < count; i++){
        free(tokens[i].text);
    }
}

// turns a part of the line into tokens, returns the number of tokens or -1 on error
int tokenize(const char *p, Token *tokens){
    int n = 0;
    WordBuf w;

    while(1){
        while(isspace((unsigned char)*p)) p++;
        if(*p == '\0') break;

        if(n == MAX_TOKENS){
            fprintf(stderr, "command too long (max %d tokens)\n", MAX_TOKENS);
            freeTokens(tokens, n);
            return -1;
        }

        Token *t = &tokens[n];
        t->text = NULL;

        if(strncmp(p, "2>&1", 4) == 0){
            t->type = T_ERR_TO_OUT;
            p += 4;
        } else if(strncmp(p, "2>", 2) == 0){
            t->type = T_ERR;
            p += 2;
        } else if(strncmp(p, ">>", 2) == 0){
            t->type = T_APPEND;
            p += 2;
        } else if(*p == '>'){
            t->type = T_OUT;
            p++;
        } else if(*p == '<'){
            t->type = T_IN;
            p++;
        } else if(*p == '|'){
            t->type = T_PIPE;
            p++;
        } else {
            int quoted;
            p = readWord(p, &w, &quoted);
            if(w.len == 0 && !quoted) continue;     // e.g. $UNSET expands to nothing
            t->type = T_WORD;
            t->text = strdup(w.buf);
        }
        n++;
    }
    return n;
}

/* ---------- parser ---------- */

void clearCommand(Command *cmd){
    cmd->args[0] = NULL;
    cmd->redirect.inFile = NULL;
    cmd->redirect.outFile = NULL;
    cmd->redirect.errFile = NULL;
    cmd->redirect.append = 0;
    cmd->redirect.errToOut = 0;
}

// groups tokens into commands split by '|'
// returns the number of commands, 0 if there were no tokens, -1 on syntax error
int parsePipeline(Token *tokens, int count, Command *cmds){
    int n = 0;
    int argc = 0;
    clearCommand(&cmds[0]);

    for(int i = 0; i < count; i++){
        Token *t = &tokens[i];
        Redirect *r = &cmds[n].redirect;

        if(t->type == T_WORD){
            if(argc == MAX_ARG_SIZE - 1){
                fprintf(stderr, "too many arguments (max %d)\n", MAX_ARG_SIZE - 1);
                return -1;
            }
            cmds[n].args[argc++] = t->text;
            continue;
        }

        if(t->type == T_PIPE){
            if(argc == 0){
                fprintf(stderr, "syntax error near '|'\n");
                return -1;
            }
            cmds[n].args[argc] = NULL;
            n++;
            if(n == MAX_CMDS){
                fprintf(stderr, "too many commands in pipeline (max %d)\n", MAX_CMDS);
                return -1;
            }
            clearCommand(&cmds[n]);
            argc = 0;
            continue;
        }

        if(t->type == T_ERR_TO_OUT){
            r->errToOut = 1;
            continue;
        }

        // every other token is a redirect and needs a file name after it
        if(i + 1 == count || tokens[i + 1].type != T_WORD){
            fprintf(stderr, "syntax error: expected file name after redirect\n");
            return -1;
        }
        char *file = tokens[++i].text;

        if(t->type == T_IN) r->inFile = file;
        if(t->type == T_ERR) r->errFile = file;
        if(t->type == T_OUT || t->type == T_APPEND){
            r->outFile = file;
            r->append = (t->type == T_APPEND);
        }
    }

    if(argc == 0){
        if(count == 0) return 0;
        fprintf(stderr, n > 0 ? "syntax error near '|'\n" : "syntax error: missing command\n");
        return -1;
    }
    cmds[n].args[argc] = NULL;
    return n + 1;
}

/* ---------- redirects ---------- */

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
    // done after stdout is set up, so "cmd > file 2>&1" sends both to the file
    if(r->errToOut){
        dup2(STDOUT_FILENO, STDERR_FILENO);
    }
}

/* ---------- background jobs ---------- */

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

/* ---------- built-ins ---------- */

void shellHelp(){
    printf("Built-in commands:\n");
    printf("  cd [dir]          change directory (no dir = home)\n");
    printf("  pwd               print current directory\n");
    printf("  export NAME=val   set an environment variable (no args = list all)\n");
    printf("  unset NAME        remove an environment variable\n");
    printf("  jobs              list background jobs\n");
    printf("  history           show previous commands (!! = last, !n = number n)\n");
    printf("  help              show this message\n");
    printf("  exit [code]       quit the shell\n");
    printf("Anything else is run as a program from your PATH.\n");
    printf("Supports | < > >> 2> 2>&1, quotes, $VAR, ~, and ; && || & between commands.\n");
}

int shellCd(char **args){
    char *dir = args[1];
    if(dir == NULL){
        dir = getenv("HOME");
    }
    if(dir == NULL || chdir(dir) != 0){
        perror("cd");
        return 1;
    }
    return 0;
}

int shellPwd(){
    char cwd[1024];
    if(getcwd(cwd, sizeof(cwd)) == NULL){
        perror("pwd");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

void shellJobs(){
    reapJobs();
    for(int i = 0; i < MAX_JOBS; i++){
        if(jobs[i].used){
            printf("[%d] Running    %s &\n", i + 1, jobs[i].cmdline);
        }
    }
}

void shellHistory(){
    for(int i = 0; i < historyCount; i++){
        printf("%5d  %s\n", i + 1, history[i]);
    }
}

// variable names are letters, digits and _, not starting with a digit
int isValidName(const char *name){
    if(!isalpha((unsigned char)*name) && *name != '_') return 0;
    for(; *name; name++){
        if(!isalnum((unsigned char)*name) && *name != '_') return 0;
    }
    return 1;
}

int shellExport(char **args){
    if(args[1] == NULL){
        for(char **env = environ; *env; env++){
            printf("%s\n", *env);
        }
        return 0;
    }

    int status = 0;
    for(int i = 1; args[i] != NULL; i++){
        char *eq = strchr(args[i], '=');
        if(eq == NULL) continue;    // "export NAME" has nothing to do, all our variables are exported already

        *eq = '\0';
        if(!isValidName(args[i])){
            fprintf(stderr, "export: '%s': not a valid name\n", args[i]);
            status = 1;
            continue;
        }
        setenv(args[i], eq + 1, 1);
    }
    return status;
}

int shellUnset(char **args){
    for(int i = 1; args[i] != NULL; i++){
        unsetenv(args[i]);
    }
    return 0;
}

// returns 1 if the command was a built-in and has been handled, its exit code goes in status
int runBuiltin(char **args, int *status){
    if(strcmp(args[0], "exit") == 0){
        exit(args[1] ? atoi(args[1]) : lastStatus);
    }

    if(strcmp(args[0], "cd") == 0)          *status = shellCd(args);
    else if(strcmp(args[0], "pwd") == 0)    *status = shellPwd();
    else if(strcmp(args[0], "export") == 0) *status = shellExport(args);
    else if(strcmp(args[0], "unset") == 0)  *status = shellUnset(args);
    else if(strcmp(args[0], "help") == 0){
        shellHelp();
        *status = 0;
    }
    else if(strcmp(args[0], "jobs") == 0){
        shellJobs();
        *status = 0;
    }
    else if(strcmp(args[0], "history") == 0){
        shellHistory();
        *status = 0;
    }
    else return 0;

    return 1;
}

/* ---------- running commands ---------- */

// runs n commands, connecting the stdout of each one to the stdin of the next
// returns the exit status of the last command
// in the background it returns right away and the job is tracked in the jobs table
int runPipeline(Command *cmds, int n, int background, char *cmdline){
    pid_t pids[MAX_CMDS];
    int started = 0;
    int prevRead = -1;      // read end of the pipe coming from the previous command
    pid_t pgid = 0;         // process group of a background job, 0 until the first child exists

    // anything still sitting in our stdout buffer would be copied into the child and printed twice
    fflush(stdout);

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
            int status;
            if(runBuiltin(args, &status)) exit(status);
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
        return 0;
    }

    int status = 1;     // stays 1 if the last command never started
    for(int i = 0; i < started; i++){
        int raw = 0;
        // Ctrl+C interrupts waitpid in the shell too, so keep waiting until the child is gone
        while(waitpid(pids[i], &raw, 0) < 0 && errno == EINTR);

        if(i == n - 1){
            status = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
        }
    }
    return status;
}

// tokenizes, parses and runs one part of the line, updating lastStatus
// this happens part by part, so "export X=1; echo $X" sees the new value
void runPart(Part *part){
    Token tokens[MAX_TOKENS];
    Command cmds[MAX_CMDS];

    int count = tokenize(part->text, tokens);
    if(count < 0){
        lastStatus = 2;
        return;
    }

    int n = parsePipeline(tokens, count, cmds);
    if(n < 0){
        lastStatus = 2;
    } else if(n > 0){
        // a lone built-in must run in the shell itself, or cd would be lost with the child
        if(part->background || n > 1 || !runBuiltin(cmds[0].args, &lastStatus)){
            lastStatus = runPipeline(cmds, n, part->background, part->text);
        }
    }

    freeTokens(tokens, count);
}

int main(){
    char input[MAX_INPUT_SIZE];
    Part parts[MAX_PARTS];

    initPrint();
    setupSignals();

    while(1){
        reapJobs();
        printPrompt();
        if(!takeInput(input)) break;

        if(!expandHistory(input)){
            lastStatus = 1;
            continue;
        }
        if(isBlank(input)) continue;
        addHistory(input);

        int n = splitList(input, parts);
        if(n < 0){
            lastStatus = 2;
            continue;
        }

        for(int i = 0; i < n; i++){
            if(parts[i].op == OP_AND && lastStatus != 0) continue;
            if(parts[i].op == OP_OR && lastStatus == 0) continue;
            runPart(&parts[i]);
        }
    }

    return 0;
}
