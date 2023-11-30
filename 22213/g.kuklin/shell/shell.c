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
volatile int should_put_back = 0;

void put_to_background(int); 

void execute_command(int id, int shell_terminal) {
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

// TODO(theblek): make this a linked list of jobs by encoding next free as a negative number
static pid_t jobs[JOBS_BUFFER_SIZE] = {0};

int add_job(pid_t job, int *next_job) {
    if (*next_job == -1) {
        printf("Out of job slots");
        return -1;
    }
    int job_id = *next_job;
    printf("[%d] %d\n", job_id, job);
    jobs[job_id] = job;
    // Find next free job handle
    (*next_job)++;
    while (jobs[*next_job] != 0 && *next_job != job_id) {
        (*next_job)++;
        if (*next_job == JOBS_BUFFER_SIZE) {
            *next_job = 0;
        }
    }
    if (*next_job == job_id) {
        *next_job = -1; // There is no empty slot
    }
    return job_id;
}

int main() {
    register int i;
    char line[1024];      /*  allow large command lines  */
    int ncmds;
    char prompt[50];      /* shell prompt */

    int next_job = 0;
    int current_job = -1;

    pid_t shell_pgid = getpid();
    int shell_terminal = STDIN_FILENO;
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
            siginfo_t child_info;
            if (strcmp("fg", cmds[i].cmdargs[0]) == 0) {
                child = jobs[current_job];
                if (tcsetpgrp(shell_terminal, child) != 0) {
                    perror("Failed to set new pg a foreground process group");
                }
                if (waitid(P_PID, child, &child_info, WEXITED | WSTOPPED) == -1) {
                    perror("Failed to wait for child");
                }
                if (child_info.si_code == CLD_STOPPED) {
                    add_job(child, &next_job);
                    current_job = child;
                    kill(child, SIGCONT);
                }
                if (tcsetpgrp(shell_terminal, shell_pgid) != 0) {
                    perror("Failed to set shell to foreground");
                }
                jobs[current_job] = 0;
                current_job = -1;
                continue;
            }
            switch (child = fork()) {
                case -1:
                    perror("Failed to fork");
                    return 1; 
                case 0:
                    /* This is a child process */
                    execute_command(i, shell_terminal);
                    break;
                default:
                    /* This is a shell process */
                    if (setpgid(child, child) != 0) {
                        perror("Failed to set child process group");    
                    }
                    if (bkgrnd) {
                        current_job = add_job(child, &next_job);
                    } else {
                        if (tcsetpgrp(shell_terminal, child) != 0) {
                            perror("Failed to set new pg a foreground process group");
                        }
                        if (waitid(P_PID, child, &child_info, WEXITED | WSTOPPED) == -1) {
                            perror("Failed to wait for child");
                        }
                        if (child_info.si_code == CLD_STOPPED) {
                            current_job = add_job(child, &next_job);
                            kill(child, SIGCONT);
                        }
                        if (tcsetpgrp(shell_terminal, shell_pgid) != 0) {
                            perror("Failed to set shell to foreground");
                        }
                    }
            }
        }
    }/* close while */
}
