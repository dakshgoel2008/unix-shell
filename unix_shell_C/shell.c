#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_INPUT_SIZE 1024     // max length of user input
#define MAX_ARG_SIZE 64        // max number of arguments

void initPrint(){
    printf("Welcome to the Unix Shell!\n");
    printf("Type 'exit' to quit.\n");
}

void printDir(){
    char curr_work_dir[1024];
    getcwd(curr_work_dir, sizeof(curr_work_dir));
    if(curr_work_dir == NULL){
        perror("getcwd() error");
    } else {
        printf("%s$ ", curr_work_dir);
    }
}

void takeInput(char *input){
    printf("\n>>> ");   // prompt for user input
    fgets(input, MAX_INPUT_SIZE, stdin); // read user input
    if(input == NULL){
        perror("fgets() error");
        print("\nExiting shell...\n");
        exit(0);
    } else {
        input[strcspn(input, "\n")] = 0; // remove trailing newline character
    }
}

void parseInput(char *input, char **args){
    
}

int main(){
    char input[MAX_INPUT_SIZE]; // buffer to hold user input
    char *args1[MAX_ARG_SIZE], *args2[MAX_ARG_SIZE]; // arrays to hold command arguments

    initPrint();

    while(1){
        printDir();     // curr directory
        takeInput(input); // take user input

        if(strlen(input) == 0) continue;

        char *left, *right;

    }

    return 0;
}