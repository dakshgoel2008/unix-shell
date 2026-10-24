#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#define MAX_INPUT_SIZE 1024     // max length of user input
#define MAX_ARG_SIZE 64         // max number of arguments
#define MAX_CMDS 16             // max commands in one pipeline

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
    if(fgets(input, MAX_INPUT_SIZE, stdin) == NULL){
        printf("\n");
        return 0;
    }
    input[strcspn(input, "\n")] = '\0';
    return 1;
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
    printf("  help       show this message\n");
    printf("  exit       quit the shell\n");
    printf("Anything else is run as a program from your PATH.\n");
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
    return 0;
}

// runs n commands, connecting the stdout of each one to the stdin of the next
void runPipeline(Command *cmds, int n){
    pid_t pids[MAX_CMDS];
    int started = 0;
    int prevRead = -1;      // read end of the pipe coming from the previous command

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

    for(int i = 0; i < started; i++){
        waitpid(pids[i], NULL, 0);
    }
}

int main(){
    char input[MAX_INPUT_SIZE];
    Command cmds[MAX_CMDS];

    initPrint();

    while(1){
        printPrompt();
        if(!takeInput(input)) break;

        int n = parseLine(input, cmds);
        if(n <= 0) continue;

        // a lone built-in must run in the shell itself, or cd would be lost with the child
        if(n == 1 && runBuiltin(cmds[0].args)) continue;

        runPipeline(cmds, n);
    }

    return 0;
}
