#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include "shell.h"

// opens file and puts it in place of targetFd (0 = stdin, 1 = stdout, 2 = stderr)
// only called in the child, so on failure we just exit
static void redirectFd(char *file, int flags, int targetFd){
    int fd = open(file, flags, 0644);
    if(fd < 0){
        perror(file);
        exit(1);
    }
    dup2(fd, targetFd);
    close(fd);
}

static void applyRedirects(Redirect *r){
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

// runs n commands, connecting the stdout of each one to the stdin of the next
// returns the exit status of the last command
static int runPipeline(Command *cmds, int n, int background, char *cmdline){
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
