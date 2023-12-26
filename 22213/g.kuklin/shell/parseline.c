#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include "shell.h"
static char *blankskip(register char *);

int parseline(char *line, struct command_sequence *sqncs) {
    int nargs, ncmds, nsqnc;
    register char *s;
    register int i;
    static char delim[] = " \t|&<>;\n\0";

    /* initialize  */
    nargs = ncmds = nsqnc = 0;
    s = line;
    sqncs[nsqnc].cmds[0].cmdargs[0] = (char *) NULL;
    for (i = 0; i < MAXSQNCS; i++) {
        for (int j = 0; j < MAXCMDS; j++)
            sqncs[i].cmds[j].cmdflag = 0;
        sqncs[i].cnt = 0;
        sqncs[i].background = 0;
    }

    while (*s) {        /* until line has been parsed */
        s = blankskip(s);       /*  skip white space */
        if (!*s) break; /*  done with line */

        /*  handle <, >, |, &, and ;  */
        switch(*s) {
            case '&':
                if (*(s+1) == '&') {
                    ncmds++; // Start reading new command
                    nargs = 0;
                    *s++ = '\0';
                    if (ncmds >= MAXCMDS) {
                        fprintf(stderr, "Too many commands in one sequence\n");
                        return -1;
                    }
                } else {
                    if (nargs == 0) { // Should have read some commands by now
                        fprintf(stderr, "Unexpected \"&\"\n");
                        return -1;
                    }
                    sqncs[nsqnc].background = 1; // Start reading into new command sequence
                    sqncs[nsqnc].cnt = ncmds + 1;
                    nsqnc++;
                    ncmds = 0;
                    nargs = 0;
                    if (nsqnc >= MAXSQNCS) {
                        fprintf(stderr, "Too many command sequences in one prompt\n");
                        return -1;
                    }
                }
                *s++ = '\0';
                break;
            case '>':
                // The last indirection to output is used
                sqncs[nsqnc].cmds[ncmds].cmdflag = (sqncs[nsqnc].cmds[ncmds].cmdflag & ~OUTREDIR);
                if (*(s+1) == '>') {
                    *s++ = '\0';
                    sqncs[nsqnc].cmds[ncmds].cmdflag |= OUTFILEAP;
                } else {
                    sqncs[nsqnc].cmds[ncmds].cmdflag |= OUTFILE;
                }
                *s++ = '\0';
                s = blankskip(s);
                if (!*s) {
                    fprintf(stderr, "Expected filename after output redirection\n");
                    return -1;
                }

                sqncs[nsqnc].cmds[ncmds].outfile = s;
                s = strpbrk(s, delim);
                if (isspace(*s))
                    *s++ = '\0';
                break;
            case '<':
                *s++ = '\0';
                s = blankskip(s);
                if (!*s) {
                    fprintf(stderr, "Expected filename after input redirection\n");
                    return -1;
                }
                if (sqncs[nsqnc].cmds[ncmds].cmdflag & INPIPE) {
                    fprintf(stderr, "Cannot redirect input into the middle of a pipeline\n");
                    return -1;
                }
                sqncs[nsqnc].cmds[ncmds].infile = s;
                sqncs[nsqnc].cmds[ncmds].cmdflag |= INFILE;
                s = strpbrk(s, delim);
                if (isspace(*s))
                    *s++ = '\0';
                break;
            case '|':
                if (nargs == 0) {
                    fprintf(stderr, "No command to the left of the pipe symbol\n");
                    return -1;
                }
                if (sqncs[nsqnc].cmds[ncmds].cmdflag & OUTREDIR) {
                    fprintf(stderr, "Cannot redirect output from the middle of a pipeline\n");
                    return -1;
                }
                sqncs[nsqnc].cmds[ncmds++].cmdflag |= OUTPIPE;
                sqncs[nsqnc].cmds[ncmds].cmdflag |= INPIPE;
                *s++ = '\0';
                nargs = 0;
                break;
            case ';':
                *s++ = '\0';
                sqncs[nsqnc].cnt = ncmds + 1;
                nsqnc++; // Start reading into a new command sequence
                ncmds = 0;
                nargs = 0;
                if (nsqnc >= MAXSQNCS) {
                    fprintf(stderr, "Too many command sequences\n");
                    return -1;
                }
                break;
            default:
                /*  a command argument  */
                if (nargs == 0 && sqncs[nsqnc].background > 0) {
                    fprintf(stderr, "\"&\" is only allowed after the last command in the line\n");
                    return -1;
                }

                sqncs[nsqnc].cmds[ncmds].cmdargs[nargs++] = s;
                sqncs[nsqnc].cmds[ncmds].cmdargs[nargs] = (char *) NULL;
                char *s1 = strpbrk(s, delim);
                if (!s1) { // If no delimeter found, null until the end
                    s1 = s;
                    while (*s1 != '\0')
                        s1++;
                }
                s = s1;
                if (isspace(*s)) {
                    *s++ = '\0';
                }
                break;
            }  /*  close switch  */
    }  /* close while  */

    /*  error check  */
    /*
     *  The only errors that will be checked for are
     *  no command on the right side of a pipe
     *  no command to the left of a pipe is checked above
     */
    if (sqncs[nsqnc].cmds[ncmds].cmdflag & OUTPIPE && nargs == 0) {
        fprintf(stderr, "Outgoing pipe on the last command\n");
        return(-1);
    }
    sqncs[nsqnc].cnt = ncmds + 1;
    if (nargs == 0) // Current command is empty, should not be counted
        nsqnc--;
    return nsqnc + 1;
}
static char *blankskip(register char *s) {
    while (isspace(*s) && *s) ++s;
    return(s);
}
