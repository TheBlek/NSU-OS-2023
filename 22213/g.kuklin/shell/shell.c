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
    int ncmds;
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

int wait_for_process(pid_t pid) {
    siginfo_t info;
    pid_t pgid = getpgid(pid);
    if (pgid == -1)
        fail("Failed to get pgid of process");

    if (tcsetpgrp(shell_terminal, pgid) != 0)
        fail("Failed to set new pg a foreground process group");

    if (waitid(P_PID, pid, &info, WEXITED | WSTOPPED) == -1)
        fail("Failed to wait for child");

    if (tcsetpgrp(shell_terminal, shell_pgid) != 0)
        fail("Failed to set shell to foreground");

    return info.si_code == CLD_EXITED ? info.si_status : NOT_AN_EXIT_STATUS;
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
            fail("Failed to open file");    

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
            fail("Failed to redirect output to file");
    }

    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);

    execvp(cmd.cmdargs[0], cmd.cmdargs);
    char message[1024] = "Failed to execute command ";
    fail(strcat(message, cmd.cmdargs[0]));
}

int add_job(const char *buffer, int size, struct command *commands, int ncmds, pid_t process) {
    if (job_count == JOBS_BUFFER_SIZE) {
        printf("Out of job slots");
        return -1;
    }
    int job_id = job_count;
    jobs[job_id].cmds = malloc(sizeof(struct command) * ncmds);
    struct command *to_cmds = jobs[job_id].cmds;
    if (to_cmds == NULL)
        fail("Failed to allocate memory for commands");

    memcpy(to_cmds, commands, sizeof(struct command) * ncmds);
    jobs[job_id].ncmds = ncmds;
    jobs[job_id].buffer = malloc(size + 1);
    jobs[job_id].process = process;
    char *to = jobs[job_id].buffer;
    if (to == NULL)
        fail("Failed to allocate memory for job buffer");

    memcpy(to, buffer, size);
    for (int i = 0; i < ncmds; i++) {
        if (to_cmds[i].infile) {
            to_cmds[i].infile = (to_cmds[i].infile - buffer) + to;
        }
        if (to_cmds[i].outfile) {
            to_cmds[i].outfile = (to_cmds[i].outfile - buffer) + to;
        }
        for (int j = 0; to_cmds[i].cmdargs[j]; j++) {
            to_cmds[i].cmdargs[j] = (to_cmds[i].cmdargs[j] - buffer) + to;
        }
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
    assert(id >= 0);
    assert(id < job_count);
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

int get_job_from_argument(int id) {
    if (job_count == 0) {
        fprintf(stderr, "No jobs to manipulate\n");
        fflush(stderr);
        return -1;
    }

    int job = 0;
    if (cmds[id].cmdargs[1]) {
        if (cmds[id].cmdargs[2]) {
            fprintf(stderr, "Invalid number of arguments\n");
            fflush(stderr);
            return -1;
        }
        int arg = atoi(cmds[id].cmdargs[1]);
        if (arg <= 0 || arg > JOBS_BUFFER_SIZE || job_index[arg - 1] == -1) {
            fprintf(stderr, "Invalid job index\n");
            fflush(stderr);
            return -1;
        }
        job = arg - 1;
    } else {
        /* Find job handle that corresponds with last job in stack */
        /* This should work always and without bounds check, but to be safe */
        while (job < JOBS_BUFFER_SIZE && job_index[job] != job_count - 1)
            job++;
        assert(job_index[job] == job_count - 1);
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
            int handle = get_job_from_argument(j);    
            if (handle == -1) {
                should_continue = 0;
                continue;
            }

            job_t job = jobs[job_index[handle]];
            pid_t pid = job.process;
            for (int i = 0; i < job.ncmds; i++) {
                for (int k = 0; job.cmds[i].cmdargs[k]; k++)
                    printf("%s ", job.cmds[i].cmdargs[k]);
            }
            printf("\n");

            kill(-pid, SIGCONT); // Minus is to send signal to the entire process group
            switch (wait_for_process(pid)) {
                case NOT_AN_EXIT_STATUS:
                    printf("\n[%d] %d Stopped\n", handle + 1, pid);
                    fflush(stdout);
                    break;
                default:
                    remove_job(handle);
            }
            continue;
        }
        if (interactive && strcmp("bg", cmds[j].cmdargs[0]) == 0) {
            int handle = get_job_from_argument(j);    
            if (handle == -1) {
                should_continue = 0;
                continue;
            }

            kill(-jobs[job_index[handle]].process, SIGCONT); // Minus is to send signal to the entire process group
            continue;
        }

        if (!(cmds[j].cmdflag & INPIPE)) {
            pgid = orig_pgid;
            pipe_ends[0] = -1;
            pipe_ends[1] = -1;
        }
        
        int last_pipe[2] = {pipe_ends[0], pipe_ends[1]};
        if (last_pipe[0] != -1) {
            if (close(last_pipe[0]) == -1)
                fail("Failed to close prev input pipe");
            last_pipe[0] = -1;
        }
        if (cmds[j].cmdflag & OUTPIPE) {
            if (pipe(pipe_ends) == -1)
                fail("Failed to open a pipe");
        }

        pid_t child = fork();
        switch (child) {
            case -1: fail("Failed to fork"); break; // Fall-through warning elision
            case 0:
                /* This is a child process */
                run_child(cmds[j], pgid, last_pipe[1], pipe_ends[0]);
                // Control flow should never return here
                assert(0);
            default:
                if (!pgid)
                    pgid = child;
                /* This is a shell process */
                if (setpgid(child, pgid) != 0)
                    fail("Failed to set child process group");    

                if (cmds[j].cmdflag & OUTPIPE)
                    continue;

                if (interactive && tcsetpgrp(shell_terminal, pgid) != 0)
                    fail("Failed to set new pg a foreground process group");

                siginfo_t info;
                int events = interactive ? WEXITED | WSTOPPED : WEXITED;
                if (waitid(P_PID, child, &info, events) == -1)
                    fail("Failed to wait for child");

                if (interactive && tcsetpgrp(shell_terminal, shell_pgid) != 0)
                    fail("Failed to set shell to foreground");

                if (info.si_code == CLD_EXITED) {
                    should_continue = !info.si_status;
                } else if (info.si_code == CLD_STOPPED) {
                    should_continue = 0;
                    int id = add_job(line, sizeof(line), cmds, ncmds, pgid);
                    printf("\n[%d] %d Stopped\n", id + 1, pgid);
                    fflush(stdout);
                } else if (info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED) {
                    should_continue = 0;
                }
        }
        if (last_pipe[1] != -1) {
            if (close(last_pipe[1]) == -1)
                fail("Failed to close prev output pipe");
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
        if (setpgid(shell_pgid, shell_pgid) != 0)
            fail("Couldn't put shell into it's own process group");
    }
    if (tcsetpgrp(shell_terminal, shell_pgid) != 0)
        fail("Failed to take control over terminal");

    sigignore(SIGINT);
    sigignore(SIGQUIT);
    sigignore(SIGTTOU);
    sigignore(SIGTTIN);

    for (int i = 0; i < JOBS_BUFFER_SIZE; i++)
        job_index[i] = -1;

    sprintf(prompt,"shell: ");

    while (promptline(prompt, line, sizeof(line)) > 0) {    /* until eof  */
        // Check for completed jobs
        for (int i = 0; i < JOBS_BUFFER_SIZE; i++) {
            if (job_index[i] == -1) continue;
            job_t job = jobs[job_index[i]];

            siginfo_t info;
            if (waitid(P_PID, job.process, &info, WEXITED | WNOHANG) != 0) {
                fprintf(stderr, "Job %d (process %d) failed\n", i + 1, job.process);
                fail("Failed to wait for a job");
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
                case -1: fail("Failed to fork shell process"); break; // Fall-through warning elision
                case 0: {
                    pid_t self = getpid();
                    if (setpgid(self, self))
                        fail("Failed to set shell's another pgid");

                    if (ncmds > 1) {
                        exit(process_command_sequence(ncmds, 0, self));
                    } else {
                        run_child(cmds[0], self, 0, 0);
                    }
                    assert(0);
                }
                default:
                    if (setpgid(process, process) != 0)
                        fail("Failed to set background task's pgid");
            }

            int id = add_job(line, sizeof(line), cmds, ncmds, process);
            printf("[%d] %d\n", id + 1, process);
            fflush(stdout);
            continue; // Next prompt
        } /* end bkrnd */

        process_command_sequence(ncmds, 1, 0);
    }/* close while */
}
