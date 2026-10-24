#ifndef SHELL_H
#define SHELL_H

#include <sys/types.h>

#define MAX_INPUT_SIZE 1024     // max length of user input
#define MAX_ARG_SIZE 64         // max number of arguments
#define MAX_CMDS 16             // max commands in one pipeline
#define MAX_JOBS 32             // max background jobs tracked at once
#define MAX_PARTS 32            // max commands chained with ; && || &
#define MAX_TOKENS 128          // max tokens in one part
#define MAX_WORD_SIZE 4096      // max length of one word after $VAR expansion
#define HISTORY_SIZE 100

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

typedef enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_ERR, T_ERR_TO_OUT } TokenType;

typedef struct {
    TokenType type;
    char *text;         // only set for T_WORD
} Token;

// how a part is connected to the part before it
typedef enum { OP_SEMI, OP_AND, OP_OR } ListOp;

typedef struct {
    char *text;
    ListOp op;
    int background;     // 1 if the part ended with &
} Part;

extern int lastStatus;  // exit status of the last command, used by $? && ||

// parser.c
int isBlank(const char *s);
int splitList(char *line, Part *parts);
int tokenize(const char *p, Token *tokens);
void freeTokens(Token *tokens, int count);
int parsePipeline(Token *tokens, int count, Command *cmds);

// executor.c
void runPart(Part *part);

// builtins.c
int runBuiltin(char **args, int *status);

// jobs.c
void addJob(pid_t *pids, int count, char *cmdline);
void reapJobs(void);
void printJobs(void);

// history.c
void addHistory(const char *line);
int expandHistory(char *line);
void printHistory(void);

#endif
