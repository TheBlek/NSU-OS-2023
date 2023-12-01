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
#include <termios.h>

#define JOBS_BUFFER_SIZE 128

struct command cmds[MAXCMDS];
char bkgrnd;

// TODO(theblek): make this a linked list of jobs by encoding next free as a negative number
static pid_t jobs[JOBS_BUFFER_SIZE] = {0};
static int next_job = 0;
static int current_job = -1;

static pid_t shell_pgid;
static int shell_terminal;

void execute_command(int id) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);

    pid_t pid = getpid();
    if (setpgid(pid, pid) != 0) {
        perror("Failed to set child process group. from child");    
    }
    if (!bkgrnd && tcsetpgrp(shell_terminal, pid) != 0) {
        perror("Failed to set new pg a foreground process group. from child");
    }
    if (cmds[id].cmdflag & (OUTFILE | OUTFILEAP)) {
        int out;
        if (cmds[id].cmdflag & OUTFILE)
            out = open(cmds[id].outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH); else
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

int add_job(pid_t job) {
    if (next_job == -1) {
        printf("Out of job slots");
        return -1;
    }
    int job_id = next_job;
    jobs[job_id] = job;
    // Find next free job handle
    next_job++;
    while (jobs[next_job] != 0 && next_job != job_id) {
        next_job++;
        if (next_job == JOBS_BUFFER_SIZE) {
            next_job = 0;
        }
    }
    if (next_job == job_id) {
        next_job = -1; // There is no empty slot
    }
    return job_id;
}

int wait_for_job(int id) {
    pid_t pid = jobs[id];
    siginfo_t info;
    if (tcsetpgrp(shell_terminal, pid) != 0) {
        perror("Failed to set new pg a foreground process group");
    }
    if (waitid(P_PID, pid, &info, WEXITED | WSTOPPED) == -1) {
        perror("Failed to wait for child");
    }
    int exited = 1;
    if (info.si_code == CLD_STOPPED) {
        printf("[%d] %d\n", id, pid);
        kill(pid, SIGCONT);
        exited = 0;
    }
    if (tcsetpgrp(shell_terminal, shell_pgid) != 0) {
        perror("Failed to set shell to foreground");
    }
    return exited;
}

int main() {
    register int i;
    char line[1024];      /*  allow large command lines  */
    int ncmds;
    char prompt[50];      /* shell prompt */

    shell_pgid = getpid();
    shell_terminal = STDIN_FILENO;
    assert(isatty(shell_terminal));

    if (shell_pgid != getpgid(shell_pgid)) {
        if (setpgid(shell_pgid, shell_pgid) != 0) {
            perror("Couldn't put shell into it's own process group");
        }
    }
    if (tcsetpgrp(shell_terminal, shell_pgid) != 0) {
        perror("Failed to take control over terminal");
    }

    sigignore(SIGINT);
    sigignore(SIGQUIT);
    sigignore(SIGTTOU);

    sprintf(prompt,"shell: ");

    while (promptline(prompt, line, sizeof(line)) > 0) {    /* until eof  */
        // Check for completed jobs
        for (int i = 0; i < JOBS_BUFFER_SIZE; i++) {
            if (!jobs[i]) continue;

            siginfo_t info;
            if (waitid(P_PID, jobs[i], &info, WEXITED | WNOHANG) != 0) {
                fprintf(stderr, "Job %d failed\n", jobs[i]);
                perror("Failed to wait for a job");
                continue;
            }

            if (info.si_pid != 0) {
                printf("[%d] %d Finished\n", i, info.si_pid);
                jobs[i] = 0;
                next_job = i;
                if (i == current_job) {
                    current_job = -1;
                }
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
            fprintf(stderr, "bkgrnd = %d\n", bkgrnd);
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
            if (strcmp("fg", cmds[i].cmdargs[0]) == 0) {
                if (current_job < 0) continue;

                if (wait_for_job(current_job)) {
                    jobs[current_job] = 0;
                    current_job = -1;
                }
                continue;
            }
            switch (child = fork()) {
                case -1:
                    perror("Failed to fork");
                    return 1; 
                case 0:
                    /* This is a child process */
                    execute_command(i);
                    break;
                default:
                    /* This is a shell process */
                    if (setpgid(child, child) != 0) {
                        perror("Failed to set child process group");    
                    }
                    if (bkgrnd) {
                        current_job = add_job(child);
                        printf("[%d] %d\n", current_job, child);
                    } else {
                        current_job = add_job(child);
                        if (wait_for_job(current_job)) {
                            jobs[current_job] = 0;
                            current_job = -1;    
                        }
                    }
            }
        }
    }/* close while */
}
