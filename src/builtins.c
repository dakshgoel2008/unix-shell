#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include "shell.h"

extern char **environ;

static void shellHelp(){
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

static int shellCd(char **args){
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

static int shellPwd(){
    char cwd[1024];
    if(getcwd(cwd, sizeof(cwd)) == NULL){
        perror("pwd");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

// variable names are letters, digits and _, not starting with a digit
static int isValidName(const char *name){
    if(!isalpha((unsigned char)*name) && *name != '_') return 0;
    for(; *name; name++){
        if(!isalnum((unsigned char)*name) && *name != '_') return 0;
    }
    return 1;
}

static int shellExport(char **args){
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

static int shellUnset(char **args){
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
        printJobs();
        *status = 0;
    }
    else if(strcmp(args[0], "history") == 0){
        printHistory();
        *status = 0;
    }
    else return 0;

    return 1;
}
