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

#define NOT_AN_EXIT_STATUS 256
#define JOBS_BUFFER_SIZE 128
#define MAX_LINE_WIDTH 1024

struct command cmds[MAXCMDS];
char bkgrnd;

typedef struct {
    pid_t process;
    struct command *cmds;
    char *buffer;
} job_t;

// TODO(theblek): make this a linked list of jobs by encoding next free as a negative number
static job_t jobs[JOBS_BUFFER_SIZE] = {0};
static int job_count = 0;

static pid_t shell_pgid;
static int shell_terminal;

static char line[MAX_LINE_WIDTH];      /*  allow large command lines  */

int wait_for_process(pid_t pid) {
    siginfo_t info;
    pid_t pgid = getpgid(pid);
    if (pgid == -1) {
        perror("Failed to get pgid of process");
        exit(1);
    }
    if (tcsetpgrp(shell_terminal, pgid) != 0) {
        perror("Failed to set new pg a foreground process group");
        exit(1);
    }

    if (waitid(P_PID, pid, &info, WEXITED | WSTOPPED) == -1) {
        perror("Failed to wait for child");
        exit(1);
    }
    if (tcsetpgrp(shell_terminal, shell_pgid) != 0) {
        perror("Failed to set shell to foreground");
        exit(1);
    }
    if (info.si_code == CLD_EXITED) {
        return info.si_status;
    }
    return NOT_AN_EXIT_STATUS;
}

void run_child(struct command cmd, pid_t pgid, int prev_pipe, int cur_pipe) {
    if (strcmp("fg", cmd.cmdargs[0]) == 0) {
        fprintf(stderr, "fg: no job control");
        exit(1);
    }
    if (strcmp("bg", cmd.cmdargs[0]) == 0) {
        fprintf(stderr, "bg: no job control");
        exit(1);
    }

    pid_t pid = getpid();
    if (pgid == 0)
        pgid = pid;
    if (setpgid(pid, pgid) != 0) {
        perror("Failed to set child process group. from child");    
        exit(1);
    }
    if (!bkgrnd && tcsetpgrp(shell_terminal, pgid) != 0) {
        perror("Failed to set new pg a foreground process group. from child");
        exit(1);
    }
    if (cmd.cmdflag & (OUTFILE | OUTFILEAP)) {
        int out;
        if (cmd.cmdflag & OUTFILE)
            out = open(cmd.outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        else
            out = open(cmd.outfile, O_WRONLY | O_APPEND);

        if (out == -1) {
            perror("Failed to open file");    
            exit(1);
        }
        if (dup2(out, 1) == -1) {
            perror("Failed to redirect output to file");
            exit(1);
        }
    }
    if (cmd.cmdflag & OUTPIPE) {
        if (dup2(cur_pipe, 1) == -1) {
            perror("Failed to redirect output to pipe");
            exit(1);
        }
    }
    if (cmd.cmdflag & INPIPE) {
        if (dup2(prev_pipe, 0) == -1) {
            perror("Failed to redirect output to pipe");
            exit(1);
        }
    }
    if (cmd.cmdflag & INFILE) {
        int in = open(cmd.infile, O_RDONLY);
        if (in == -1) {
            perror("Failed to open file"); 
            exit(1);
        }
        if (dup2(in, 0) == -1) {
            perror("Failed to redirect output to file");
            exit(1);
        }
    }

    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);

    execvp(cmd.cmdargs[0], cmd.cmdargs);
    char message[1024] = "Failed to execute command ";
    perror(strcat(message, cmd.cmdargs[0]));
    exit(1);
}

int add_job(const char *buffer, int size, struct command *commands, int ncmds) {
    if (job_count == JOBS_BUFFER_SIZE) {
        printf("Out of job slots");
        return -1;
    }
    int job_id = job_count;
    jobs[job_id].cmds = commands;
    jobs[job_id].buffer = malloc(size + 1);
    char *to = jobs[job_id].buffer;
    memcpy(to, buffer, size);
    for (int i = 0; i < ncmds; i++) {
        if (commands[i].infile) {
            commands[i].infile = (commands[i].infile - buffer) + to;
        }
        if (commands[i].outfile) {
            commands[i].outfile = (commands[i].outfile - buffer) + to;
        }
        for (int j = 0; commands[i].cmdargs[j]; j++) {
            commands[i].cmdargs[j] = (commands[i].cmdargs[j] - buffer) + to;
        }
    }
    job_count++;
    return job_id;
}

void remove_job(int id) {
    assert(id >= 0);
    assert(id < job_count);
    free(jobs[id].buffer);
    memmove(&jobs[id], &jobs[id+1], sizeof(job_t) * (job_count - id));
    job_count--;
}

int get_job_from_argument(int id) {
    if (job_count == 0) {
        fprintf(stderr, "No jobs to manipulate\n");
        fflush(stderr);
        return -1;
    }

    int job = job_count - 1;
    if (cmds[id].cmdargs[1]) {
        if (cmds[id].cmdargs[2]) {
            fprintf(stderr, "Invalid number of arguments\n");
            fflush(stderr);
            return -1;
        }
        int arg = atoi(cmds[id].cmdargs[1]);
        if (arg <= 0 || arg > job_count) {
            fprintf(stderr, "Invalid job index\n");
            fflush(stderr);
            return -1;
        }
        job = arg - 1;
    }
    fflush(stdout);
    return job;
}

// if pgid is zero commands are non-blocking under this shell
int process_command_sequence(int ncmds, int interactive, int orig_pgid) {
    int should_continue = 1;
    int pipe_ends[2] = {-1, -1};
    int pgid = orig_pgid;
    for (int j = 0; j < ncmds && should_continue; j++) {
        if (interactive && strcmp("fg", cmds[j].cmdargs[0]) == 0) {
            int job = get_job_from_argument(j);    
            if (job == -1)
                should_continue = 0;

            pid_t pid = jobs[job].process;
            kill(-pid, SIGCONT);
            switch (wait_for_process(pid)) {
                case NOT_AN_EXIT_STATUS:
                    printf("\n[%d] %d Stopped\n", job + 1, pid);
                    fflush(stdout);
                    break;
                default:
                    remove_job(job);
            }
            continue;
        }
        if (interactive && strcmp("bg", cmds[j].cmdargs[0]) == 0) {
            int job = get_job_from_argument(j);    
            if (job == -1)
                should_continue = 0;

            kill(-jobs[job].process, SIGCONT);
            continue;
        }

        if (!(cmds[j].cmdflag & INPIPE)) {
            pgid = orig_pgid;
            pipe_ends[0] = -1;
            pipe_ends[1] = -1;
        }
        
        int last_pipe[2] = {pipe_ends[0], pipe_ends[1]};
        if (last_pipe[0] != -1) {
            if (close(last_pipe[0]) == -1) {
                perror("Failed to close prev input pipe");
                fprintf(stderr, "It was %d\n", last_pipe[0]);
                exit(1);
            }
            last_pipe[0] = -1;
        }
        if (cmds[j].cmdflag & OUTPIPE) {
            if (pipe(pipe_ends) == -1) {
                perror("Failed to open a pipe");
                exit(1);
            }
        }

        pid_t child = fork();
        switch (child) {
            case -1:
                perror("Failed to fork");
                exit(1);
            case 0:
                /* This is a child process */
                run_child(cmds[j], pgid, last_pipe[1], pipe_ends[0]);
                // Control flow should never return here
                assert(0);
            default:
                if (!pgid)
                    pgid = child;
                /* This is a shell process */
                if (setpgid(child, pgid) != 0) {
                    perror("Failed to set child process group");    
                    exit(1);
                }

                if (cmds[j].cmdflag & OUTPIPE)
                    continue;

                if (interactive && tcsetpgrp(shell_terminal, pgid) != 0) {
                    perror("Failed to set new pg a foreground process group");
                    exit(1);
                }

                siginfo_t info;
                int events = interactive ? WEXITED | WSTOPPED : WEXITED;
                if (waitid(P_PID, child, &info, events) == -1) {
                    perror("Failed to wait for child");
                    exit(1);
                }

                if (interactive && tcsetpgrp(shell_terminal, shell_pgid) != 0) {
                    perror("Failed to set shell to foreground");
                    exit(1);
                }

                if (info.si_code == CLD_EXITED) {
                    should_continue = !info.si_status;
                } else if (info.si_code == CLD_STOPPED) {
                    should_continue = 0;
                    int id = add_job(line, sizeof(line), cmds, ncmds);
                    jobs[id].process = pgid;
                    printf("\n[%d] %d Stopped\n", id + 1, child);
                    fflush(stdout);
                }
        }
        if (last_pipe[1] != -1) {
            if (close(last_pipe[1]) == -1) {
                perror("Failed to close prev output pipe");
                fprintf(stderr, "It was %d\n", last_pipe[1]);
                exit(1);
            }
            last_pipe[1] = -1;
        }
    } /* close for */
    return should_continue ? 0 : 1;
}

int main() {
    int ncmds;
    char prompt[50];      /* shell prompt */

    shell_pgid = getpid();
    shell_terminal = STDIN_FILENO;
    assert(isatty(shell_terminal));

    if (shell_pgid != getpgid(shell_pgid)) {
        if (setpgid(shell_pgid, shell_pgid) != 0) {
            perror("Couldn't put shell into it's own process group");
            exit(1);
        }
    }
    if (tcsetpgrp(shell_terminal, shell_pgid) != 0) {
        perror("Failed to take control over terminal");
        exit(1);
    }

    sigignore(SIGINT);
    sigignore(SIGQUIT);
    sigignore(SIGTTOU);
    sigignore(SIGTTIN);

    sprintf(prompt,"shell: ");

    while (promptline(prompt, line, sizeof(line)) > 0) {    /* until eof  */
        // Check for completed jobs
        for (int i = 0; i < job_count; i++) {
            siginfo_t info;
            if (waitid(P_PID, jobs[i].process, &info, WEXITED | WNOHANG) != 0) {
                fprintf(stderr, "Job %d (process %d) failed\n", i + 1, jobs[i].process);
                perror("Failed to wait for a job");
                exit(1);
            }

            if (info.si_code == CLD_EXITED) {
                printf("[%d] %d Finished. Exit code: %d\n", i+1, info.si_pid, info.si_status);
                remove_job(i);
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
                fprintf(stderr, "cmds[%d].cmdflag = %x\n", i, cmds[i].cmdflag);
            }
        }
        #endif

        if (ncmds == 0)
            continue;

        if (bkgrnd) {
            pid_t process = fork();
            switch (process) {
                case -1:
                    perror("Failed to fork shell process");
                    exit(1);
                case 0: {
                    pid_t self = getpid();
                    if (setpgid(self, self)) {
                        perror("Failed to set shell's another pgid");
                        exit(1);
                    }

                    if (ncmds > 1) {
                        exit(process_command_sequence(ncmds, 0, self));
                    } else {
                        run_child(cmds[0], self, 0, 0);
                    }
                    assert(0);
                }
                default:
                    if (setpgid(process, process) != 0) {
                        perror("Failed to set another shell's pgid");
                        exit(1);
                    }
            }

            int id = add_job(line, sizeof(line), cmds, ncmds);
            jobs[id].process = process;
            printf("[%d] %d\n", id + 1, jobs[id].process);
            fflush(stdout);
            continue; // Next prompt
        } /* end bkrnd */

        process_command_sequence(ncmds, 1, 0);
    }/* close while */
}
