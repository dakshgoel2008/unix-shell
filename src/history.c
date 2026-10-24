#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "shell.h"

static char history[HISTORY_SIZE][MAX_INPUT_SIZE];
static int historyCount = 0;

void addHistory(const char *line){
    if(historyCount == HISTORY_SIZE){
        // drop the oldest entry
        memmove(history[0], history[1], (HISTORY_SIZE - 1) * MAX_INPUT_SIZE);
        historyCount--;
    }
    strcpy(history[historyCount++], line);
}

void printHistory(){
    for(int i = 0; i < historyCount; i++){
        printf("%5d  %s\n", i + 1, history[i]);
    }
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
