#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "shell.h"

typedef struct {
    char buf[MAX_WORD_SIZE];
    int len;
} WordBuf;

int isBlank(const char *s){
    while(isspace((unsigned char)*s)) s++;
    return *s == '\0';
}

static char *trim(char *s){
    while(isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while(end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

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

// words longer than MAX_WORD_SIZE get cut off
static void addChar(WordBuf *w, char c){
    if(w->len < MAX_WORD_SIZE - 1) w->buf[w->len++] = c;
}

static void addStr(WordBuf *w, const char *s){
    while(*s) addChar(w, *s++);
}

static int isOperatorChar(char c){
    return c == '|' || c == '<' || c == '>';
}

// p points just after a '$'. appends the variable's value to w
// and returns where the variable name ends
static const char *expandVar(const char *p, WordBuf *w){
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
static const char *readWord(const char *p, WordBuf *w, int *quoted){
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

static void clearCommand(Command *cmd){
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
