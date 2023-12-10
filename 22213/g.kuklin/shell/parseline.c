#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include "shell.h"
static char *blankskip(register char *);

int parseline(char *line) {
    int nargs, ncmds;
    register char *s;
    int rval;
    register int i;
    static char delim[] = " \t|&<>;\n\0";

    /* initialize  */
    bkgrnd = nargs = ncmds = rval = 0;
    s = line;
    cmds[0].cmdargs[0] = (char *) NULL;
    for (i = 0; i < MAXCMDS; i++)
        cmds[i].cmdflag = 0;

    while (*s) {        /* until line has been parsed */
        s = blankskip(s);       /*  skip white space */
        if (!*s) break; /*  done with line */

        /*  handle <, >, |, &, and ;  */
        switch(*s) {
            case '&':
                if (*(s+1) == '&') {
                    ++ncmds;
                    nargs = 0;
                    *s++ = '\0';
                } else {
                    if (bkgrnd > 0) {
                        fprintf(stderr, "Only one \"&\" expected\n");
                        return -1;
                    }
                    ++ncmds;
                    nargs = 0;
                    ++bkgrnd;
                }
                *s++ = '\0';
                break;
            case '>':
                // The last indirection to output is used
                cmds[ncmds].cmdflag = (cmds[ncmds].cmdflag & ~OUTREDIR);
                if (*(s+1) == '>') {
                    *s++ = '\0';
                    cmds[ncmds].cmdflag |= OUTFILEAP;
                } else {
                    cmds[ncmds].cmdflag |= OUTFILE;
                }
                *s++ = '\0';
                s = blankskip(s);
                if (!*s) {
                    fprintf(stderr, "Expected filename after output redirection\n");
                    return(-1);
                }

                cmds[ncmds].outfile = s;
                s = strpbrk(s, delim);
                if (isspace(*s))
                    *s++ = '\0';
                break;
            case '<':
                *s++ = '\0';
                s = blankskip(s);
                if (!*s) {
                    fprintf(stderr, "Expected filename after input redirection\n");
                    return(-1);
                }
                if (cmds[ncmds].cmdflag & INPIPE) {
                    fprintf(stderr, "Cannot redirect input into the middle of a pipeline\n");
                    return -1;
                }
                cmds[ncmds].infile = s;
                cmds[ncmds].cmdflag |= INFILE;
                s = strpbrk(s, delim);
                if (isspace(*s))
                    *s++ = '\0';
                break;
            case '|':
                if (nargs == 0) {
                    fprintf(stderr, "No command to the left of the pipe symbol\n");
                    return(-1);
                }
                if (cmds[ncmds].cmdflag & OUTREDIR) {
                    fprintf(stderr, "Cannot redirect output from the middle of a pipeline\n");
                    return -1;
                }
                cmds[ncmds++].cmdflag |= OUTPIPE;
                cmds[ncmds].cmdflag |= INPIPE;
                *s++ = '\0';
                nargs = 0;
                break;
            case ';':
                *s++ = '\0';
                ++ncmds;
                nargs = 0;
                break;
            default:
                /*  a command argument  */
                if (nargs == 0) {
                    rval = ncmds+1; // If this is the first argument - it is a new command
                    if (bkgrnd > 0) {
                        fprintf(stderr, "\"&\" is only allowed after the last command\n");
                        return -1;
                    }
                }

                cmds[ncmds].cmdargs[nargs++] = s;
                cmds[ncmds].cmdargs[nargs] = (char *) NULL;
                char *s1 = strpbrk(s, delim);
                if (!s1) {
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
    if (cmds[ncmds-1].cmdflag & OUTPIPE) {
        if (nargs == 0) {
            fprintf(stderr, "syntax error\n");
            return(-1);
        }
    }
    return(rval);
}
static char *blankskip(register char *s) {
    while (isspace(*s) && *s) ++s;
    return(s);
}
