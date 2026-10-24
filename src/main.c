#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include "shell.h"

int lastStatus = 0;

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

int main(){
    char input[MAX_INPUT_SIZE];
    Part parts[MAX_PARTS];

    // when reading from a file or pipe there is nobody to show a prompt to
    int interactive = isatty(STDIN_FILENO);

    if(interactive) initPrint();
    setupSignals();

    while(1){
        reapJobs();
        if(interactive) printPrompt();

        if(!takeInput(input)){
            if(interactive) printf("\n");
            break;
        }

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

    return lastStatus;
}
