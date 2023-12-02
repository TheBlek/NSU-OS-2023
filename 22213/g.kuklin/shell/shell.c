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
static int job_count = 0;

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
    if (job_count == JOBS_BUFFER_SIZE) {
        printf("Out of job slots");
        return -1;
    }
    int job_id = job_count;
    jobs[job_id] = job;
    job_count++;
    return job_id;
}

int wait_for_job(int id) {
    assert(id < job_count);

    pid_t pid = jobs[id];
    siginfo_t info;
    if (tcsetpgrp(shell_terminal, pid) != 0) {
        perror("Failed to set new pg a foreground process group");
    }
    if (waitid(P_PID, pid, &info, WEXITED | WSTOPPED) == -1) {
        perror("Failed to wait for child");
    }
    if (tcsetpgrp(shell_terminal, shell_pgid) != 0) {
        perror("Failed to set shell to foreground");
    }
    if (info.si_code == CLD_STOPPED) {
        kill(pid, SIGCONT);
        return 0;
    }
    return 1;
}

void handle_child(pid_t child) {
    if (setpgid(child, child) != 0) {
        perror("Failed to set child process group");    
    }
    int job = add_job(child);
    if (job == -1) return;

    if (bkgrnd) {
        printf("[%d] %d\n", job + 1, child);
        fflush(stdout);
        return;
    }
    if (wait_for_job(job)) {
        job_count--;
    } else {
        printf("\n[%d] Stopped\n", job + 1);
        fflush(stdout);
    }
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
        for (int i = 0; i < job_count; i++) {
            siginfo_t info;
            if (waitid(P_PID, jobs[i], &info, WEXITED | WNOHANG) != 0) {
                fprintf(stderr, "Job %d (process %d) failed\n", i + 1, jobs[i]);
                perror("Failed to wait for a job");
                continue;
            }

            if (info.si_pid == 0) continue;

            printf("[%d] %d Finished\n", i+1, info.si_pid);
            memmove(&jobs[i], &jobs[i+1], sizeof(pid_t) * (job_count - i));
            job_count--;
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
                if (job_count == 0) {
                    fprintf(stderr, "No jobs to put to foreground\n");
                    fflush(stderr);    
                    continue;
                }

                int job = job_count - 1;
                if (cmds[i].cmdargs[1]) {
                    int arg = atoi(cmds[i].cmdargs[1]);
                    if (arg <= 0 || arg > job_count) {
                        fprintf(stderr, "Invalid job index\n");
                        fflush(stderr);
                        continue;
                    }
                    job = arg - 1;
                }

                if (wait_for_job(job)) {
                    job_count--;
                } else {
                    printf("\n[%d] Stopped\n", job + 1);
                    fflush(stdout);
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
                    handle_child(child);
            }
        }
    }/* close while */
}
