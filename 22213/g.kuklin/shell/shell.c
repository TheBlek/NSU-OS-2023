#include "shell.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wait.h>

#define JOBS_BUFFER_SIZE 128

struct command cmds[MAXCMDS];
char bkgrnd;

int active_job = -1;

void send_to_active(int); 

void execute_command(int id) {
    if (cmds[id].cmdflag & (OUTFILE | OUTFILEAP)) {
        int out;
        if (cmds[id].cmdflag & OUTFILE)
            out = open(cmds[id].outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        else
            out = open(cmds[id].outfile, O_WRONLY | O_APPEND);

        if (out == -1) {
            perror("Failed to open file");    
            return;
        }
        if (dup2(out, 1) == -1) {
            perror("Failed to redirect output to file"); return;
        }
    }
    if (cmds[id].cmdflag & INFILE) {
        int in = open(cmds[id].infile, O_RDONLY);
        if (in == -1) {
            perror("Failed to open file"); 
            return;
        }
        if (dup2(in, 0) == -1) {
            perror("Failed to redirect output to file");
            return;
        }
    }
    execvp(cmds[id].cmdargs[0], cmds[id].cmdargs);
    char message[1024] = "Failed to execute command ";
    perror(strcat(message, cmds[id].cmdargs[0]));
    exit(1);
}

int main() {
    register int i;
    char line[1024];      /*  allow large command lines  */
    int ncmds;
    char prompt[50];      /* shell prompt */

    // TODO(theblek): make this a linked list of jobs by encoding next free as a negative number
    pid_t jobs[JOBS_BUFFER_SIZE] = {0};
    int next_job = 0;
    int active_job = -1;

    // Register signal handlers
    struct sigaction sigint = {
        .sa_handler = send_to_active,
        .sa_flags = 0,
    };
    if (sigaction(SIGINT, &sigint, NULL) == -1) {
        perror("Failed to install new signal handlers");
        return -1;
    }
    if (sigaction(SIGQUIT, &sigint, NULL) == -1) {
        perror("Failed to install new signal handlers");
        return -1;
    }

    sprintf(prompt,"[shell] ");

    while (promptline(prompt, line, sizeof(line)) > 0) {    /* until eof  */
        // Check for completed jobs
        for (int i = 0; i < JOBS_BUFFER_SIZE; i++) {
            if (!jobs[i]) continue;

            siginfo_t info;
            // A call might be interrupted
            int res = 0;
            while ((res = waitid(P_PID, jobs[i], &info, WEXITED | WNOHANG)) == -1 && errno == EINTR) {}
            if (res != 0) {
                perror("Failed to wait for a job");
                continue;
            }

            if (info.si_pid != 0) {
                printf("[%d] %d Finished\n", i, info.si_pid);
                jobs[i] = 0;
                next_job = i;
            }
        }

        if ((ncmds = parseline(line)) < 0) {
            #ifdef DEBUG
            fprintf(stderr, "Unrecognised command\n");
            #endif
            continue;   /* read next line */
        }
        #ifdef DEBUG
        {
            fprintf(stderr, "ncmds = %d\n", ncmds);
            int i, j;
            for (i = 0; i < ncmds; i++) {
                for (j = 0; cmds[i].cmdargs[j] != (char *) NULL; j++)
                    fprintf(stderr, "cmd[%d].cmdargs[%d] = %s\n", i, j, cmds[i].cmdargs[j]);
                fprintf(stderr, "cmds[%d].cmdflag = %o\n", i, cmds[i].cmdflag);
            }
        }
        #endif

        for (i = 0; i < ncmds; i++) {
            pid_t child;
            siginfo_t child_info;
            switch (child = fork()) {
                case -1:
                    perror("Failed to fork");
                    return 1; 
                case 0:
                    execute_command(i);
                    break;
                default:
                    if (bkgrnd) {
                        if (next_job == -1) {
                            printf("Out of job slots");
                        }
                        printf("[%d] %d\n", next_job, child);
                        jobs[next_job] = child;
                        // Find next free job handle
                        int initial = next_job;
                        next_job++;
                        while (jobs[next_job] != 0 && next_job != initial) {
                            next_job++;
                            if (next_job == JOBS_BUFFER_SIZE) {
                                next_job = 0;
                            }
                        }
                        if (next_job == initial) {
                            next_job = -1; // There is no empty slot
                        }
                    } else {
                        active_job = child;
                        tc
                        if (waitid(P_PID, child, &child_info, WEXITED) == -1 && errno != EINTR) {
                            perror("Failed to wait for child");
                        }
                        active_job = -1;
                    }
            }
        }
    }/* close while */
}

void send_to_active(int signal) {
    if (active_job < 0) {
        return;
    }
    sigsend(P_PID, active_job, signal);
}
