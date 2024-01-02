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

typedef struct {
    pid_t process;
    int process_cnt;
    struct command *cmds;
    int ncmds;
    char stopped;
    char *buffer;
} job_t;

static job_t jobs[JOBS_BUFFER_SIZE] = {0};
// TODO(theblek): make this a linked list of jobs by encoding next free as a negative number
// This should be possible because wait takes process groups as negative numbers meaning there are no processes with negative ids
static int job_index[JOBS_BUFFER_SIZE] = {-1};
static int job_count = 0;

static pid_t shell_pgid;
static int shell_terminal;

static char line[MAX_LINE_WIDTH];      /*  allow large command lines  */

void fail(const char *message) {
    perror(message);
    exit(1);
}

void run_child(struct command cmd, pid_t pgid, int prev_pipe, int cur_pipe, char bkgrnd) {
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
    if (setpgid(pid, pgid) != 0)
        fail("Failed to set child process group. from child");    

    if (!bkgrnd && tcsetpgrp(shell_terminal, pgid) != 0)
        fail("Failed to set new pg a foreground process group. from child");
        
    if (cmd.cmdflag & (OUTFILE | OUTFILEAP)) {
        int out;
        if (cmd.cmdflag & OUTFILE)
            out = open(cmd.outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        else
            out = open(cmd.outfile, O_WRONLY | O_APPEND);

        if (out == -1)
            fail("Failed to open output file");    

        if (dup2(out, 1) == -1)
            fail("Failed to redirect output to file");
    }
    if ((cmd.cmdflag & OUTPIPE) && dup2(cur_pipe, 1) == -1)
        fail("Failed to redirect input from a pipe");
    if ((cmd.cmdflag & INPIPE) && dup2(prev_pipe, 0) == -1)
        fail("Failed to redirect output to a pipe");

    if (cmd.cmdflag & INFILE) {
        int in = open(cmd.infile, O_RDONLY);
        if (in == -1)
            fail("Failed to open file"); 
        if (dup2(in, 0) == -1)
            fail("Failed to redirect input from file");
    }

    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);

    execvp(cmd.cmdargs[0], cmd.cmdargs);
    char message[1024] = "Failed to execute command ";
    fail(strcat(message, cmd.cmdargs[0]));
}

int add_job(const char *buffer, int size, struct command *commands, int ncmds, pid_t process, int process_cnt) {
    if (job_count == JOBS_BUFFER_SIZE) {
        fprintf(stderr, "Out of job slots");
        return -1;
    }
    int job_id = job_count;
    jobs[job_id].ncmds = ncmds;
    jobs[job_id].process = process;
    jobs[job_id].process_cnt = process_cnt;
    jobs[job_id].buffer = malloc(size + 1);
    char *to = jobs[job_id].buffer;
    if (to == NULL)
        fail("Failed to allocate memory for job buffer");
    jobs[job_id].cmds = malloc(sizeof(struct command) * ncmds);
    struct command *to_cmds = jobs[job_id].cmds;
    if (to_cmds == NULL)
        fail("Failed to allocate memory for commands");

    memcpy(to_cmds, commands, sizeof(struct command) * ncmds);
    memcpy(to, buffer, size);
    for (int i = 0; i < ncmds; i++) {
        if (to_cmds[i].infile)
            to_cmds[i].infile = (to_cmds[i].infile - buffer) + to;
        if (to_cmds[i].outfile)
            to_cmds[i].outfile = (to_cmds[i].outfile - buffer) + to;
        for (int j = 0; to_cmds[i].cmdargs[j]; j++)
            to_cmds[i].cmdargs[j] = (to_cmds[i].cmdargs[j] - buffer) + to;
    }

    int job_handle = -1;
    for (int i = 0; i < JOBS_BUFFER_SIZE; i++) {
        if (job_index[i] == -1) {
            job_handle = i;
            job_index[job_handle] = job_id;
            break;
        }
    }
    job_count++;
    return job_handle;
}

void remove_job(int handle) {
    int id = job_index[handle];
    assert(id >= 0 && "Negative job id");
    assert(id < job_count && "Out of bounds job id");
    free(jobs[id].buffer);
    free(jobs[id].cmds);
    memmove(&jobs[id], &jobs[id+1], sizeof(job_t) * (job_count - id));
    job_index[handle] = -1;
    for (int i = 0; i < JOBS_BUFFER_SIZE; i++) {
        if (job_index[i] > id)
            job_index[i]--;
    }
    job_count--;
}

int get_job_from_argument(struct command cmd, char stopped) {
    if (job_count == 0) {
        fprintf(stderr, "No jobs to manipulate\n");
        return -1;
    }

    int job = 0;
    if (cmd.cmdargs[1]) {
        int arg = atoi(cmd.cmdargs[1]);
        if (arg <= 0 || arg > JOBS_BUFFER_SIZE || job_index[arg - 1] == -1) {
            fprintf(stderr, "Invalid job index\n");
            return -1;
        }
        job = arg - 1;
    } else {
        int target = job_count - 1;
        // If we need a stopped job (for bg), find latest one
        if (stopped) {
            while (target >= 0 && jobs[target].stopped == 0)
                target--;
            if (target < 0) {
                fprintf(stderr, "No jobs to manipulate\n");
                return -1;
            }
        }

        /* Find job handle that corresponds with target job in stack */
        /* This should work always and without bounds check, but to be safe */
        while (job < JOBS_BUFFER_SIZE && job_index[job] != target)
            job++;
        assert(job_index[job] == target);
    }
    return job;
}

void wait_for_job(int handle, int foreground, int options) {
    job_t *job = &jobs[job_index[handle]];
    pid_t pid = job->process;
    if (foreground && tcsetpgrp(shell_terminal, pid) != 0)
        fail("Failed to set new pg a foreground process group");

    siginfo_t info;
    int exited = 0;
    int count = job->process_cnt > 0 ? job->process_cnt : 1;
    idtype_t type = job->process_cnt > 0 ? P_PGID : P_PID;
    for (int i = 0; i < count; i++) {
        if (waitid(type, pid, &info, options) == -1)
            fail("Failed to wait for child");
        if (info.si_code == CLD_EXITED || info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED)
            exited++;
    }
    if (job->process_cnt == -1)
        job->process_cnt = exited ? 0 : -1;
    else
        job->process_cnt -= exited;

    if (foreground && tcsetpgrp(shell_terminal, shell_pgid) != 0)
        fail("Failed to set shell to foreground");

    if (info.si_code == CLD_STOPPED) {
        jobs[job_index[handle]].stopped = 1;
        printf("\n[%d] %d Stopped\n", handle + 1, info.si_pid);
        fflush(stdout);
    }

    if (jobs[job_index[handle]].process_cnt == 0) {
        if (!foreground)
            printf("[%d] %d Finished. Exit code: %d\n", handle+1, pid, info.si_status);
        remove_job(handle);
    }
}

// if pgid is zero commands are non-blocking under this shell
int process_command_sequence(struct command_sequence sqnc, int interactive, int orig_pgid, int wait) {
    int exit_code = 0;
    int pipe_ends[2] = {-1, -1};
    int pipe_size = 0;
    int pgid = orig_pgid;
    for (int j = 0; j < sqnc.cnt && !exit_code; j++) {
        // Check for internal commands like fg, bg (could be other)
        if (interactive && strcmp("fg", sqnc.cmds[j].cmdargs[0]) == 0) {
            int handle = get_job_from_argument(sqnc.cmds[j], 0);    
            if (handle == -1) {
                exit_code = 1;
                fflush(stderr); // Force error on screen
                continue;
            }

            job_t job = jobs[job_index[handle]];
            pid_t pid = job.process;
            for (int i = 0; i < job.ncmds; i++) {
                for (int k = 0; job.cmds[i].cmdargs[k]; k++)
                    printf("%s ", job.cmds[i].cmdargs[k]);

                if (i + 1 == job.ncmds) continue;
                if (job.cmds[i].cmdflag & OUTPIPE)
                    printf("| ");
                else
                    printf("&& ");
            }
            printf("\n");

            if (job.stopped) {
                kill(-pid, SIGCONT); // Minus is to send signal to the entire process group
                jobs[job_index[handle]].stopped = 0; 
            }

            wait_for_job(handle, 1, WEXITED | WSTOPPED);

            continue;
        }
        if (interactive && strcmp("bg", sqnc.cmds[j].cmdargs[0]) == 0) {
            int handle = get_job_from_argument(sqnc.cmds[j], 1); 
            if (handle == -1) {
                exit_code = 1;
                continue;
            }

            if (jobs[job_index[handle]].stopped) {
                kill(-jobs[job_index[handle]].process, SIGCONT); // Minus is to send signal to the entire process group
                jobs[job_index[handle]].stopped = 0;
            } else {
                fprintf(stderr, "Job %d is already in background\n", handle + 1);
            }
            continue;
        }

        if (sqnc.cmds[j].cmdflag & INPIPE) {
            pipe_size++;
        } else {
            pgid = orig_pgid;
            pipe_size = 1;
            pipe_ends[0] = -1;
            pipe_ends[1] = -1;
        }
        
        int last_pipe[2] = {pipe_ends[0], pipe_ends[1]};
        if (last_pipe[0] != -1) {
            if (close(last_pipe[0]) == -1)
                fail("Failed to close prev input pipe");
            last_pipe[0] = -1;
        }
        if (sqnc.cmds[j].cmdflag & OUTPIPE) {
            if (pipe(pipe_ends) == -1)
                fail("Failed to open a pipe");
        }

        pid_t child = fork();
        switch (child) {
            case -1: fail("Failed to fork"); break; // Fall-through warning elision
            case 0:
                // This is a child process
                run_child(sqnc.cmds[j], pgid, last_pipe[1], pipe_ends[0], !interactive);
                // Control flow should never return here
                assert(0);
            default:
                if (!pgid)
                    pgid = child;
                // This is a shell process
                if (setpgid(child, pgid) != 0)
                    fail("Failed to set child process group");

                if (!(sqnc.cmds[j].cmdflag & OUTPIPE) && !wait) {
                    int id = add_job(line, sizeof(line), &sqnc.cmds[j - pipe_size + 1], pipe_size, pgid, pipe_size);
                    jobs[job_index[id]].stopped = 0;
                    printf("[%d] %d\n", id + 1, pgid);
                }

                if ((sqnc.cmds[j].cmdflag & OUTPIPE) || !wait)
                    continue;

                if (interactive && tcsetpgrp(shell_terminal, pgid) != 0)
                    fail("Failed to set new pg a foreground process group");

                siginfo_t info;
                int events = interactive ? WEXITED | WSTOPPED : WEXITED;
                int finished = 0;
                for (int i = 0; i < pipe_size; i++) {
                    if (waitid(P_PGID, pgid, &info, events) == -1)
                        fail("Failed to wait for child");
                    if (info.si_code == CLD_EXITED || info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED) {
                        finished++;
                        if (info.si_pid == child)
                            exit_code = info.si_status;
                    }
                }
                fflush(stdout);

                if (interactive && tcsetpgrp(shell_terminal, shell_pgid) != 0)
                    fail("Failed to set shell to foreground");

                if (info.si_code == CLD_STOPPED) {
                    exit_code = 1;
                    int id = add_job(line, sizeof(line), &sqnc.cmds[j - pipe_size + 1], pipe_size, pgid, pipe_size - finished);
                    jobs[job_index[id]].stopped = 1;
                    printf("\n[%d] %d Stopped\n", id + 1, pgid);
                    fflush(stdout);
                } else if (info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED) {
                    exit_code = info.si_status;
                }
        }
        if (last_pipe[1] != -1) {
            if (close(last_pipe[1]) == -1)
                fail("Failed to close prev output pipe");
            last_pipe[1] = -1;
        }
    } // close for
    return exit_code;
}

int main() {
    int nsqnc;
    char prompt[50];      /* shell prompt */
    struct command_sequence sqncs[MAXSQNCS];
    for (int i = 0; i < MAXSQNCS; i++) {
        sqncs[i].cmds = malloc(sizeof(struct command) * MAXCMDS); 
        if (!sqncs[i].cmds)
            fail("Failed to allocate memory for commands"); 
    }

    shell_pgid = getpid();
    shell_terminal = STDIN_FILENO;
    assert(isatty(shell_terminal));

    if (shell_pgid != getpgid(shell_pgid)) {
        if (setpgid(shell_pgid, shell_pgid) != 0)
            fail("Couldn't put shell into it's own process group");
    }
    if (tcsetpgrp(shell_terminal, shell_pgid) != 0)
        fail("Failed to take control over terminal");

    sigignore(SIGINT);
    sigignore(SIGQUIT);
    sigignore(SIGTTOU);
    sigignore(SIGTTIN);
    sigignore(SIGTSTP);

    for (int i = 0; i < JOBS_BUFFER_SIZE; i++)
        job_index[i] = -1;

    sprintf(prompt,"shell: ");

    while (promptline(prompt, line, sizeof(line)) > 0) {    /* until eof  */
        if ((nsqnc = parseline(line, sqncs)) < 0) {
            #ifdef DEBUG
            fprintf(stderr, "Unrecognised command\n");
            #endif
            continue;   /* read next line */
        }

        #ifdef DEBUG
        {
            fprintf(stderr, "nsqnc = %d\n", nsqnc);
            for (int k = 0; k < nsqnc; k++) {
                for (int i = 0; i < sqncs[k].cnt; i++) {
                    for (int j = 0; sqncs[k].cmds[i].cmdargs[j] != (char *) NULL; j++)
                        fprintf(stderr, "cmd[%d].cmdargs[%d] = %s\n", i, j, sqncs[k].cmds[i].cmdargs[j]);
                    fprintf(stderr, "cmds[%d].cmdflag = %x\n", i, sqncs[k].cmds[i].cmdflag);
                }
                fprintf(stderr, "bkgrnd = %d\n", sqncs[k].background);
            }
        }
        #endif

        for (int i = 0; i < nsqnc; i++) {
            if (sqncs[i].background) {
                int multiple = 0;
                for (int j = 0; j < sqncs[i].cnt - 1; j++) {
                    if (!(sqncs[i].cmds[j].cmdflag & OUTPIPE)) {
                        multiple = 1;
                        break;
                    }
                }
                if (!multiple && sqncs[i].cnt > 1) {
                    process_command_sequence(sqncs[i], 0, 0, 0);
                    continue;
                }

                pid_t process = fork();
                switch (process) {
                    case -1: fail("Failed to fork shell process"); break; // Fall-through warning elision
                    case 0: {
                        pid_t self = getpid();
                        if (setpgid(self, self))
                            fail("Failed to set shell's another pgid");

                        if (sqncs[i].cnt > 1)
                            exit(process_command_sequence(sqncs[i], 0, self, 1));
                        else
                            run_child(sqncs[i].cmds[0], self, 0, 0, 1);
                        assert(0);
                    }
                    default:
                        if (setpgid(process, process) != 0)
                            fail("Failed to set background task's pgid");
                }

                int id = add_job(line, sizeof(line), sqncs[i].cmds, sqncs[i].cnt, process, -1);
                jobs[job_index[id]].stopped = 0;
                printf("[%d] %d\n", id + 1, process);
                fflush(stdout);
                continue; // Next prompt
            } // end bkrnd

            process_command_sequence(sqncs[i], 1, 0, 1);
        } // close for

        // Check for completed jobs
        for (int i = 0; i < JOBS_BUFFER_SIZE; i++) {
            if (job_index[i] == -1) continue;

            wait_for_job(i, 0, WEXITED | WSTOPPED | WNOHANG);
        }
    } // close while
    for (int i = 0; i < MAXSQNCS; i++)
       free(sqncs[i].cmds);
    return 0;
}
