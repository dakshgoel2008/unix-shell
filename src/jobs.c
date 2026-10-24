#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include "shell.h"

typedef struct {
    int used;
    pid_t pids[MAX_CMDS];
    int count;
    int alive;          // processes of this job still running
    char cmdline[MAX_INPUT_SIZE];
} Job;

static Job jobs[MAX_JOBS];

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

static Job *findJob(pid_t pid){
    for(int i = 0; i < MAX_JOBS; i++){
        if(!jobs[i].used) continue;
        for(int j = 0; j < jobs[i].count; j++){
            if(jobs[i].pids[j] == pid) return &jobs[i];
        }
    }
    return NULL;
}

// collects finished background processes so they don't stay as zombies
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

void printJobs(){
    reapJobs();
    for(int i = 0; i < MAX_JOBS; i++){
        if(jobs[i].used){
            printf("[%d] Running    %s &\n", i + 1, jobs[i].cmdline);
        }
    }
}
